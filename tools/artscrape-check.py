#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run both normalizers over the same names and insist they agree.

    make check-artscrape

Names come from the real ROM library when one is reachable, and from a list of
awkward cases either way. The awkward ones are not invented: they are the
shapes that actually differ between libretro's catalog and a card, which is
the whole reason norm() exists.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import importlib.util
spec = importlib.util.spec_from_file_location(
    "scrape_art", os.path.join(ROOT, "tools", "scrape-art.py"))
scrape = importlib.util.module_from_spec(spec)
spec.loader.exec_module(scrape)

CASES = [
    "Sonic The Hedgehog (USA, Europe, Brazil) (En)",
    "Sonic The Hedgehog (USA, Europe, Brazil)",
    "Mario & Luigi - Superstar Saga (USA)",
    "Castlevania III - Dracula's Curse (USA)",
    "Final Fantasy III (USA) (Rev 1)",
    "Kirby's Dream Land 2 (USA, Europe) (SGB Enhanced)",
    "Dragon Warrior I & II (USA)",
    "Pokemon - Crystal Version (USA, Europe) (Rev 1)",
    "Legend of Zelda, The - A Link to the Past (USA)",
    "R-Type III - The Third Lightning (USA)",
    "  leading and trailing   ",
    "(only tags)",
    "",
    "UPPER CASE (USA)",
    "punctuation!!! ...here",
    "nested (a (b) c) tail",
    "unclosed (tag",
]


def library_names(roms):
    out = []
    if not os.path.isdir(roms):
        return out
    for folder in sorted(os.listdir(roms)):
        d = os.path.join(roms, folder)
        if not os.path.isdir(d):
            continue
        for f in sorted(os.listdir(d)):
            if f.startswith("."):
                continue
            out.append(os.path.splitext(f)[0])
    return out


def main():
    binary = os.path.join(ROOT, "build-native", "artscrape-check")
    roms = os.environ.get("TORTOS_ROMS", os.path.join(ROOT, "Roms"))

    names = CASES + library_names(roms)
    src = "\n".join(names) + "\n"
    got = subprocess.run([binary], input=src, capture_output=True,
                         text=True, check=True).stdout.split("\n")

    bad = 0
    for i, name in enumerate(names):
        want = scrape.norm(name)
        have = got[i] if i < len(got) else "<missing>"
        if want != have:
            bad += 1
            print(f"  FAIL: {name!r}\n        python {want!r}\n        c      {have!r}")

    where = "the library" if len(names) > len(CASES) else "the built-in cases only"
    print(f"\n  {len(names)} names from {where}, {bad} disagreement(s)")
    if bad:
        return 1
    print("\nok: both normalizers agree on every name")
    return 0


if __name__ == "__main__":
    sys.exit(main())
