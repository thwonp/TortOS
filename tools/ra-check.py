#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Report which games RetroAchievements recognizes, and which have a set.

    tools/ra-check.py [--roms DIR] [--system NAME] [--verbose]

RA identifies a game by an MD5 over the ROM, but not always over the WHOLE
ROM - each console has its own rule about what to skip. This implements the
rules for the nine systems TortOS ships and asks RA what it makes of each.

Needs no credentials. `r=gameid` is unauthenticated; it does need a
User-Agent, because RA refuses curl's default with `unsupported_client`,
which reads like a permissions problem and is not one.

SEQUENTIALLY, on purpose. A first version ran six requests at a time, got
rate-limited, counted the failures as "no achievements" and reported 48%
coverage. The real figure was 93%. Anything that turns a network failure into
a negative result will eventually tell you a confident lie; failures are
retried here and counted separately.

Measured 2026-08-29 over 180 ROMs: 168 recognized. Of the 12 that are not,
8 are fan translations - romhacks are in no No-Intro-derived database under
those names, so RA has never seen their hashes and never will.
"""
import argparse
import hashlib
import json
import os
import sys
import time
import urllib.parse
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UA = "TortOS/1.0 (+achievement id check)"

# systems.cfg tag -> how much of the ROM RA hashes.
#   NES   an iNES header is 16 bytes of container, not game
#   SFC   copier headers are 512 bytes and only present sometimes, which is
#         what the size test detects
#   PCE   the same idea against a different block size
#   MD/SMS/GG  an SMD-style header, likewise conditional
#   GB/GBC/GBA  the whole file; there is no container to strip
def ra_body(tag, d):
    if tag == "NES":
        return d[16:] if d[:4] == b"NES\x1a" else d
    if tag == "SFC":
        return d[512:] if len(d) % 1024 == 512 else d
    if tag == "PCE":
        return d[512:] if len(d) % 131072 == 512 else d
    if tag in ("MD", "SMS", "GG"):
        return d[512:] if len(d) % 16384 == 512 else d
    return d


def systems_from_cfg(path):
    out = []
    for line in open(path, encoding="utf-8"):
        f = line.rstrip("\n").split("|")
        if len(f) > 7 and f[0] == "sys":
            exts = {e.strip().lower() for e in f[7].split(",") if e.strip()}
            out.append((f[2], f[4], exts))
    return out


def rom_bytes(path):
    """The ROM itself. The library is zipped one game per archive, and RA
    hashes what is inside, not the container.

    THE LARGEST ENTRY, not the first, because that is the one Diatom loads
    (src/zip.c). For one-ROM-per-archive the two are the same file and the
    distinction never shows; for anything else this would hash one game and
    run another, and the achievements would silently be for something else."""
    if path.lower().endswith(".zip"):
        with zipfile.ZipFile(path) as z:
            infos = [i for i in z.infolist() if not i.is_dir() and i.file_size]
            if not infos:
                return None
            return z.read(max(infos, key=lambda i: i.file_size))
    return open(path, "rb").read()


def gameid(md5, tries=3):
    """RA's id for a hash. 0 means RA does not know it; -1 means the question
    could not be asked, which is a different thing and must not be counted as
    an answer."""
    for k in range(tries):
        try:
            body = urllib.parse.urlencode({"r": "gameid", "m": md5}).encode()
            req = urllib.request.Request("https://retroachievements.org/dorequest.php",
                                         data=body, headers={"User-Agent": UA})
            return json.load(urllib.request.urlopen(req, timeout=25)).get("GameID", 0)
        except Exception:
            time.sleep(1.5 * (k + 1))
    return -1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--roms", default=os.path.join(ROOT, "TortOS-Test-Set", "Roms"))
    ap.add_argument("--system")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()

    systems = systems_from_cfg(os.path.join(ROOT, "config", "systems.cfg"))
    if a.system:
        systems = [s for s in systems if s[0] == a.system]
        if not systems:
            sys.exit(f"ra-check: {a.system} is not in systems.cfg")

    tot = hit = failed = 0
    misses = []
    for folder, tag, exts in systems:
        d = os.path.join(a.roms, folder)
        if not os.path.isdir(d):
            continue
        k = n = 0
        for f in sorted(os.listdir(d)):
            p = os.path.join(d, f)
            if f.startswith(".") or not os.path.isfile(p):
                continue
            if exts and os.path.splitext(f)[1][1:].lower() not in exts:
                continue
            data = rom_bytes(p)
            if data is None:
                continue
            n += 1
            g = gameid(hashlib.md5(ra_body(tag, data)).hexdigest())
            if g > 0:
                k += 1
                if a.verbose:
                    print(f"    {g:>6}  {f}")
            elif g < 0:
                failed += 1
                print(f"    ASK FAILED: {folder}/{f}")
            else:
                misses.append((folder, f))
            time.sleep(0.12)          # civility, and it is what keeps this honest
        hit += k
        tot += n
        print(f"  {folder:<20} {k:>3}/{n}")

    if tot:
        print(f"  {'TOTAL':<20} {hit:>3}/{tot}  ({100 * hit // tot}%)"
              f"   unanswered: {failed}")
    tr = [m for m in misses if "Translated" in m[1]]
    if misses:
        print(f"\n  not recognized: {len(misses)}, of which fan translations: {len(tr)}")
        for folder, f in misses:
            if "Translated" not in f:
                print(f"    {folder}: {f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
