#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Write res/fbneo-titles.tsv: every FBNeo arcade set's name and the game it is.

    tools/gen-fbneo-titles.py FBNEO_CLONE

An arcade zip is named for its set - mk3.zip, 3countb.zip - and a shelf that
showed that would be a list of codes. A gamelist's <name> is the first choice
for the title (src/titles.c); this table is what the Arcade and Neo Geo
shelves fall back on when the list has no entry for a set.

FROM THE DAT OF THE CORE THAT SHIPS. FBNeo's set names change between
versions - mario was Rev G in ROCKNIX's older build and is Rev E now - so the
clone must be at the commit mk/build-fbneo.sh pins, or this refuses. One DAT
covers both shelves: "Arcade only" holds every Neo Geo set as well ("Neogeo
only" is a subset of it, checked 2026-10-01).

One line per set, `set<TAB>description`, sorted bytewise on the set so the
launcher can bsearch it with strcmp. The description is kept RAW, revision
and all: libretro's "FBNeo - Arcade Games" and "SNK - Neo Geo" thumbnails are
named by it, and the shelf strips the brackets itself (lib_title).
"""
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DAT = "dats/FinalBurn Neo (ClrMame Pro XML, Arcade only).dat"
OUT = os.path.join(ROOT, "res", "fbneo-titles.tsv")


def pinned():
    with open(os.path.join(ROOT, "mk", "build-fbneo.sh")) as f:
        m = re.search(r"^PIN=([0-9a-f]{40})", f.read(), re.M)
    if not m:
        sys.exit("gen-fbneo-titles: no PIN= in mk/build-fbneo.sh")
    return m.group(1)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip().splitlines()[2].strip())
    clone = sys.argv[1]
    head = subprocess.run(["git", "-C", clone, "rev-parse", "HEAD"],
                          capture_output=True, text=True).stdout.strip()
    if head != pinned():
        sys.exit(f"gen-fbneo-titles: {clone} is at {head or 'no commit'}, "
                 f"not the pinned {pinned()}")

    rows = {}
    for g in ET.parse(os.path.join(clone, DAT)).getroot().iter("game"):
        name = g.get("name") or ""
        desc = " ".join((g.findtext("description") or "").split())
        if name and desc:
            rows[name] = desc
    with open(OUT, "wb") as out:
        for name in sorted(rows, key=lambda s: s.encode()):
            out.write(f"{name}\t{rows[name]}\n".encode("utf-8"))
    print(f"gen-fbneo-titles: {len(rows)} sets -> {os.path.relpath(OUT, ROOT)}")


if __name__ == "__main__":
    main()
