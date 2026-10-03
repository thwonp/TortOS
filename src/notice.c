/* SPDX-License-Identifier: MIT */
/* See notice.h for why the launcher draws this and Diatom shows it. */
#include <SDL.h>
#include <SDL_ttf.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "notice.h"
#include "platform.h"
#include "ui.h"

#define NOTICE_MAX_W 1024        /* Diatom refuses anything wider */
#define NOTICE_MAX_H 256

/* One pixel of the destination, straight alpha, BGRA byte order - which is
 * what SDL_PIXELFORMAT_ARGB8888 already is in memory on a little-endian host,
 * and both ends of this are little-endian. */
static void blend(uint8_t *dst, unsigned b, unsigned g, unsigned r, unsigned a)
{
	if (a == 0) return;
	if (a == 255) {
		dst[0] = (uint8_t)b; dst[1] = (uint8_t)g;
		dst[2] = (uint8_t)r; dst[3] = 255;
		return;
	}
	dst[0] = (uint8_t)((b * a + dst[0] * (255 - a) + 127) / 255);
	dst[1] = (uint8_t)((g * a + dst[1] * (255 - a) + 127) / 255);
	dst[2] = (uint8_t)((r * a + dst[2] * (255 - a) + 127) / 255);
	dst[3] = (uint8_t)(a + dst[3] * (255 - a) / 255);
}

/* Diatom composites this buffer in the panel's own pixels, so everything here
 * is in pixels - the size SDL_ttf renders at - not the launcher's units. */
static int line_px(ui_font_role role)
{
	TTF_Font *f = ui_font(role);
	return f ? TTF_FontLineSkip(f) : 0;
}

static int width_px(TTF_Font *f, const char *s)
{
	int w = 0;
	if (f && s) TTF_SizeUTF8(f, s, &w, NULL);
	return w;
}

static void draw_text(uint8_t *out, int ow, int oh, TTF_Font *f,
                      const char *s, int x, int y, SDL_Color col)
{
	SDL_Surface *t;
	int sx, sy;

	if (!f || !s || !*s) return;
	t = TTF_RenderUTF8_Blended(f, s, col);
	if (!t) return;

	/* Blended gives ARGB8888 with per-pixel alpha - the antialiasing is IN
	 * the alpha, so ignoring it would give the text hard jagged edges over
	 * the game. */
	for (sy = 0; sy < t->h; sy++) {
		const uint8_t *row = (const uint8_t *)t->pixels + (size_t)sy * t->pitch;
		int dy = y + sy;

		if (dy < 0 || dy >= oh) continue;
		for (sx = 0; sx < t->w; sx++) {
			int dx = x + sx;

			if (dx < 0 || dx >= ow) continue;
			blend(out + ((size_t)dy * ow + dx) * 4,
			      row[sx * 4 + 0], row[sx * 4 + 1],
			      row[sx * 4 + 2], row[sx * 4 + 3]);
		}
	}
	SDL_FreeSurface(t);
}

bool notice_render(const char *heading, const char *body, const char *path)
{
	/* The NAME is the line worth reading, so it gets the larger face and the
	 * brighter ink; "Achievement unlocked" is context and sits above it,
	 * smaller and dimmer. These were the other way round in the first
	 * version, which put the least interesting words in the biggest type -
	 * obvious the moment it was rendered and looked at, and invisible while
	 * it was only being reasoned about. UI_F_LABEL is 48 and UI_F_MENU 43. */
	TTF_Font *fh = ui_font(UI_F_MENU), *fb = ui_font(UI_F_LABEL);
	int padx, pady, w, h, x, y, corner, maxw;
	char headfit[192], bodyfit[192];
	uint8_t *px;
	FILE *f;
	unsigned char hdr[8];
	const SDL_Color WHITE = { 235, 235, 240, 255 };
	const SDL_Color DIM   = { 128, 168, 190, 255 };

	if (!fb) return false;
	if (!fh) fh = fb;

	/* Tighter than square. A notice sits over someone's game for four
	 * seconds, so it should be the words and little else - the first version
	 * used one padding value everywhere and read as a slab with text in the
	 * middle of it.
	 *
	 * The vertical is smaller than the horizontal because the two lines carry
	 * their own leading already, and the body's descender space is subtracted
	 * outright: TTF_FontDescent is negative, and without this the gap under
	 * the last line is a descender deep even when nothing descends. The same
	 * trick menu_draw uses to stop its rows riding high. */
	padx = line_px(UI_F_MENU) / 3;
	pady = line_px(UI_F_MENU) / 6;

	/* A margin off the panel edge as well as off Diatom's hard limit: a
	 * notice that runs the full width of the screen stops reading as a thing
	 * laid over the game and starts reading as part of it. */
	maxw = (int)(TORTOS_SCREEN_W * plat_scale()) * 5 / 6;
	if (maxw > NOTICE_MAX_W) maxw = NOTICE_MAX_W;
	maxw -= padx * 2;
	/* Not optional. Diatom REFUSES an overlay wider than the panel rather than
	 * clipping it (its ADR-0027), which is the right call there and means a
	 * long name would silently show nothing at all. 144 of the achievement
	 * titles in the 180-ROM test library are over 40 characters and the
	 * longest is 64 - "From Johnny, Harris, Brooklyn Bob, and Reggie! Yeah
	 * Even Reggie!" - so this is the common case, not the edge. */
	/* ui_fit_text measures in units, which round up: what fits maxw/scale
	 * units fits maxw pixels. */
	ui_fit_text(fb, body, bodyfit, sizeof bodyfit, (int)(maxw / plat_scale()));
	ui_fit_text(fh, heading, headfit, sizeof headfit, (int)(maxw / plat_scale()));
	body = bodyfit;
	heading = headfit;

	w = width_px(fb, body);
	x = width_px(fh, heading);
	if (x > w) w = x;
	w += padx * 2;
	h = line_px(UI_F_MENU) + line_px(UI_F_LABEL) + pady * 2
	    + TTF_FontDescent(fb) / 3;
	if (w > NOTICE_MAX_W) w = NOTICE_MAX_W;
	if (h > NOTICE_MAX_H) h = NOTICE_MAX_H;
	if (w <= 0 || h <= 0) return false;

	px = calloc((size_t)w * (size_t)h, 4);
	if (!px) return false;

	/* A dark slab at 85%, rounded the way every menu panel in the launcher is
	 * rounded. The first version chamfered the corners - one comparison per
	 * pixel - and the comment justified it as reading the same from a meter
	 * away. It does not read the same. It reads as a different piece of
	 * software borrowing the screen for a moment. */
	corner = padx;
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			int dx = x < corner ? corner - x : (x >= w - corner ? x - (w - corner - 1) : 0);
			int dy = y < corner ? corner - y : (y >= h - corner ? y - (h - corner - 1) : 0);
			unsigned a = 217;

			if (dx || dy) {
				/* Coverage from the distance to the arc, so the curve is
				 * antialiased rather than staircased. More than half a pixel
				 * outside is nothing; more than half inside is solid. */
				double d = sqrt((double)dx * dx + (double)dy * dy) - corner;

				if (d > 0.5) continue;
				if (d > -0.5) a = (unsigned)(a * (0.5 - d));
			}
			blend(px + ((size_t)y * w + x) * 4, 26, 22, 18, a);
		}
	}

	/* Both lines centered, not left-aligned. The slab is as wide as its wider
	 * line, so the shorter one sat against the left edge with a gap after it -
	 * which reads as text that failed to fill rather than as a caption. It is
	 * also what menu_draw does with a heading, so the two agree. */
	draw_text(px, w, h, fh, heading,
	          (w - width_px(fh, heading)) / 2, pady, DIM);
	draw_text(px, w, h, fb, body,
	          (w - width_px(fb, body)) / 2,
	          pady + line_px(UI_F_MENU), WHITE);

	f = fopen(path, "wb");
	if (!f) { free(px); return false; }
	memcpy(hdr, "DTOV", 4);
	hdr[4] = (unsigned char)w;  hdr[5] = (unsigned char)(w >> 8);
	hdr[6] = (unsigned char)h;  hdr[7] = (unsigned char)(h >> 8);
	if (fwrite(hdr, 1, sizeof hdr, f) != sizeof hdr ||
	    fwrite(px, 4, (size_t)w * (size_t)h, f) != (size_t)w * (size_t)h) {
		fclose(f);
		free(px);
		return false;
	}
	fclose(f);
	free(px);
	return true;
}
