#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Prove src/main.c's copy of the mark still matches tools/markdef.py.

markdef.py is the definition and the generators import it, but draw_shell() in
src/main.c cannot: it is C. So the shutdown animation carries a hand-kept copy
of the same six ring cells, the head, the center and the color the center dims
to, and its comment says "change one, change both".

That instruction is the whole safeguard, and an instruction in a comment is
worth about as much as the reader's memory on the day. The colors had already
drifted once in a way nobody noticed: the mark carried its own blue while the
launcher's chrome carried a cyan, and comments in both files claimed the two
were the same color for as long as they were not. Unifying them on 2026-08-28
is what turned the claim into something checkable, so it is checked here.

Also compares MENU_ACCENT, since it is now the same accent as the mark's
center rather than merely a neighbor of it.

Exits non-zero and prints what drifted. No output when everything agrees.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import markdef as m  # noqa: E402

MAIN = os.path.join(ROOT, "src", "main.c")


def main():
    src = open(MAIN).read()
    try:
        body = src[src.index("static void draw_shell"):]
        body = body[:body.index("\n}\n")]
    except ValueError:
        print("checkmark: draw_shell() not found in src/main.c", file=sys.stderr)
        return 1

    ring = [tuple(int(x) for x in t) for t in re.findall(
        r"\{\s*-?[\d.]+f,\s*-?[\d.]+f,\s*\{\s*(\d+),\s*(\d+),\s*(\d+)\s*\}\s*\}", body)]
    # col.r/g/b assignments in source order: head, center, then the dim over it
    assigns = [tuple(int(x) for x in t) for t in re.findall(
        r"col\.r\s*=\s*(\d+);\s*col\.g\s*=\s*(\d+);\s*col\.b\s*=\s*(\d+)", body)]

    bad = []
    want_ring = [c for _, _, c in m.CELLS]
    if ring != want_ring:
        bad.append(("ring cells", ring, want_ring))

    if len(assigns) < 3:
        bad.append(("head/center/dim", assigns, "three col.r/g/b assignments"))
    else:
        for name, got, want in (("head", assigns[0], m.DKGREEN),
                                ("center", assigns[1], m.CENTER),
                                ("dim", assigns[2], m.DKGREEN)):
            if got != want:
                bad.append((name, got, want))

    accent = re.search(r"#define MENU_ACCENT 0x([0-9A-Fa-f]{6})", src)
    if not accent:
        bad.append(("MENU_ACCENT", "not found", m.hexf(m.CYAN)))
    elif accent.group(1).upper() != ("%02X%02X%02X" % m.CYAN):
        bad.append(("MENU_ACCENT", "#" + accent.group(1).upper(), m.hexf(m.CYAN)))

    for name, got, want in bad:
        print(f"checkmark: {name} drifted\n"
              f"    src/main.c      {got}\n"
              f"    tools/markdef.py {want}", file=sys.stderr)
    if bad:
        print("checkmark: change one, change both.", file=sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
