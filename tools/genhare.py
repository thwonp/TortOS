# SPDX-License-Identifier: MIT
"""The Over The Hare mark: the tortoise has already gone past.

Left to right: a hare's head, the speed lines the tortoise left behind, and
the tortoise itself - up and ahead. The joke is the fable's ending rather than
its premise, and it puts the three pieces in reading order.

NOTHING HERE IS REDRAWN. The shell's cells, its head, its colors and the
three speed lines are imported from markdef.py and genmark.py, which is where
they were already defined and already exported to tortos_logo_ideas. The first
version of this file hand-copied the cell table (and lost the tortoise's head
doing it) and then invented the speed lines from scratch as thin rounded bars -
they are thick parallelograms slanted seven degrees, and that was sitting in
genmark.py the whole time.

So: the only thing this module defines is the hare and where the three pieces
sit relative to each other. Everything that already had a definition keeps it.

Regenerate with:

    python3 tools/genhare.py res/web/mark.svg
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import genmark as G
import markdef as M

# The one color this mark adds, and it is not a new one: markdef retired
# OFFWHT from the wordmark's "Tort" on 2026-08-28 for belonging to no part of
# the mark. The hare is the part of this mark that is not a tortoise, so the
# color that belonged to nothing has something to be.
OFFWHT = (233, 236, 227)

R = 10.0                       # hex radius here; genmark draws at G.R
DX, DY = math.sqrt(3.0) * R, 1.5 * R
SCALE = R / G.R                # genmark's units into these

SX, SY = 5.5, -2.0             # the tortoise: right, and up
HX, HY = -2.0, 2.0             # the hare's head

# A hex flower for the face. The seven-cell version is not a style choice: a
# compact head detaches from the ears entirely, because its top row is two
# lattice rows away rather than one, and only the flower bridges.
HEAD = [(HX-1, HY), (HX, HY), (HX+1, HY),
        (HX-0.5, HY-1), (HX+0.5, HY-1), (HX-0.5, HY+1), (HX+0.5, HY+1)]
# Two ears with a whole empty cell between them at every row. Hexes tile with
# no gaps, so anything adjacent merges into one mass - three earlier attempts
# put the ears beside the head and drew a blob. The negative space is the ear.
EARS = [(HX-1, HY-2), (HX-1.5, HY-3),
        (HX+1, HY-2), (HX+1.5, HY-3)]


def hexpts(cx, cy, r):
    return [(cx + r * math.cos(math.radians(60 * k - 90)),
             cy + r * math.sin(math.radians(60 * k - 90))) for k in range(6)]


def cells():
    """(points, color) for every hexagon: the hare, then the tortoise."""
    out = [(hexpts(i * DX, j * DY, R * 0.94), OFFWHT) for i, j in HEAD + EARS]
    for i, j, col in list(M.CELLS) + [M.HEAD]:
        out.append((hexpts((SX + i) * DX, (SY + j) * DY, R * 0.94), col))
    out.append((hexpts(SX * DX, SY * DY, R * 0.94), M.CENTER))
    return out


def bars():
    """The wordmark's own three lines, moved into this composition.

    Taken from genmark.RAW through genmark.barpts, so the thickness, the pitch,
    the seven-degree slant and the three lengths are the ones the lockup
    already uses. Only the placement is decided here: scaled to this hex
    radius, then set down in the gap between the hare and the tortoise.

    Free at both ends. They are the tortoise's wake, not a tether - touching
    the shell read as a wire plugged into it, and touching the ear read as one
    joining the two animals.
    """
    raw = [(G.barpts(a, b, y), c) for (y, a, b, c) in G.RAW]
    xs = [p[0] for pts, _ in raw for p in pts]
    ys = [p[1] for pts, _ in raw for p in pts]
    # genmark's group, in this file's units, centered on nothing yet.
    w = (max(xs) - min(xs)) * SCALE
    h = (max(ys) - min(ys)) * SCALE

    # (SX - 1), not SX: the shell's LEFT CELL is one step left of its center,
    # and measuring the gap from the center put the bars a whole cell into it.
    right = (SX - 1) * DX - 0.95 * R - 0.9 * R
    left = 0.55 * R                           # clear of the hare's ear tip
    # Squeezed horizontally to fit the gap; the vertical keeps genmark's own
    # proportion, so the bars stay as thick relative to each other as they are
    # in the lockup.
    kx = (right - left) / w
    cy = SY * DY

    out = []
    for pts, col in raw:
        out.append(([(left + (x - min(xs)) * SCALE * kx,
                      cy + (y - min(ys)) * SCALE - h / 2) for x, y in pts], col))
    return out


def svg(pad=2.0):
    shapes = [(p, c) for p, c in bars()] + cells()
    xs = [x for pts, _ in shapes for x, _ in pts]
    ys = [y for pts, _ in shapes for _, y in pts]
    x0, x1 = min(xs) - pad, max(xs) + pad
    y0, y1 = min(ys) - pad, max(ys) + pad
    o = ['<svg viewBox="%.1f %.1f %.1f %.1f" xmlns="http://www.w3.org/2000/svg" '
         'role="img" aria-label="A tortoise, already past a hare">'
         % (x0, y0, x1 - x0, y1 - y0)]
    for pts, col in shapes:
        o.append('<polygon points="%s" fill="#%02X%02X%02X"/>'
                 % (" ".join("%.2f,%.2f" % p for p in pts), *col))
    o.append('</svg>')
    return "".join(o)




# ---- the favicon ----------------------------------------------------------
#
# The tortoise with the head left off: six shell cells and the charge at the
# center, which is a compact near-circular shape that still reads at 16px.
# The full mark does not - the head puts two thirds of the ink on one side, so
# scaled into a square favicon the shell shrinks to nothing.
#
# Imported from markdef/genmark like everything else here. CELLS without HEAD
# is the whole difference; no cell is redrawn.

def icon_svg(pad=4.0):
    cells = [(i * G._dxh, j * G._dyh, c) for (i, j, c) in M.CELLS]
    shapes = [(G.hexpts(x, y, G.R * 0.95), c) for x, y, c in cells]
    shapes.append((G.hexpts(0.0, 0.0, G.R * 0.95), M.CENTER))
    xs = [x for pts, _ in shapes for x, _ in pts]
    ys = [y for pts, _ in shapes for _, y in pts]
    # Square, and centered on the ink rather than on the origin: a favicon is
    # drawn into a square box whatever we say, so the box is chosen here where
    # the padding can stay even.
    cx, cy = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
    half = max(max(xs) - min(xs), max(ys) - min(ys)) / 2 + pad
    o = ['<svg viewBox="%.1f %.1f %.1f %.1f" xmlns="http://www.w3.org/2000/svg" '
         'role="img" aria-label="TortOS">'
         % (cx - half, cy - half, half * 2, half * 2)]
    for pts, col in shapes:
        o.append('<polygon points="%s" fill="#%02X%02X%02X"/>'
                 % (" ".join("%.2f,%.2f" % p for p in pts), *col))
    o.append('</svg>')
    return "".join(o)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "res/web"
    for name, doc in (("mark.svg", svg()), ("icon.svg", icon_svg())):
        open(os.path.join(out, name), "w").write(doc)
        print("wrote", os.path.join(out, name))
    # The page's watermark: the plain tortoise, written through genmark's OWN
    # writer so it is the same document as tortos_logo_ideas/exports rather
    # than a copy of it that can drift.
    G.OUT = out
    G.svg("turtle.svg", G.MARK, G.mark_svg(), False)
    print("wrote", os.path.join(out, "turtle.svg"))
