#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Draw Muse's two cards: res/cards/classic/MUSE.png and res/cards/fancy/MUSE.png.

Muse is not a console, so like Favorites it has no photograph in the classic
set to trace and no hand-drawn card to copy - which is why it is generated, the
way tools/genfavcard.py generates the Favorites card.

THE ACCENT is the iPod's own anodised green, sampled from the photograph the
fancy card is cut from: the median of its body pixels, 2026-09-18, is #9CD345.
Clear of the three greens already on the shelf - Game Boy's olive 7E9B47,
NGPC's emerald 00AA4F and Game Gear's teal 00B589. The same value is
MUSE_ACCENT in src/main.c; change one, change the other.

CLASSIC matches the set by measurement, as the Favorites card does: 640x820,
card stock (28,31,42), a 12px accent rule at y=587, the name under it in
(239,239,244), 54px, baseline 727. The mark is an iPod reduced to three flat
shapes - body, screen, click wheel - with the screen and the wheel KNOCKED OUT
back to the card stock rather than outlined, because the deck has no strokes
anywhere and a stroked mark thins to nothing at the 62% a side card is drawn at.

FANCY follows res/cards/fancy/SOURCE.md: trimmed to its own alpha, longest side
96% of a 384 frame, centered - and KEPT IN THE 384 CANVAS, because it is taller
than it is wide. src/cards.h says why that is load-bearing: cropped, contain-fit
would grow it through the system name at y=618. Game Boy Color's ink is 369px
tall in the same canvas, and so is this.

    tools/genmusecard.py [photo]     default: cards_backup_photos/muse.png
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = os.path.join(ROOT, "res", "fonts", "menu.ttf")
CLASSIC = os.path.join(ROOT, "res", "cards", "classic", "MUSE.png")
FANCY = os.path.join(ROOT, "res", "cards", "fancy", "MUSE.png")
PHOTO = os.path.join(ROOT, "cards_backup_photos", "muse.png")

GREEN = (0x9C, 0xD3, 0x45)

W, H = 640, 820
STOCK = (28, 31, 42)
LABEL = (239, 239, 244)
RULE_Y, RULE_H = 587, 12
RADIUS = 28
SS = 4                      # supersample, so the curves are not ragged


def mark(d, cx, cy, fill, stock):
    """An iPod in three flat shapes: the body, then the screen and the click
    wheel punched through it, then the wheel's center button left standing.

    Sized to sit where the Favorites ribbon sits - that is 232x330 - so the two
    special cards read as the same kind of thing at the ends of the shelf."""
    bw, bh = 220 * SS, 340 * SS
    x0, y0 = cx - bw // 2, cy - bh // 2
    d.rounded_rectangle([x0, y0, x0 + bw, y0 + bh], radius=34 * SS, fill=fill)

    sw, sh = 172 * SS, 124 * SS
    sx, sy = cx - sw // 2, y0 + 26 * SS
    d.rounded_rectangle([sx, sy, sx + sw, sy + sh], radius=10 * SS, fill=stock)

    wr = 70 * SS                               # the wheel
    wy = y0 + bh - 26 * SS - wr
    d.ellipse([cx - wr, wy - wr, cx + wr, wy + wr], fill=stock)
    br = 26 * SS                               # its center button
    d.ellipse([cx - br, wy - br, cx + br, wy + br], fill=fill)


def classic():
    im = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)

    d.rounded_rectangle([0, 0, W * SS - 1, H * SS - 1], radius=RADIUS * SS,
                        fill=STOCK + (255,))
    d.rectangle([0, RULE_Y * SS, W * SS - 1, (RULE_Y + RULE_H) * SS - 1],
                fill=GREEN + (255,))
    mark(d, (W // 2) * SS, 300 * SS, GREEN + (255,), STOCK + (255,))

    im = im.resize((W, H), Image.LANCZOS)
    d = ImageDraw.Draw(im)
    try:
        f = ImageFont.truetype(FONT, 54)
    except OSError:
        f = ImageFont.load_default()
    d.text((W // 2, 727), "MUSE", font=f, fill=LABEL + (255,), anchor="ms")
    im.save(CLASSIC)
    print(CLASSIC, os.path.getsize(CLASSIC), "bytes")


def fancy(photo):
    src = Image.open(photo).convert("RGBA")
    box = src.getchannel("A").getbbox()
    if box:
        src = src.crop(box)
    k = 369.0 / max(src.size)                  # 96% of 384
    sw, sh = round(src.width * k), round(src.height * k)
    src = src.resize((sw, sh), Image.LANCZOS)
    out = Image.new("RGBA", (384, 384), (0, 0, 0, 0))
    out.paste(src, ((384 - sw) // 2, (384 - sh) // 2), src)
    out.save(FANCY)
    print(FANCY, out.size, "ink", out.getchannel("A").getbbox())


def main():
    classic()
    photo = sys.argv[1] if len(sys.argv) > 1 else PHOTO
    if os.path.exists(photo):
        fancy(photo)
    else:
        print("no photograph at", photo, "- the fancy card was not redrawn")


if __name__ == "__main__":
    main()
