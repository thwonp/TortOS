#!/usr/bin/env python3
"""Draw the Singles cover: res/singles.png.

Singles is Music/Singles, where the launcher moves songs left loose at the top
of Music/ (src/muselib.h), and this is the cover it puts at
Music/.media/Singles.png when Singles has none. There is no record to ask
MusicBrainz about, so it gets a cover of its own, drawn here the way
tools/genmusecard.py draws Muse's card: flat shapes, no strokes, the card stock
behind, the launcher's face. The label is TortOS's light green, the turtle's
shell and the wordmark's "Tort" (tools/markdef.py), Eric's pick, 2026-10-05:
Singles is TortOS's album, not a record's, and the cyan it wore first was a
step too loud beside the covers around it.

THE MARK is a 7-inch single: a dark disc with two faint grooved bands, a green
label, and the wide center hole a 45 has for a jukebox's spindle, which is what
says "single" rather than "record". Everything is filled shapes punched
through one another, as on the Muse card, because a stroked ring thins to
nothing at the size a cover is drawn on the shelf.

SQUARE, 500px: Muse draws covers square and crops anything that is not, and
the Cover Art Archive's 500px is the size Album Art fetches.

    tools/gensingles.py
"""
import os

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = os.path.join(ROOT, "res", "fonts", "menu.ttf")
OUT = os.path.join(ROOT, "res", "singles.png")

SIZE = 500
SS = 4                              # supersample, so the circles are not ragged
STOCK = (28, 31, 42)                # the cards' stock, as genmusecard.py
LABEL = (239, 239, 244)             # and their name color
GREEN = (128, 176, 118)             # TortOS light green, LTGRN in tools/markdef.py
VINYL = (14, 15, 20)
GROOVE = (24, 26, 34)


def disc(d, cx, cy, r, fill):
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=fill)


def main():
    s = SIZE * SS
    img = Image.new("RGB", (s, s), STOCK)
    d = ImageDraw.Draw(img)

    cx, cy = s // 2, int(s * 0.43)
    r = int(s * 0.36)
    disc(d, cx, cy, r, VINYL)
    # Two grooved bands, as filled rings a shade lighter than the vinyl.
    for outer, inner in ((0.94, 0.86), (0.74, 0.66)):
        disc(d, cx, cy, int(r * outer), GROOVE)
        disc(d, cx, cy, int(r * inner), VINYL)
    disc(d, cx, cy, int(r * 0.44), GREEN)          # the label
    disc(d, cx, cy, int(r * 0.17), STOCK)          # a 45's wide hole

    font = ImageFont.truetype(FONT, int(s * 0.11))
    text = "Singles"
    w = d.textlength(text, font=font)
    d.text((cx - w / 2, cy + r + s * 0.06), text, font=font, fill=LABEL)

    img.resize((SIZE, SIZE), Image.LANCZOS).save(OUT, optimize=True)
    print("wrote", os.path.relpath(OUT, ROOT))


if __name__ == "__main__":
    main()
