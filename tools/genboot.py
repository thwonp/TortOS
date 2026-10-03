#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Render the TortOS boot animation and the still images that bracket it.

Run from the repo root:  python3 tools/genboot.py

Outputs (all under res/boot/):
  tortos-boot.mp4   1024x768, 30 fps, exactly 72 frames (2.400 s), H.264.
  bootlogo.bmp      frame 0 as a 24-bit BMP, for the bootloader splash slot.
  splash.png        frame 0 as a PNG, for tooling that wants a lossless copy.

Frame 0 is pure black on purpose, and asserted before anything is written. The
stock splash is replaced by bootlogo.bmp so the handoff into the animation is a
continuation rather than a cut, and that only works if the first frame matches
what the bootloader left on the panel.

THE LENGTH IS A CONTRACT, NOT A PREFERENCE. launch.sh starts this in the
background and the launcher blocks on /tmp/tortos_bootanim until ffmpeg clears
it, so anything longer than the launcher's own startup is time the player
spends waiting at a picture. Today the launcher is ready about 1.06 s after it
starts, and roughly 90 lines of launch.sh run between this starting and the
launcher starting, so 2.4 s finishes first and nobody waits. The launcher
reports it if that ever stops being true: wait_for_boot_anim calls
t_mark("anim wait"), which prints only when it actually waited. A `boot: anim
wait` line in tortos.log means this file got too long or startup got faster.

The last beat is a still hold, and the frame it holds is the finished lockup -
the same composition as tortos-lockup.svg, from the same constants. When
ffmpeg exits, nothing clears the framebuffer, so that frame stays on the panel
until the launcher presents. It is on screen longer than any other frame in the
file and is the one worth getting right.

Everything is drawn at 3x and downsampled with LANCZOS: the shell is all
diagonal hexagon edges and PIL's polygon rasterizer has no antialiasing of its
own.
"""

import math
import os
import shutil
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "res", "boot")
TMP_DIR = os.path.join(ROOT, "build", "boot-frames")
FONT_PATH = os.path.join(ROOT, "res", "fonts", "wordmark.ttf")

W, H = 1024, 768
FPS = 30
N_FRAMES = 72                      # 2.400 s exactly
SS = 3                             # supersample factor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from markdef import BG, LTGRN, CYAN, MIDGRN, DKGREEN, CELLS, HEAD   # noqa: E402

# ---- the lockup, in the same design units the exported SVG uses -------------
FSIZE  = 150.0
K      = FSIZE / 1000.0            # font units -> design units
TUCK   = -24.0                     # the T pulled toward 'ort'
ANG    = math.radians(7.0)         # the font's own italic angle
TH     = 20.0                      # speed bar thickness
TCAP   = 110.0                     # the T's cap height; the bar stack matches it
GAP    = (TCAP - 3 * TH) / 2
PITCH  = TH + GAP
BASELINE = 248.0
T_TOP  = BASELINE - TCAP
# The capital T as an outline, so it can be positioned independently of 'ort'.
T_PTS  = [(109, 728), (596, 728), (581, 614), (396, 614),
          (320, 0), (200, 0), (276, 614), (95, 614)]
T_ADV  = 583.0
INK_R  = 477.0                     # measured ink right edge, with TUCK applied
DESIGN_W = 1200.0

BARS_RAW = [(T_TOP + TH / 2,             400, 600, DKGREEN),
            (T_TOP + TH / 2 + PITCH,     250, 622, CYAN),
            (T_TOP + TH / 2 + PITCH * 2, 355, 600, MIDGRN)]


def bar_pts(x0, x1, yc):
    """A speed bar: ends cut parallel to the T's stem rather than square."""
    s = math.tan(ANG) * TH / 2
    return [(x0 + s, yc - TH / 2), (x1 + s, yc - TH / 2),
            (x1 - s, yc + TH / 2), (x0 - s, yc + TH / 2)]


_raw_left = min(min(p[0] for p in bar_pts(a, b, y)) for (y, a, b, _) in BARS_RAW)
_raw_right = 650.0 + INK_R
DX = DESIGN_W / 2 - (_raw_left + _raw_right) / 2      # center the lockup
WORD_X = 650.0 + DX
BARS = [(y, a + DX, b + DX, c) for (y, a, b, c) in BARS_RAW]
LOCK_L, LOCK_R = _raw_left + DX, _raw_right + DX

# ---- fit that lockup into the 4:3 boot frame -------------------------------
TARGET_W = 800.0                                      # of 1024, leaving margins
SCALE = TARGET_W / (LOCK_R - LOCK_L)
OX = (W - TARGET_W) / 2 - LOCK_L * SCALE
OY = H / 2 - ((T_TOP + BASELINE) / 2) * SCALE
R = 40.0                                              # turtle cell radius


def X(v): return (OX + v * SCALE) * SS
def Y(v): return (OY + v * SCALE) * SS


# ---- timing, in frames -----------------------------------------------------
F_FADE   = (0, 8)        # black -> the shell appears
F_BACK   = (8, 20)       # reverse toward the left edge, coiling
F_SET    = (20, 24)      # held, loaded
F_ZIP    = (24, 46)      # launch and exit
F_WORD   = (0.46, 0.84)  # fractions OF the zip
F_BARS   = (0.60, 1.00)
F_SETTLE = (46, 56)      # the lockup eases the last of the way in
# 56..71 is a still hold: half a second, and the frame left on the panel.

START_CX, BACK_CX, EXIT_CX = 470.0, 250.0, 1560.0


def hexpts(cx, cy, r, sx, sy):
    return [(cx + r * math.cos(math.pi / 6 + i * math.pi / 3) * sx,
             cy + r * math.sin(math.pi / 6 + i * math.pi / 3) * sy) for i in range(6)]


def draw_turtle(d, cx, cy, r, sx, sy, alpha):
    """The shell, from tools/markdef.py - the one place its cells are defined."""
    dx, dy = math.sqrt(3) * r, 1.5 * r
    for (i, j, col) in list(CELLS) + [HEAD]:
        d.polygon(hexpts(cx + i * dx * sx, cy + j * dy * sy, r * 0.95, sx, sy),
                  fill=col + (alpha,), outline=(0, 0, 0, alpha), width=max(1, int(3 * SS * SCALE)))
    d.polygon(hexpts(cx, cy, r * 0.95, sx, sy),
              fill=CYAN + (alpha,), outline=(0, 0, 0, alpha), width=max(1, int(3 * SS * SCALE)))


def eo(t): return 1 - (1 - t) ** 3
def ei(t): return t * t * t
def cl(t): return max(0.0, min(1.0, t))
def lerp(a, b, t): return a + (b - a) * t
def span(n, lo, hi): return cl((n - lo) / float(hi - lo)) if hi > lo else 1.0


def render(n, font):
    im = Image.new("RGBA", (W * SS, H * SS), BG + (255,))
    d = ImageDraw.Draw(im, "RGBA")

    # --- the turtle ---------------------------------------------------------
    show, cx, sx, sy, a = True, START_CX, 1.0, 1.0, 255
    if n < F_FADE[1]:
        a = int(255 * span(n, F_FADE[0], F_FADE[1]))
    elif n < F_BACK[1]:
        e = eo(span(n, F_BACK[0], F_BACK[1]))
        cx = lerp(START_CX, BACK_CX, e); sx = lerp(1.0, 0.84, e); sy = lerp(1.0, 1.12, e)
    elif n < F_SET[1]:
        cx, sx, sy = BACK_CX, 0.84, 1.12
    elif n < F_ZIP[1]:
        t = span(n, F_ZIP[0], F_ZIP[1])
        cx = BACK_CX + (EXIT_CX - BACK_CX) * (ei(t / 0.26) * 0.10 if t < 0.26
                                              else 0.10 + eo((t - 0.26) / 0.74) * 0.90)
        if t < 0.16:
            k = eo(t / 0.16); sx = lerp(0.84, 1.34, k); sy = lerp(1.12, 1.00, k)
        else:
            k = eo(cl((t - 0.16) / 0.34)); sx = lerp(1.34, 1.00, k); sy = 1.0
        if OX + (cx - math.sqrt(3) * R * sx - R) * SCALE > W:
            show = False
    else:
        show = False
    if show and a > 3:
        draw_turtle(d, X(cx), Y(BASELINE - TCAP / 2), R * SCALE * SS, sx, sy, a)

    # --- the wordmark, sliding in behind him --------------------------------
    zt = span(n, F_ZIP[0], F_ZIP[1]) if n >= F_ZIP[0] else 0.0
    if n >= F_ZIP[1]:
        zt = 1.0
    wt = eo(cl((zt - F_WORD[0]) / (F_WORD[1] - F_WORD[0])))
    if n >= F_SETTLE[0]:
        wt = 1.0
    if wt > 0.01:
        wx = WORD_X - 430.0 * (1 - wt)
        L = Image.new("RGBA", im.size, (0, 0, 0, 0)); ld = ImageDraw.Draw(L)
        al = int(255 * wt)
        ld.polygon([(X(wx + px * K), Y(BASELINE - py * K)) for px, py in T_PTS],
                   fill=LTGRN + (al,))
        ox = wx + T_ADV * K + TUCK
        ld.text((X(ox), Y(BASELINE)), "ort", font=font, fill=LTGRN + (al,), anchor="ls")
        tw = ld.textlength("ort", font=font)
        ld.text((X(ox) + tw, Y(BASELINE)), "OS", font=font, fill=CYAN + (al,), anchor="ls")
        im = Image.alpha_composite(im, L); d = ImageDraw.Draw(im, "RGBA")

    # --- the speed bars, in behind the wordmark -----------------------------
    lt = eo(cl((zt - F_BARS[0]) / (F_BARS[1] - F_BARS[0])))
    if n >= F_SETTLE[1]:
        lt = 1.0
    elif n >= F_SETTLE[0]:
        lt = max(lt, eo(span(n, F_SETTLE[0], F_SETTLE[1])))
    if lt > 0.01:
        off = 500.0 * (1 - lt)
        for (yc, x0, x1, col) in BARS:
            pts = [(X(px - off), Y(py)) for px, py in bar_pts(x0, x1, yc)]
            d.polygon(pts, fill=col + (int(255 * lt),))

    return im.convert("RGB").resize((W, H), Image.LANCZOS)


def find_ffmpeg():
    for cand in ("/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg"):
        if os.path.exists(cand):
            return cand
    found = shutil.which("ffmpeg")
    if not found:
        sys.exit("ffmpeg not found on PATH")
    return found


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    if os.path.isdir(TMP_DIR):
        shutil.rmtree(TMP_DIR)
    os.makedirs(TMP_DIR)
    font = ImageFont.truetype(FONT_PATH, int(FSIZE * SCALE * SS))

    first = None
    for i in range(N_FRAMES):
        frame = render(i, font)
        if i == 0:
            first = frame
        frame.save(os.path.join(TMP_DIR, "f%04d.png" % i))
        if (i + 1) % 12 == 0 or i == N_FRAMES - 1:
            print("  frame %2d/%d" % (i + 1, N_FRAMES))

    if first.convert("RGB").getextrema() != ((BG[0], BG[0]), (BG[1], BG[1]), (BG[2], BG[2])):
        sys.exit("frame 0 is not the flat background color")

    bmp = os.path.join(OUT_DIR, "bootlogo.bmp")
    png = os.path.join(OUT_DIR, "splash.png")
    first.convert("RGB").save(bmp)
    first.convert("RGB").save(png)

    mp4 = os.path.join(OUT_DIR, "tortos-boot.mp4")
    subprocess.run([
        find_ffmpeg(), "-y", "-loglevel", "error",
        "-framerate", str(FPS), "-start_number", "0",
        "-i", os.path.join(TMP_DIR, "f%04d.png"),
        "-frames:v", str(N_FRAMES), "-fps_mode", "passthrough",
        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "20",
        "-movflags", "+faststart", mp4,
    ], check=True)

    for path in (mp4, bmp, png):
        print("%s  %d bytes" % (path, os.path.getsize(path)))


if __name__ == "__main__":
    main()
