# SPDX-License-Identifier: MIT
"""The TortOS mark: one definition, everything else derives from it.

The shell's colors and cell positions were duplicated across the boot
animation, the shutdown animation in src/main.c, and the exported SVG and PNG.
On 2026-08-28 the cell beside the head changed color and that meant editing
the same table in four places - the identical failure the system accents had,
where six of nine cards had drifted from config/systems.cfg.

So: this module is the source. tools/genboot.py and tools/genmark.py import it.
src/main.c cannot, being C, and carries the only hand-kept copy - draw_shell()
names this file so the two can be compared when either changes.

CYAN is canonical for the whole system as of 2026-08-28, and is the same value
as MENU_ACCENT in src/main.c and UI_CYAN_* in src/ui.h. The mark used to carry
its own blue, (74,158,255), while the launcher's chrome was this cyan; two
comments claimed they were one color and neither was checked against the other.
They are one color now, so the boot animation, the shutdown animation, the
wordmark and the menu chrome are the same accent rather than a near miss.

That direction was the safe one. Unifying on the mark's old blue instead would
have put the launcher's chrome about eight degrees of hue from the Genesis card
accent in config/systems.cfg, close enough to read as a mistake; cyan is well
clear of all nine.

The palette is four: the shell's three greens, and cyan. The wordmark uses the
same four rather than a neutral, so it reads as the mark unrolled - shell green
for Tort, cyan for the OS, and speed lines behind it repeating the light, mid
and dark stack the shell already has. An off-white Tort belonged to nothing
else in the mark, which is the argument that retired it.

The ring alternates light, mid, light, mid, light, mid, which gives the shell
three-fold symmetry rather than a light pair on one side. The head is the same
radius and the same lattice step as every other cell: it is a scute that
happens to be dark, not an appendage.
"""

BG      = (17, 19, 16)        # the ground, not one of the four

# Four colors, and only these four. The three greens are the shell, lightest to
# darkest; cyan is the charge at the center of it. Anything that needs a color
# picks one of these rather than introducing a near neighbor - which is exactly
# how the palette went wrong twice. GREEN, (94,138,86), painted one speed line
# and sat ten points from MIDGRN, indistinguishable and off the shell. OFFWHT,
# (233,236,227), was the wordmark's Tort and belonged to no part of the mark.
LTGRN   = (128, 176, 118)     # shell, and the wordmark's Tort
MIDGRN  = (104, 138, 96)      # shell, and the lower speed line
DKGREEN = (61, 89, 67)        # the head, the upper speed line, and what dims
                              # the center at shutdown
CYAN    = (61, 214, 255)      # the center, the wordmark's OS, the middle speed
                              # line; also the launcher's chrome and volume OSD

# (lattice i, lattice j, color) - i in units of sqrt(3)*r, j in units of 1.5*r
CELLS = [
    (-0.5, -1.0, LTGRN),   # upper left
    ( 0.5, -1.0, MIDGRN),  # upper right
    ( 1.0,  0.0, LTGRN),   # right, beside the head
    ( 0.5,  1.0, MIDGRN),  # lower right
    (-0.5,  1.0, LTGRN),   # lower left
    (-1.0,  0.0, MIDGRN),  # left
]
HEAD = (2.0, 0.0, DKGREEN)    # one step beyond the right cell
CENTER = CYAN

# ---- the favorite mark ----------------------------------------------------
#
# A heart, as the classic parametric curve:
#
#     x = 16 sin^3 t
#     y = 13 cos t - 5 cos 2t - 2 cos 3t - cos 4t
#
# Here rather than in either drawer because there are two: the shelf draws it
# in C with SDL_RenderGeometry, and tools/genfavcard.py draws it into
# res/cards/FAVORITES.png with PIL. The same argument as CELLS - a hand-kept
# second copy is a second thing to get wrong, and these two are meant to be
# the same shape.
#
# Returns points in a unit box centered on the origin, y DOWN (screen order),
# scaled so the taller axis spans 1.0.

def heart(n=64):
    import math

    pts = []
    for i in range(n):
        t = 2.0 * math.pi * i / n
        x = 16.0 * math.sin(t) ** 3
        y = (13.0 * math.cos(t) - 5.0 * math.cos(2 * t)
             - 2.0 * math.cos(3 * t) - math.cos(4 * t))
        pts.append((x, -y))                      # -y: screen coordinates
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    cx, cy = (min(xs) + max(xs)) / 2.0, (min(ys) + max(ys)) / 2.0
    k = 1.0 / max(max(xs) - min(xs), max(ys) - min(ys))
    return [((x - cx) * k, (y - cy) * k) for x, y in pts]


def hexf(rgb):
    return "#%02X%02X%02X" % rgb
