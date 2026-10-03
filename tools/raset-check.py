#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Do the C and Python set converters agree, on real RetroAchievements data?

    RA_USER=... RA_PASS=... make check-raset

Fetches a sample of patch responses, converts each one both ways, and requires
the two files to be byte identical. Needs credentials and a network; skips
cleanly without them, because `make` should not fail on a plane.

Two converters exist for the same reason two hashers do: one has to run on the
device and one is the host bulk tool. This is what keeps the duplication from
becoming a drift - and the failure it guards against is silent, since a set
that is subtly wrong still loads and just never fires.

Nothing fetched here is written into the repository. RetroAchievements' data is
theirs; this holds it in a temp directory for the length of the run.
"""
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build-native", "raset-check")
UA = "TortOS/1.0 (+converter check)"

# A spread rather than a favorite: a small set, two large ones, and one whose
# conditions are long enough to have broken a fixed buffer.
#
# IDS ONLY. An earlier version carried a name beside each one, written from
# memory and never checked, and it printed "Castlevania" for a set that is
# Contra's. The comparison was still correct - it diffs two converters over
# whatever came back - but the report was a confident lie, and it took a human
# who knew he had not played Castlevania to catch it. The title is read from
# RetroAchievements' own response now, so it cannot be wrong without RA being
# wrong.
GAMES = [1459, 355, 10003, 11278, 1447, 4646]


def post(**kw):
    body = urllib.parse.urlencode(kw).encode()
    req = urllib.request.Request("https://retroachievements.org/dorequest.php",
                                 data=body, headers={"User-Agent": UA})
    return json.load(urllib.request.urlopen(req, timeout=30))


def main():
    user, pw = os.environ.get("RA_USER"), os.environ.get("RA_PASS")
    if not user or not pw:
        print("  skip: set RA_USER and RA_PASS to run this one")
        return 0
    ras = importlib.util.spec_from_file_location(
        "ra_sets", os.path.join(ROOT, "tools", "ra-sets.py"))
    m = importlib.util.module_from_spec(ras)
    ras.loader.exec_module(m)

    tok = post(r="login2", u=user, p=pw).get("Token")
    if not tok:
        print("  login failed")
        return 1

    tmp = tempfile.mkdtemp(prefix="raset-check-")
    bad = 0
    for gid in GAMES:
        d = post(r="patch", u=user, t=tok, g=gid)
        name = (d.get("PatchData") or {}).get("Title") or f"game {gid}"
        raw = os.path.join(tmp, f"{gid}.json")
        with open(raw, "w", encoding="utf-8") as f:
            json.dump(d, f, ensure_ascii=False)

        pd = d.get("PatchData", {})
        def real(a):
            t = str(a.get("Title", ""))
            return (int(a.get("Flags", 3)) == 3 and a.get("MemAddr")
                    and not t.startswith("Warning: Unknown Emulator")
                    and not t.startswith("Unsupported Game Version"))

        keep = [a for a in (pd.get("Achievements") or []) if real(a)]
        py = os.path.join(tmp, f"{gid}.py.set")
        m.write_set(py, gid, pd.get("ConsoleID", 0), pd.get("Title", ""), keep)

        c = os.path.join(tmp, f"{gid}.c.set")
        r = subprocess.run([BIN, str(gid), raw, c], capture_output=True, text=True)
        if r.returncode != 0:
            print(f"    {name}: C converter failed: {r.stderr.strip()}")
            bad += 1
            continue

        a, b = open(py, "rb").read(), open(c, "rb").read()
        if a != b:
            bad += 1
            print(f"    MISMATCH {name} ({len(keep)} achievements)")
            la, lb = a.decode().splitlines(), b.decode().splitlines()
            for i, (x, y) in enumerate(zip(la, lb)):
                if x != y:
                    print(f"      line {i+1}\n        python {x[:100]}\n        c      {y[:100]}")
                    break
            if len(la) != len(lb):
                print(f"      python {len(la)} lines, c {len(lb)}")
        else:
            print(f"  {name:<28} {len(keep):>3} achievements, identical")

    print(f"\n  {len(GAMES)} sets, {bad} mismatched")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
