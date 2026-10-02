#!/usr/bin/env python3
"""Do the C and Python hashers agree, over every ROM in the library?

    make check-rahash

Two implementations of RetroAchievements' per-console hash rules exist because
one has to run on the device and one has to run on a host. That is a
duplication, and this is what stops it being a drift: they are both run over
the same ROMs and every hash has to match.

Getting a rule wrong does not crash anything. It produces a hash RA has never
seen, which looks exactly like a game RA does not know - and there are twelve
of those in this library already, so the wrong answer hides among real ones.
"""
import hashlib
import importlib.util
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build-native", "rahash-check")


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def main():
    rac = load("ra_check", os.path.join(ROOT, "tools", "ra-check.py"))
    roms = sys.argv[1] if len(sys.argv) > 1 else \
        os.path.join(ROOT, "TortOS-Test-Set", "Roms")
    if not os.path.isdir(roms):
        print(f"  skip: no ROM library at {roms}")
        return 0

    systems = rac.systems_from_cfg(os.path.join(ROOT, "config", "systems.cfg"))
    total = mismatched = failed = 0
    for folder, tag, exts in systems:
        d = os.path.join(roms, folder)
        if not os.path.isdir(d):
            continue
        files = [os.path.join(d, f) for f in sorted(os.listdir(d))
                 if not f.startswith(".")
                 and os.path.isfile(os.path.join(d, f))
                 and (not exts or os.path.splitext(f)[1][1:].lower() in exts)]
        if not files:
            continue

        out = subprocess.run([BIN, tag] + files, capture_output=True, text=True)
        got = {}
        for line in out.stdout.splitlines():
            h, _, p = line.partition("\t")
            got[p] = h

        n = 0
        for p in files:
            if tag in ("ARCADE", "NEOGEO"):
                # An arcade set is hashed by its name, not its contents.
                name = os.path.splitext(os.path.basename(p))[0]
                want = hashlib.md5(name.encode()).hexdigest()
            else:
                data = rac.rom_bytes(p)
                if data is None:
                    continue
                want = hashlib.md5(rac.ra_body(tag, data)).hexdigest()
            have = got.get(p)
            total += 1
            n += 1
            if have is None or have == "FAILED":
                failed += 1
                print(f"    C hasher failed: {os.path.basename(p)}")
            elif have != want:
                mismatched += 1
                print(f"    MISMATCH {os.path.basename(p)}\n"
                      f"      C      {have}\n      python {want}")
        print(f"  {folder:<20} {n:>3}")

    print(f"\n  {total} ROMs, {mismatched} mismatched, {failed} unreadable")
    if mismatched or failed:
        return 1
    print("\nok: both hashers agree on every ROM")
    return 0


if __name__ == "__main__":
    sys.exit(main())
