#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Draw res/cards/FAVORITES.png, the card for the Favorites shelf.

The other nine cards are drawn by hand and nothing generates them - gencards.py
was deleted because it could not reproduce any of the nine. This one is
generated because it is not a console: it belongs to TortOS, its accent is
TortOS's cyan rather than a machine's, and that cyan already has exactly one
definition in tools/markdef.py. A hand-drawn copy would be a fourth place for
it to drift.

Matched to the set by measurement rather than by eye: 640x820, card stock
(28,31,42), a 12px accent rule at y=587, the label under it in (239,239,244).
One mark: a bookmark ribbon with markdef.heart() knocked out of it, in the
same cyan as the rule. Three ascending stars before 2026-08-30, then three
hearts for about an hour.

KNOCKED OUT RATHER THAN OUTLINED, and that is the whole reason it fits. The
other nine cards are flat shapes - no strokes anywhere in the deck - and this
is the only card that is not a console, so it has to join that set by
construction rather than by luck. A stroked version was drawn and compared
side by side; it also thins out badly at the 62% a side card is scaled to,
where a solid silhouette does not.

The heart is markdef.heart(), the same curve src/main.c draws beside a
favorited game's name. A card and a shelf disagreeing about what "favorite"
looks like is the kind of thing nobody notices and everybody feels.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from markdef import CYAN, heart  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "res", "cards", "FAVORITES.png")
FONT = os.path.join(ROOT, "res", "fonts", "menu.ttf")

W, H = 640, 820
STOCK = (28, 31, 42)
LABEL = (239, 239, 244)
RULE_Y, RULE_H = 587, 12
RADIUS = 28
SS = 4                      # supersample, so the curve is not ragged


def bookmark(cx, cy, w, h, notch):
    """A ribbon: square shoulders, two points at the bottom with a V between."""
    x0, x1 = cx - w / 2.0, cx + w / 2.0
    y0, y1 = cy - h / 2.0, cy + h / 2.0
    return [(x0, y0), (x1, y0), (x1, y1), (cx, y1 - notch), (x0, y1)]


def mark(d, cx, cy, ribbon, stock):
    """The ribbon, then the heart punched through it back to the card stock.

    The heart sits above the notch rather than centered in the ribbon: centered,
    its point crowds the V and the two shapes argue about where the middle is.
    """
    d.polygon(bookmark(cx, cy, 232 * SS, 330 * SS, 68 * SS), fill=ribbon)
    hr, hy = 78 * SS, cy - 28 * SS
    d.polygon([(cx + x * 2 * hr, hy + y * 2 * hr) for x, y in heart(96)],
              fill=stock)


def main():
    im = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)

    d.rounded_rectangle([0, 0, W * SS - 1, H * SS - 1], radius=RADIUS * SS,
                        fill=STOCK + (255,))
    d.rectangle([0, RULE_Y * SS, W * SS - 1, (RULE_Y + RULE_H) * SS - 1],
                fill=CYAN + (255,))

    # Centered on the panel above the rule, not on the card.
    mark(d, (W // 2) * SS, 300 * SS, CYAN + (255,), STOCK + (255,))

    im = im.resize((W, H), Image.LANCZOS)
    d = ImageDraw.Draw(im)
    try:
        f = ImageFont.truetype(FONT, 54)
    except OSError:
        f = ImageFont.load_default()
    # Baseline and size measured off the set rather than chosen: the other
    # nine cards put a 42px cap height with its baseline at y=727.
    d.text((W // 2, 727), "FAVORITES", font=f, fill=LABEL + (255,), anchor="ms")

    im.save(OUT)
    print(OUT, os.path.getsize(OUT), "bytes")


if __name__ == "__main__":
    main()
