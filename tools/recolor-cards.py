#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Repaint each card's accent rule from config/systems.cfg.

systems.cfg is the one place a system's color is written down. The card art is
downstream of it: this reads the accent, finds the rule band on the card, and
repaints it, leaving every other pixel alone.

Why this exists. The color used to live in three places - systems.cfg, a table
inside a card generator, and the pixels of nine PNGs - kept in step by hand. On
2026-08-28 six of the nine had drifted from the config, and the three that had
not were exactly the three the generator produced, because its duplicate table
happened to agree. Nothing tied the hand-drawn six to anything. The generator
is gone; this is what keeps the art honest to the config now.

What it does NOT do. It will not draw a rule that is not already there, and it
does not touch the mark, the label or the slab. Recoloring an existing band is
the whole job; the rest of a card is artwork.

    python3 tools/recolor-cards.py            # report only
    python3 tools/recolor-cards.py --apply    # rewrite the PNGs

Antialiasing is preserved rather than redrawn. Every pixel of the band is some
blend of the old accent and the card's background; the blend factor is measured
per pixel and re-applied against the new accent, so the soft top and bottom
edges stay exactly as soft as the artist drew them.

Not quite lossless. A round trip through a different color and back leaves the
two antialiased edge rows differing by a few units of 255 - about 1280 pixels
of 524800, invisible on the panel. The size depends on the color pair, not on
some fixed bound: green through magenta and back measured 2, orange through
pink and back measured 4, because how far a channel separates the accent from
the background is what the blend factor is solved on.

It does not accumulate. A card whose band already matches the config is skipped
entirely, so the only way to pay the rounding twice is to change the color
twice. If a card must stay byte-identical, restore it from git rather than
recoloring it back.
"""
import collections
import os
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CFG = os.path.join(ROOT, "config", "systems.cfg")
CARDS = os.path.join(ROOT, "res", "cards")


def systems():
    """(card filename, display name, accent rgb) for each configured system."""
    out = []
    for line in open(CFG, encoding="utf-8"):
        if not line.startswith("sys|"):
            continue
        f = line.rstrip("\n").split("|")
        hexcol = f[6].strip()
        rgb = tuple(int(hexcol[i:i + 2], 16) for i in (0, 2, 4))
        out.append((f[5], f[1], rgb))
    return out


def find_band(px, w, h, bg):
    """Rows of the full-width accent rule, or None.

    Deliberately strict: a row counts only if most of its width differs from
    the background. A card's mark and label also differ from the background,
    and neither runs the full width.
    """
    rows = [
        y for y in range(int(h * 0.6), h)
        if sum(1 for x in range(20, w - 20, 2)
               if sum(abs(px[x, y][c] - bg[c]) for c in range(3)) > 60)
        > (w - 40) // 2 * 0.8
    ]
    if not rows or rows != list(range(rows[0], rows[-1] + 1)):
        return None
    return rows


def solid_color(px, w, rows):
    """The band's own color: the most common pixel across its middle row."""
    y = rows[len(rows) // 2]
    return collections.Counter(
        px[x, y][:3] for x in range(w // 2 - 60, w // 2 + 60)).most_common(1)[0][0]


def blend(bg, old, new, pixel):
    """Re-apply this pixel's blend factor against the new accent.

    Solved on whichever channel separates bg from old the most, because a
    channel where they nearly agree gives a ratio dominated by rounding.
    """
    spread = [abs(old[c] - bg[c]) for c in range(3)]
    c = spread.index(max(spread))
    if spread[c] == 0:
        return pixel[:3]
    t = (pixel[c] - bg[c]) / (old[c] - bg[c])
    t = max(0.0, min(1.0, t))
    return tuple(int(round(bg[k] + t * (new[k] - bg[k]))) for k in range(3))


def main():
    apply = "--apply" in sys.argv
    changed = failed = 0
    for card, name, want in systems():
        path = os.path.join(CARDS, card)
        if not os.path.exists(path):
            print(f"  {card:14} {name:18} MISSING")
            failed += 1
            continue
        im = Image.open(path).convert("RGBA")
        px = im.load()
        w, h = im.size
        bg = px[10, h - 10][:3]
        rows = find_band(px, w, h, bg)
        if rows is None:
            print(f"  {card:14} {name:18} no full-width rule found - skipped")
            failed += 1
            continue
        have = solid_color(px, w, rows)
        if have == want:
            print(f"  {card:14} {name:18} #{'%02X%02X%02X' % have} ok")
            continue
        changed += 1
        print(f"  {card:14} {name:18} #{'%02X%02X%02X' % have} -> "
              f"#{'%02X%02X%02X' % want}{'' if apply else '   (dry run)'}")
        if not apply:
            continue
        for y in rows:
            for x in range(w):
                p = px[x, y]
                px[x, y] = blend(bg, have, want, p) + (p[3],)
        im.save(path)

    print(f"\n{changed} card(s) {'repainted' if apply else 'would change'}, "
          f"{failed} skipped")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
