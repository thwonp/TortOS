#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Export the TortOS mark and wordmark as SVG and PNG.

    python3 tools/genmark.py [outdir]      default: tortos_logo_ideas/exports

Cells and colors come from tools/markdef.py, the same module the boot
animation imports, so the art cannot drift from what the device draws. The type
is converted to outlines rather than referenced by family name: the files render
identically on a machine with no Josefin installed, and the paths can be pulled
straight into a design app.

The capital T is drawn from its own outline points rather than set as text,
because it is positioned independently of "ort" to close the gap between them.
"""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from markdef import BG, LTGRN, CYAN, MIDGRN, DKGREEN, CELLS, HEAD, CENTER, hexf

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = os.path.join(ROOT, "res", "fonts", "wordmark.ttf")
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "tortos_logo_ideas", "exports")

FSIZE, K, TUCK = 150.0, 0.15, -24.0
ANG = math.radians(7.0)
TH, TCAP = 20.0, 110.0
GAP = (TCAP - 3 * TH) / 2
PITCH = TH + GAP
BASELINE = 248.0
T_TOP = BASELINE - TCAP
T_PTS = [(109, 728), (596, 728), (581, 614), (396, 614),
         (320, 0), (200, 0), (276, 614), (95, 614)]
T_ADV, INK_R, CANVAS_W = 583.0, 477.0, 1200.0
RAW = [(T_TOP + TH / 2, 400, 600, DKGREEN),
       (T_TOP + TH / 2 + PITCH, 250, 622, CYAN),
       (T_TOP + TH / 2 + PITCH * 2, 355, 600, MIDGRN)]
R, PAD = 40.0, 24.0


def barpts(x0, x1, yc):
    s = math.tan(ANG) * TH / 2
    return [(x0 + s, yc - TH / 2), (x1 + s, yc - TH / 2),
            (x1 - s, yc + TH / 2), (x0 - s, yc + TH / 2)]


DX = CANVAS_W / 2 - (min(min(p[0] for p in barpts(a, b, y)) for (y, a, b, _) in RAW)
                     + 650.0 + INK_R) / 2
WORD_X = 650.0 + DX
BARS = [(y, a + DX, b + DX, c) for (y, a, b, c) in RAW]
LOCK = (min(min(p[0] for p in barpts(a, b, y)) for (y, a, b, _) in BARS) - PAD,
        T_TOP - PAD, WORD_X + INK_R + PAD, BASELINE + 6 + PAD)


def hexpts(cx, cy, r):
    return [(cx + r * math.cos(math.pi / 6 + i * math.pi / 3),
             cy + r * math.sin(math.pi / 6 + i * math.pi / 3)) for i in range(6)]


_dxh, _dyh = math.sqrt(3) * R, 1.5 * R
MARK_CELLS = [(i * _dxh, j * _dyh, c) for (i, j, c) in list(CELLS) + [HEAD]]
_mx = [p[0] for (x, y, _) in MARK_CELLS for p in hexpts(x, y, R * 0.95)]
_my = [p[1] for (x, y, _) in MARK_CELLS for p in hexpts(x, y, R * 0.95)]
MARK = (min(_mx) - 6, min(_my) - 6, max(_mx) + 6, max(_my) + 6)

_f = TTFont(FONT)
_upm = _f["head"].unitsPerEm
_gs, _cmap, _hmtx = _f.getGlyphSet(), _f.getBestCmap(), _f["hmtx"]


def glyph(ch):
    gn = _cmap[ord(ch)]
    pen = SVGPathPen(_gs)
    _gs[gn].draw(pen)
    return pen.getCommands(), _hmtx[gn][0]


def svg(name, box, inner, dark):
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    rect = ('  <rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="%s"/>\n'
            % (x0, y0, w, h, hexf(BG))) if dark else ""
    doc = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="%.1f %.1f %.1f %.1f" '
           'width="%.0f" height="%.0f">\n%s  <g>\n%s\n  </g>\n</svg>\n'
           % (x0, y0, w, h, w, h, rect, inner))
    open(os.path.join(OUT, name), "w").write(doc)


def poly(pts, fill):
    return ('    <polygon points="%s" fill="%s"/>'
            % (" ".join("%.3f,%.3f" % p for p in pts), fill))


def lockup_svg():
    f = [poly(barpts(a, b, y), hexf(c)) for (y, a, b, c) in BARS]
    f.append(poly([(WORD_X + px * K, BASELINE - py * K) for px, py in T_PTS], hexf(LTGRN)))
    scale, penx = FSIZE / _upm, WORD_X + T_ADV * K + TUCK
    for text, col in (("ort", hexf(LTGRN)), ("OS", hexf(CYAN))):
        for ch in text:
            d, adv = glyph(ch)
            if d:
                f.append('    <path d="%s" fill="%s" transform="translate(%.3f,%.3f) '
                         'scale(%.6f,%.6f)"/>' % (d, col, penx, BASELINE, scale, -scale))
            penx += adv * scale
    return "\n".join(f)


def mark_svg():
    f = [poly(hexpts(x, y, R * 0.95), hexf(c)) for (x, y, c) in MARK_CELLS]
    f.append(poly(hexpts(0, 0, R * 0.95), hexf(CENTER)))
    return "\n".join(f)


def png(kind, scale, dark):
    x0, y0, x1, y1 = LOCK if kind == "lockup" else MARK
    w, h = x1 - x0, y1 - y0
    ss, k = 3, scale * 3
    im = Image.new("RGBA", (int(w * scale * ss), int(h * scale * ss)),
                   (BG + (255,)) if dark else (0, 0, 0, 0))
    d = ImageDraw.Draw(im, "RGBA")
    T = lambda px, py: ((px - x0) * k, (py - y0) * k)
    if kind == "lockup":
        for (y, a, b, c) in BARS:
            d.polygon([T(*p) for p in barpts(a, b, y)], fill=c + (255,))
        d.polygon([T(WORD_X + px * K, BASELINE - py * K) for px, py in T_PTS],
                  fill=LTGRN + (255,))
        fo = ImageFont.truetype(FONT, int(FSIZE * k))
        bx, by = T(WORD_X + T_ADV * K + TUCK, BASELINE)
        d.text((bx, by), "ort", font=fo, fill=LTGRN + (255,), anchor="ls")
        d.text((bx + d.textlength("ort", font=fo), by), "OS", font=fo,
               fill=CYAN + (255,), anchor="ls")
    else:
        for (x, y, c) in MARK_CELLS:
            d.polygon([T(*p) for p in hexpts(x, y, R * 0.95)], fill=c + (255,))
        d.polygon([T(*p) for p in hexpts(0, 0, R * 0.95)], fill=CENTER + (255,))
    return im.resize((int(w * scale), int(h * scale)), Image.LANCZOS)


def main():
    os.makedirs(OUT, exist_ok=True)
    for nm, box, inner in (("tortos-lockup", LOCK, lockup_svg()),
                           ("tortos-mark", MARK, mark_svg())):
        svg(nm + ".svg", box, inner, False)
        svg(nm + "-on-dark.svg", box, inner, True)
        print("  %s.svg  + on-dark" % nm)
    for kind in ("lockup", "mark"):
        for scale, tag in ((1, "@1x"), (2, "@2x"), (3, "@3x"), (8, "-master")):
            for dark in (False, True):
                im = png(kind, scale, dark)
                n = "tortos-%s%s%s.png" % (kind, tag, "-on-dark" if dark else "")
                im.save(os.path.join(OUT, n), dpi=(72 * scale, 72 * scale))
        print("  tortos-%s  @1x @2x @3x master, transparent + on-dark" % kind)

    # The two the README shows, written into the repository rather than left
    # here: tortos_logo_ideas/ is ignored, so a front page pointing at it would
    # be broken for everyone who clones. Transparent, because GitHub renders
    # the page on either a light or a dark ground depending on the reader.
    rd = os.path.join(ROOT, "res", "readme")
    os.makedirs(rd, exist_ok=True)
    for kind, name in (("mark", "turtle.png"), ("lockup", "wordmark.png")):
        png(kind, 2, False).save(os.path.join(rd, name), dpi=(144, 144))
    print("  res/readme/turtle.png + wordmark.png")


if __name__ == "__main__":
    main()
