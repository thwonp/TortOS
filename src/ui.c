/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include "ui.h"
#include "platform.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLOW_SIZE 192      /* the glow is drawn stretched; small is enough */
#define TEXT_CACHE 12

/* The type scale: one base size and a multiplier per role. The panel is
 * 1024x768 across about three inches, so the base is set for reading at arm's
 * length on a handheld rather than for a screen at desk distance, and the
 * ratios between the roles are what keeps a count from competing with a title.
 * UI_F_CARD is in card pixels, not screen pixels -- the card face is drawn at
 * 512 wide and shown at roughly two thirds of that. */
/* THE ONE SIZE THE LAUNCHER DRAWS AT, and 1.15 is here rather than 36.8 so it
 * stays legible as what it is: the largest of the three steps the Text Size row
 * used to offer, which was also the most readable of them.
 *
 * The row is gone. It ran 85% to 115%, and that band is too narrow to solve
 * anyone's problem - a player who cannot read the default is not rescued by
 * fifteen percent, and one who wants more games on screen is not served by
 * fifteen percent either. It changed how the launcher looked without changing
 * what anyone could do, while making every panel's fit a function of a
 * variable. The range once reached 1.50 and was cut to 1.15 because the
 * keyboard panel did not survive it: the panels size themselves from constants,
 * so a scale big enough to matter breaks their layout rather than stretching
 * it. That is what a large-text mode would have to fix first.
 *
 * Written as the same product it used to be evaluated as, so every font comes
 * out at exactly the point size 115% gave and nothing moves by a rounding
 * accident. */
#define FONT_BASE (32.0f * 1.15f)
static const float font_mul[UI_F_COUNT] = {
	[UI_F_TITLE] = 1.62f,   /* 52 */
	[UI_F_MENU]  = 1.34f,   /* 43 */
	[UI_F_LABEL] = 1.50f,   /* 48 */
	[UI_F_META]  = 1.00f,   /* 32 */
	[UI_F_CARD]  = 1.94f,   /* 62, in card pixels - the title IS the card */
};

static TTF_Font *fonts[UI_F_COUNT];
static TTF_Font *f_mark;

/* Panel pixels to the layout unit, from plat_scale() at ui_init. Text, marks
 * and cards are rasterized in pixels, so they are sharp on the panel, and
 * placed in units like everything else. At 1 the two are the same number and
 * every path below is the integer one it always was - the Brick draws exactly
 * what it drew before there was a scale. */
static float ts = 1.0f;

static int px(int v) { return ts == 1.0f ? v : (int)(v * ts + 0.5f); }
static int units(int v) { return ts == 1.0f ? v : (int)lroundf(v / ts); }
/* Widths round up, so whatever fits in units fits in pixels too. */
static int units_up(int v) { return ts == 1.0f ? v : (int)ceilf(v / ts); }
/* To the nearest panel pixel, so a texture drawn at 1/ts lands 1:1. */
static float snap(float v) { return roundf(v * ts) / ts; }
static char font_path_kept[512];
static SDL_Texture *glow_tex;

/* One title is redrawn on every frame while the rest of the screen changes
 * around it. Rendering TTF text is milliseconds; blitting a texture is not.
 * A tiny most-recently-used cache turns the first into the second. */
struct text_entry {
	TTF_Font *font;
	char str[256];
	unsigned rgb;
	SDL_Texture *tex;
	int w, h;
	unsigned used;
};
static struct text_entry cache[TEXT_CACHE];
static unsigned cache_clock;

static SDL_Texture *make_glow(SDL_Renderer *r)
{
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, GLOW_SIZE, GLOW_SIZE, 32,
	                                                SDL_PIXELFORMAT_ARGB8888);
	SDL_Texture *t;
	int x, y;
	if (!s) return NULL;
	for (y = 0; y < GLOW_SIZE; y++) {
		Uint32 *row = (Uint32 *)((Uint8 *)s->pixels + (size_t)y * s->pitch);
		for (x = 0; x < GLOW_SIZE; x++) {
			float dx = (x - GLOW_SIZE / 2.0f) / (GLOW_SIZE / 2.0f);
			float dy = (y - GLOW_SIZE / 2.0f) / (GLOW_SIZE / 2.0f);
			float d = sqrtf(dx * dx + dy * dy);
			float a = 1.0f - d;
			if (a < 0) a = 0;
			a = a * a * a;   /* a soft shoulder, no visible edge */
			row[x] = SDL_MapRGBA(s->format, 255, 255, 255, (Uint8)(a * 255.0f));
		}
	}
	t = SDL_CreateTextureFromSurface(r, s);
	SDL_FreeSurface(s);
	if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_ADD);
	return t;
}


bool ui_init(SDL_Renderer *r, const char *font_path)
{
	int i;

	if (TTF_Init() != 0) {
		fprintf(stderr, "ttf: %s\n", TTF_GetError());
		return false;
	}
	snprintf(font_path_kept, sizeof font_path_kept, "%s", font_path);
	ts = plat_scale();
	for (i = 0; i < UI_F_COUNT; i++) {
		int pt = (int)(FONT_BASE * font_mul[i] * ts + 0.5f);
		fonts[i] = TTF_OpenFont(font_path, pt);
		if (!fonts[i])
			fprintf(stderr, "font %s @%d: %s\n", font_path, pt, TTF_GetError());
	}
	glow_tex = make_glow(r);
	return fonts[UI_F_TITLE] != NULL;
}

static void glyphs_free(void);
static void digits_free(void);

void ui_quit(void)
{
	glyphs_free();
	digits_free();
	for (int i = 0; i < TEXT_CACHE; i++)
		if (cache[i].tex) { SDL_DestroyTexture(cache[i].tex); cache[i].tex = NULL; }
	if (glow_tex) { SDL_DestroyTexture(glow_tex); glow_tex = NULL; }
	for (int i = 0; i < UI_F_COUNT; i++)
		if (fonts[i]) { TTF_CloseFont(fonts[i]); fonts[i] = NULL; }
	if (f_mark) { TTF_CloseFont(f_mark); f_mark = NULL; }
	TTF_Quit();
}

TTF_Font *ui_font(ui_font_role role)
{
	if (role < 0 || role >= UI_F_COUNT) return NULL;
	return fonts[role];
}

int ui_font_line(ui_font_role role)
{
	TTF_Font *f = ui_font(role);
	return f ? units(TTF_FontLineSkip(f)) : 0;
}

int ui_font_height(ui_font_role role)
{
	return ui_font_box(ui_font(role));
}

int ui_font_box(TTF_Font *f) { return f ? units(TTF_FontHeight(f)) : 0; }
int ui_font_ascent(TTF_Font *f) { return f ? units(TTF_FontAscent(f)) : 0; }
int ui_font_descent(TTF_Font *f) { return f ? units(TTF_FontDescent(f)) : 0; }

int ui_font_cap(TTF_Font *f)
{
	int mnx, mxx, mny, mxy, adv;

	if (!f) return 0;
	if (TTF_GlyphMetrics(f, 'H', &mnx, &mxx, &mny, &mxy, &adv) == 0)
		return units(mxy);
	return ui_font_ascent(f);
}

static struct text_entry *text_get(SDL_Renderer *r, TTF_Font *f, const char *s,
                                  SDL_Color col)
{
	unsigned rgb = ((unsigned)col.r << 16) | ((unsigned)col.g << 8) | col.b;
	int i, oldest = 0;

	if (!f || !s || !*s) return NULL;
	for (i = 0; i < TEXT_CACHE; i++) {
		if (cache[i].tex && cache[i].font == f && cache[i].rgb == rgb &&
		    strcmp(cache[i].str, s) == 0) {
			cache[i].used = ++cache_clock;
			return &cache[i];
		}
		if (!cache[i].tex) { oldest = i; goto fill; }
		if (cache[i].used < cache[oldest].used) oldest = i;
	}
fill:
	if (cache[oldest].tex) SDL_DestroyTexture(cache[oldest].tex);
	memset(&cache[oldest], 0, sizeof cache[oldest]);
	{
		SDL_Surface *surf = TTF_RenderUTF8_Blended(f, s, col);
		if (!surf) return NULL;
		cache[oldest].tex = SDL_CreateTextureFromSurface(r, surf);
		if (cache[oldest].tex && ts != 1.0f)
			SDL_SetTextureScaleMode(cache[oldest].tex, SDL_ScaleModeNearest);
		cache[oldest].w = surf->w;
		cache[oldest].h = surf->h;
		SDL_FreeSurface(surf);
	}
	if (!cache[oldest].tex) return NULL;
	cache[oldest].font = f;
	cache[oldest].rgb = rgb;
	snprintf(cache[oldest].str, sizeof cache[oldest].str, "%s", s);
	cache[oldest].used = ++cache_clock;
	return &cache[oldest];
}

/* A cached line with its top-left at (x,y) in units, at its pixel size. */
static void text_copy(SDL_Renderer *r, const struct text_entry *e, float x, int y)
{
	if (ts == 1.0f) {
		SDL_RenderCopy(r, e->tex, NULL, &(SDL_Rect){ (int)x, y, e->w, e->h });
		return;
	}
	SDL_RenderCopyF(r, e->tex, NULL, &(SDL_FRect){
		snap(x), snap((float)y), e->w / ts, e->h / ts });
}

int ui_text(SDL_Renderer *r, TTF_Font *f, const char *s, int x, int y,
            int anchor, SDL_Color col)
{
	struct text_entry *e = text_get(r, f, s, col);
	float w;
	if (!e) return 0;
	SDL_SetTextureAlphaMod(e->tex, col.a);
	if (ts == 1.0f) {
		text_copy(r, e, anchor == 0 ? x - e->w / 2 : (anchor > 0 ? x - e->w : x), y);
		return e->w;
	}
	w = e->w / ts;
	text_copy(r, e, anchor == 0 ? x - w / 2 : (anchor > 0 ? x - w : x), y);
	return units_up(e->w);
}

int ui_text_width(TTF_Font *f, const char *s)
{
	int w = 0;
	if (f && s) TTF_SizeUTF8(f, s, &w, NULL);
	return units_up(w);
}

/* ---- tabular digits ---------------------------------------------------------
 *
 * The ten digits of a font, rendered once in white and tinted as they are
 * drawn, apart from the string cache: a clock would otherwise put a string a
 * second into a cache of twelve and push out the title it sits under. One slot
 * per font, and Now Playing uses one font. */
#define DIGIT_FONTS 3
static struct {
	TTF_Font    *f;
	SDL_Texture *tex[10];
	int          w[10], h;
} digits[DIGIT_FONTS];

static void digits_free(void)
{
	int i, d;

	for (i = 0; i < DIGIT_FONTS; i++) {
		for (d = 0; d < 10; d++)
			if (digits[i].tex[d]) SDL_DestroyTexture(digits[i].tex[d]);
		memset(&digits[i], 0, sizeof digits[i]);
	}
}

/* The width every digit is given: the widest one's advance. */
static int digit_cell(TTF_Font *f)
{
	int d, cell = 0, minx, maxx, miny, maxy, adv;

	for (d = 0; d < 10; d++)
		if (TTF_GlyphMetrics(f, (Uint16)('0' + d), &minx, &maxx, &miny, &maxy, &adv) == 0 &&
		    adv > cell)
			cell = adv;
	return cell;
}

static int digits_for(SDL_Renderer *r, TTF_Font *f)
{
	static const SDL_Color white = { 255, 255, 255, 255 };
	int i, d, slot = -1;

	for (i = 0; i < DIGIT_FONTS; i++) {
		if (digits[i].f == f) return i;
		if (!digits[i].f && slot < 0) slot = i;
	}
	if (slot < 0) {                      /* all taken: the first makes room */
		for (d = 0; d < 10; d++)
			if (digits[0].tex[d]) SDL_DestroyTexture(digits[0].tex[d]);
		memset(&digits[0], 0, sizeof digits[0]);
		slot = 0;
	}
	for (d = 0; d < 10; d++) {
		SDL_Surface *s = TTF_RenderGlyph_Blended(f, (Uint16)('0' + d), white);

		if (!s) continue;
		digits[slot].tex[d] = SDL_CreateTextureFromSurface(r, s);
		digits[slot].w[d] = s->w;
		digits[slot].h = s->h;
		SDL_FreeSurface(s);
	}
	digits[slot].f = f;
	return slot;
}

/* The next run of `s`: digits one at a time, anything else up to the next
 * digit. Returns its length, and the run in `out`. */
static size_t tab_run(const char *s, char *out, size_t n)
{
	size_t len = 1;

	if (!(*s >= '0' && *s <= '9'))
		while (s[len] && !(s[len] >= '0' && s[len] <= '9')) len++;
	if (len >= n) len = n - 1;
	memcpy(out, s, len);
	out[len] = '\0';
	return len;
}

int ui_text_tabular_width(TTF_Font *f, const char *s)
{
	char run[128];
	int w = 0, cell;

	if (!f || !s) return 0;
	cell = digit_cell(f);
	while (*s) {
		s += tab_run(s, run, sizeof run);
		w += run[0] >= '0' && run[0] <= '9' ? cell : ui_text_width(f, run);
	}
	return w;
}

int ui_text_tabular(SDL_Renderer *r, TTF_Font *f, const char *s, int x, int y,
                    int anchor, SDL_Color col)
{
	char run[128];
	int w, cell, at, k;

	if (!f || !s || !*s) return 0;
	w = ui_text_tabular_width(f, s);
	cell = digit_cell(f);
	k = digits_for(r, f);
	at = anchor == 0 ? x - w / 2 : (anchor > 0 ? x - w : x);
	while (*s) {
		s += tab_run(s, run, sizeof run);
		if (run[0] >= '0' && run[0] <= '9') {
			int d = run[0] - '0';
			SDL_Texture *t = digits[k].tex[d];

			if (t) {
				/* Centered. At the cell's right edge, tried 2026-09-29, a
				 * thin digit closed up on the digit after it and opened a
				 * gap on the other side instead: "- 1:00: 10". */
				SDL_Rect dst = { at + (cell - digits[k].w[d]) / 2, y,
				                 digits[k].w[d], digits[k].h };

				SDL_SetTextureColorMod(t, col.r, col.g, col.b);
				SDL_SetTextureAlphaMod(t, col.a);
				SDL_RenderCopy(r, t, NULL, &dst);
			}
			at += cell;
		} else {
			at += ui_text(r, f, run, at, y, -1, col);
		}
	}
	return w;
}

/* Tuned by eye, and the first is the one that matters - see ui.h.
 *
 * 70 px/s is slow enough to read at arm's length on a three-inch panel and
 * fast enough that a 300px overrun is done in four seconds. The pause at the
 * far end stops the reversal reading as jitter: without it the text arrives
 * and instantly leaves, which looks like a glitch rather than an end. */
#define MQ_FADE_PX     36          /* how far the edges dissolve */
#define MQ_FADE_STEP    3
#define MQ_HOLD_MS   1400          /* stillness before it starts */
#define MQ_END_MS     900          /* stillness at the far end */
#define MQ_SPEED_PXPS  70

/* How far into a ping-pong a given phase has got: still at 0 for a beat, out
 * to `over` at a walking pace, still again at the far end, then back.
 *
 * Shared rather than written twice. A long title scrolling sideways on the
 * shelf and a long description scrolling down a card are the same gesture, and
 * two copies of this arithmetic would drift the moment either was retuned. */
int ui_pingpong(int over, unsigned phase)
{
	int travel = over * 1000 / MQ_SPEED_PXPS;
	unsigned cycle, p;

	if (over <= 0) return 0;
	if (travel < 1) travel = 1;
	cycle = (unsigned)(MQ_HOLD_MS + travel + MQ_END_MS + travel);
	p     = phase % cycle;

	if (p < MQ_HOLD_MS) return 0;
	if (p < (unsigned)(MQ_HOLD_MS + travel))
		return (int)((p - MQ_HOLD_MS) * (unsigned)over / (unsigned)travel);
	if (p < (unsigned)(MQ_HOLD_MS + travel + MQ_END_MS)) return over;
	return over - (int)((p - MQ_HOLD_MS - travel - MQ_END_MS)
	                    * (unsigned)over / (unsigned)travel);
}

int ui_scrollthrough(int cycle, unsigned phase)
{
	int travel = cycle * 1000 / MQ_SPEED_PXPS;
	unsigned lap, p;

	if (cycle <= 0) return 0;
	if (travel < 1) travel = 1;
	lap = (unsigned)(MQ_HOLD_MS + travel);
	p   = phase % lap;
	if (p < MQ_HOLD_MS) return 0;
	return (int)((p - MQ_HOLD_MS) * (unsigned)cycle / (unsigned)travel);
}

/* The same cycle as ui_pingpong, phase for phase - it has to be, or a caller
 * would wake early, costing a frame, or late, holding back a scroll that should
 * already have started. */
unsigned ui_pingpong_wait(int over, unsigned phase)
{
	int travel = over * 1000 / MQ_SPEED_PXPS;
	unsigned cycle, p;

	if (over <= 0) return (unsigned)-1;         /* fits, so it never moves */
	if (travel < 1) travel = 1;
	cycle = (unsigned)(MQ_HOLD_MS + travel + MQ_END_MS + travel);
	p     = phase % cycle;

	if (p < MQ_HOLD_MS) return MQ_HOLD_MS - p;
	if (p < (unsigned)(MQ_HOLD_MS + travel)) return 0;
	if (p < (unsigned)(MQ_HOLD_MS + travel + MQ_END_MS))
		return (unsigned)(MQ_HOLD_MS + travel + MQ_END_MS) - p;
	return 0;
}

void ui_text_marquee(SDL_Renderer *r, TTF_Font *f, const char *s,
                     int x, int y, int w, unsigned phase, SDL_Color col)
{
	int tw = ui_text_width(f, s);
	int over, off, i;
	SDL_Rect clip, was;
	SDL_bool had;

	if (!f || !s || w <= 0) return;
	if (tw <= w) { ui_text(r, f, s, x, y, -1, col); return; }

	over = tw - w;
	off  = ui_pingpong(over, phase);

	/* Nested clips: the caller may already have one, and dropping it would
	 * let this draw outside whatever panel it sits in. */
	had = SDL_RenderIsClipEnabled(r);
	if (had) SDL_RenderGetClipRect(r, &was);
	clip.x = x; clip.y = y;
	clip.w = w; clip.h = ui_font_box(f);
	SDL_RenderSetClipRect(r, &clip);

	/* Faded at both edges, and faded in the TEXT rather than by laying a
	 * gradient of the background over it. The background here is a coverflow
	 * with a vignette, not a flat color, so anything painted on top would
	 * show as a band. Fading the glyphs works over whatever is behind them.
	 *
	 * Without it the text is chopped mid-stroke - and on the shelf the left
	 * chop lands right beside the heart, where it reads as damage rather than
	 * as more text.
	 *
	 * EACH SLICE IS DRAWN EXACTLY ONCE, at its own alpha, with the clip
	 * deciding which part of the text lands. The first attempt drew the whole
	 * string at full alpha and then re-drew the edges dimmer on top, which
	 * cannot subtract - blending only adds - and used BLENDMODE_NONE to try to
	 * force it, which replaces the destination wholesale and painted two solid
	 * white blocks where the fades should have been. */
	{
		struct text_entry *e = text_get(r, f, s, col);

		if (!e) { SDL_RenderSetClipRect(r, had ? &was : NULL); return; }

		/* A FADE MEANS "MORE TEXT THIS WAY", so each side only fades when
		 * there is something hidden on it. Both faded unconditionally at
		 * first, which put a soft left edge on a title resting at its
		 * beginning - nothing was hidden there, and it read as the name
		 * starting halfway through a word.
		 *
		 * The width is the amount hidden, capped. So it grows from nothing as
		 * the text pulls away and shrinks back to nothing as it returns,
		 * which also means neither end pops. */
		int lf = off < MQ_FADE_PX ? off : MQ_FADE_PX;
		int rf = (over - off) < MQ_FADE_PX ? (over - off) : MQ_FADE_PX;

		clip.x = x + lf;
		clip.w = w - lf - rf;
		if (clip.w > 0) {
			SDL_RenderSetClipRect(r, &clip);
			SDL_SetTextureAlphaMod(e->tex, col.a);
			text_copy(r, e, (float)(x - off), y);
		}

		for (i = 0; i < lf; i += MQ_FADE_STEP) {
			SDL_SetTextureAlphaMod(e->tex, (Uint8)(col.a * i / lf));
			clip.x = x + i;
			clip.w = lf - i < MQ_FADE_STEP ? lf - i : MQ_FADE_STEP;
			SDL_RenderSetClipRect(r, &clip);
			text_copy(r, e, (float)(x - off), y);
		}
		for (i = 0; i < rf; i += MQ_FADE_STEP) {
			int sw = rf - i < MQ_FADE_STEP ? rf - i : MQ_FADE_STEP;

			SDL_SetTextureAlphaMod(e->tex, (Uint8)(col.a * i / rf));
			clip.x = x + w - sw - i;
			clip.w = sw;
			SDL_RenderSetClipRect(r, &clip);
			text_copy(r, e, (float)(x - off), y);
		}
		SDL_SetTextureAlphaMod(e->tex, 255);
	}
	SDL_RenderSetClipRect(r, had ? &was : NULL);
}

void ui_fit_text(TTF_Font *f, const char *src, char *dst, size_t dstn,
                 int maxw)
{
	size_t n;

	snprintf(dst, dstn, "%s", src ? src : "");
	if (!f || ui_text_width(f, dst) <= maxw) return;

	n = strlen(dst);
	while (n > 0) {
		n--;
		while (n > 0 && ((unsigned char)dst[n] & 0xC0) == 0x80) n--;
		if (n + 3 >= dstn) continue;
		memcpy(dst + n, "...", 4);
		if (ui_text_width(f, dst) <= maxw) return;
		dst[n] = '\0';
	}
}

void ui_glow(SDL_Renderer *r, const SDL_Rect *rect, unsigned rgb, int alpha,
             float spread)
{
	SDL_Rect dst;
	int gw, gh;
	if (!glow_tex) return;
	gw = (int)(rect->w * spread);
	gh = (int)(rect->h * spread);
	dst.x = rect->x + rect->w / 2 - gw / 2;
	dst.y = rect->y + rect->h / 2 - gh / 2;
	dst.w = gw;
	dst.h = gh;
	SDL_SetTextureColorMod(glow_tex, (Uint8)(rgb >> 16), (Uint8)(rgb >> 8), (Uint8)rgb);
	SDL_SetTextureAlphaMod(glow_tex, (Uint8)alpha);
	SDL_RenderCopy(r, glow_tex, NULL, &dst);
}

/* How much of a band the boundary between two colors eats at its widest, in
 * band widths, half of it on each side.
 *
 * WIDEST IS HALFWAY THROUGH A MOVE, AND ZERO AT REST - see rail_blend. At 0
 * throughout, the bands are hard and a crisp edge wipes across the marker as it
 * moves; at 1 the strip is one long gradient. 0.40 gives the moving edge about
 * 30px of a 76px marker, which reads as a blend rather than as a seam. */
#define RAIL_BLEND 0.40f

/* The blend width for a marker `index` items along, which is RAIL_BLEND only
 * in the middle of a move and nothing at all at rest.
 *
 * A MARKER AT REST IS EXACTLY ONE BAND WIDE, SO ITS EDGES SIT ON THE
 * BOUNDARIES. A blend straddling those tints a resting marker with the system
 * before it and the system after it - about a pixel of each on the device,
 * which Eric spotted on 2026-09-17, and no width setting can fix it because the
 * contamination is where the marker ENDS, not how wide the ramp is.
 *
 * Nothing needs softening at rest either: the softening exists for the edge
 * that sweeps across during a move, and at rest there is no edge, only one
 * band filling the window. So the width follows the distance between bands -
 * triangular, zero at both ends, so the ramp hardens back to nothing as the
 * move lands, exactly as it grew out of nothing when it left. */
static float rail_blend(float index)
{
	float f = index - floorf(index);

	return RAIL_BLEND * (1.0f - fabsf(2.0f * f - 1.0f));
}

static unsigned rgb_mix(unsigned a, unsigned b, float t)
{
	unsigned out = 0;
	int i;

	for (i = 16; i >= 0; i -= 8) {
		float x = (float)((a >> i) & 0xff);
		float y = (float)((b >> i) & 0xff);

		out |= (unsigned)(int)(x + (y - x) * t + 0.5f) << i;
	}
	return out;
}

/* The color the strip carries `x` items along it.
 *
 * THE STRIP IS FIXED TO THE TRACK AND THE MARKER IS A WINDOW ONTO IT. Eric's
 * idea, 2026-09-17: a line made of every system's color in order, invisible
 * except where the marker is. So the color is a function of WHERE ON THE TRACK
 * a pixel is, never of where the marker is - which is what makes the wrap come
 * out right for free. Mid-wrap the marker's remains at the far end still sit
 * over the last system's band while its copy at the near end sits over the
 * first system's, so the two stubs are correctly different colors without
 * anything being told that a wrap is happening.
 *
 * Bands are one item wide and the marker is one band wide, so at rest it
 * covers exactly its own system's band, pure to both edges. A rail whose marker
 * is WIDER than a band, which is any long list, covers several: that is only
 * ever the games rail, where every item carries its system's color and the
 * whole strip is one color anyway.
 *
 * `blend` is a width, not a switch: at zero the bands meet at a hard edge and
 * neither division below is reached. */
static unsigned rail_hue_at(float x, int count, float blend,
                            ui_rail_hue hue, void *ctx)
{
	int i = (int)floorf(x);
	float f = x - (float)i;

	i = ((i % count) + count) % count;
	/* Half the blend sits above the boundary and half below it, so the mix is
	 * exactly even where two bands meet. */
	if (f < blend * 0.5f)
		return rgb_mix(hue(ctx, i), hue(ctx, (i - 1 + count) % count),
		               0.5f - f / blend);
	if (f > 1.0f - blend * 0.5f)
		return rgb_mix(hue(ctx, i), hue(ctx, (i + 1) % count),
		               0.5f - (1.0f - f) / blend);
	return hue(ctx, i);
}

/* One marker, cut down to its track and colored by what it is over.
 *
 * Clipped rather than kept inside, because the rail describes a RING: the shelf
 * wraps, so the marker has to be able to leave by one end while the copy a lap
 * behind it arrives at the other. Both are handed over as whole rectangles and
 * the track decides how much of each is seen.
 *
 * `t0` and `dir` say where the strip's origin is relative to the rectangle:
 * the horizontal rail counts from the track's left edge, the vertical one from
 * its BOTTOM, because that is where its first item sits.
 *
 * Runs of one color are filled as one rectangle, so a rail nobody is moving -
 * and every rail whose items share a color, which is every games rail - still
 * costs the single fill it always did. */
static void rail_seg(SDL_Renderer *r, const SDL_Rect *track, SDL_Rect seg,
                     bool vertical, float step, float blend, int count,
                     unsigned flat, ui_rail_hue hue, void *ctx)
{
	SDL_Rect q;
	int len, k, run = 0;
	unsigned held = flat;

	if (!SDL_IntersectRect(track, &seg, &q)) return;
	len = vertical ? q.h : q.w;
	for (k = 0; k <= len; k++) {
		unsigned rgb = held;

		if (k < len && hue) {
			int t = vertical ? track->y + track->h - 1 - (q.y + k)
			                 : q.x + k - track->x;

			/* THE PIXEL'S CENTER, NOT ITS NEAR EDGE, and the half is
			 * load-bearing rather than tidy. The marker's rectangle is
			 * rounded to whole pixels and the bands are not, so wherever
			 * round(i * step) lands BELOW i * step - four of the eleven
			 * systems, and the second position once Favorites makes it
			 * twelve - the marker's first pixel starts just inside the
			 * previous band and comes out that system's color. One pixel,
			 * spotted on the device 2026-09-17. A pixel covers [t, t+1),
			 * so asking what is under its middle answers for the pixel
			 * rather than for the boundary it happens to start on. */
			rgb = rail_hue_at(((float)t + 0.5f) / step, count, blend,
			                  hue, ctx);
		}
		if (k == 0) { held = rgb; continue; }
		if (k < len && rgb == held) continue;
		SDL_SetRenderDrawColor(r, (Uint8)(held >> 16), (Uint8)(held >> 8),
		                       (Uint8)held, 235);
		SDL_RenderFillRect(r, vertical
			? &(SDL_Rect){ q.x, q.y + run, q.w, k - run }
			: &(SDL_Rect){ q.x + run, q.y, k - run, q.h });
		run = k;
		held = rgb;
	}
}

/* How far along a track of `track_len` the marker sits, in pixels from the
 * track's start, with one lap of the ring returned in the same units.
 *
 * `index` IS CONTINUOUS AND COMES FROM cf.pos, not from a cursor. The cursor
 * changes the instant a button goes down and the cards take the whole of
 * ANIM_MS to follow, so a rail drawn from it does not merely jump - it jumps
 * EARLY, arriving while the shelf is still setting off. The same fault the
 * system name had before cf_label, and cf.pos is the same answer.
 *
 * At whole numbers this is the integer version's own answer to within the
 * rounding - the first item against the near end, the last flush against the
 * far one - so nothing moved at rest when it changed.
 *
 * Taken modulo the lap because `pos` runs PAST the ends and is put back by
 * step_anim only once the move lands: a wrap carries the marker off the end it
 * left by, and the caller's copy one lap behind brings it back on at the other.
 * A lap is count steps, not the track: the last item is flush with the end, so
 * there is one more step to travel before the first item comes round again. */
static float rail_at(float index, int count, int track_len, int seg_len,
                     float *lap)
{
	float step = (float)(track_len - seg_len) / (float)(count - 1);
	float p = step * (float)count;
	float o = fmodf(index * step, p);

	if (o < 0.0f) o += p;
	*lap = p;
	return o;
}

void ui_rail(SDL_Renderer *r, int screen_w, int screen_h, float index, int count,
             unsigned rgb, ui_rail_hue hue, void *ctx)
{
	/* Grown upward from where the old 3px bar's bottom edge sat, so matching
	 * the settings line's weight did not also move the rail. */
	int h = UI_BAR_H, y = screen_h - 23 - h;
	int track_x = 90, track_w = screen_w - track_x * 2;
	SDL_Rect track;
	int seg_w, off, lap;
	float o, fl, step, blend;

	if (count <= 1) return;
	seg_w = track_w / count;
	if (seg_w < 18) seg_w = 18;
	o = rail_at(index, count, track_w, seg_w, &fl);
	step = fl / (float)count;
	blend = rail_blend(index);
	/* Rounded to whole pixels HERE rather than compared as floats below: at
	 * rest on the last item the offset is the track's far end to within a
	 * rounding error either way, and a float comparison would draw the wrapped
	 * copy on one side of that error and not the other - a second marker
	 * appearing at the near end of a long list for no reason. */
	off = (int)(o + 0.5f);
	lap = (int)(fl + 0.5f);
	track = (SDL_Rect){ track_x, y, track_w, h };

	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
	/* The track carries twice the weight it used to, so it takes less alpha to
	 * say the same thing; brighter than this and an empty rail reads as a
	 * drawn element rather than as the absence of one. */
	SDL_SetRenderDrawColor(r, 255, 255, 255, 16);
	SDL_RenderFillRect(r, &track);
	rail_seg(r, &track, (SDL_Rect){ track_x + off, y, seg_w, h },
	         false, step, blend, count, rgb, hue, ctx);
	/* Only while the marker overhangs the far end, which is the only time the
	 * copy has anything to show. On a shelf whose segments tile the track -
	 * eleven systems, where a segment is one step wide - the two are exactly
	 * complementary and the marker crosses the seam without a break. On a long
	 * shelf a segment is many steps wide, so a stub shows at each end for the
	 * length of that one move; that seam is a genuine discontinuity in the
	 * list, and showing where it goes beats teleporting across it. */
	if (off > track_w - seg_w)
		rail_seg(r, &track, (SDL_Rect){ track_x + off - lap, y, seg_w, h },
		         false, step, blend, count, rgb, hue, ctx);
}

void ui_rail_v(SDL_Renderer *r, int screen_w, int screen_h, float index, int count,
               unsigned rgb, ui_rail_hue hue, void *ctx)
{
	int w = UI_BAR_H, x = 23;
	int track_y = 90, track_h = screen_h - track_y * 2;
	SDL_Rect track;
	int seg_h, off, lap, end;
	float o, fl, step, blend;

	(void)screen_w;
	if (count <= 1) return;
	seg_h = track_h / count;
	if (seg_h < 18) seg_h = 18;
	o = rail_at(index, count, track_h, seg_h, &fl);
	off = (int)(o + 0.5f);
	lap = (int)(fl + 0.5f);
	step = fl / (float)count;
	blend = rail_blend(index);
	/* Inverted: the first item sits at the BOTTOM and the last at the top, so
	 * the indicator travels the same way the shelf does. A rail that runs
	 * top-down under a shelf that runs bottom-up moves opposite the thumb.
	 * Which is all the inversion is - the offset is still measured from the
	 * first item, it is just subtracted from the far end rather than added to
	 * the near one. */
	end = track_y + track_h - seg_h;
	track = (SDL_Rect){ x, track_y, w, track_h };

	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(r, 255, 255, 255, 16);
	SDL_RenderFillRect(r, &track);
	rail_seg(r, &track, (SDL_Rect){ x, end - off, w, seg_h },
	         true, step, blend, count, rgb, hue, ctx);
	if (off > track_h - seg_h)
		rail_seg(r, &track, (SDL_Rect){ x, end - off + lap, w, seg_h },
		         true, step, blend, count, rgb, hue, ctx);
}

void ui_round_rect(SDL_Renderer *r, const SDL_Rect *q, int radius, SDL_Color col)
{
	int y;

	if (q->w <= 0 || q->h <= 0) return;
	if (radius * 2 > q->w) radius = q->w / 2;
	if (radius * 2 > q->h) radius = q->h / 2;
	if (radius < 0) radius = 0;

	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(r, col.r, col.g, col.b, col.a);
	SDL_RenderFillRect(r, &(SDL_Rect){ q->x, q->y + radius, q->w, q->h - radius * 2 });
	for (y = 0; y < radius; y++) {
		int dy = radius - y;
		int dx = radius - (int)(sqrt((double)(radius * radius - dy * dy)) + 0.5);
		SDL_RenderFillRect(r, &(SDL_Rect){ q->x + dx, q->y + y, q->w - dx * 2, 1 });
		SDL_RenderFillRect(r, &(SDL_Rect){ q->x + dx, q->y + q->h - 1 - y,
		                                   q->w - dx * 2, 1 });
	}
}

/* The border is the system's color at full strength, not a wash of it. At
 * alpha 110 over a near-black background it read as a darker version of the
 * accent rather than the accent, so a system's identity barely reached the one
 * piece of chrome that frames everything it does. Twelve pixels rather than two
 * for the same reason: at two it was a hairline that the eye resolved as gray.
 *
 * Twelve specifically, matching the save/load frame's `bw`, so the two pieces
 * of accent chrome a player sees are the same weight rather than nearly so. */

void ui_panel(SDL_Renderer *r, const SDL_Rect *q, int radius, unsigned border)
{
	const int bw = UI_PANEL_BORDER;
	SDL_Rect in = { q->x + bw, q->y + bw, q->w - bw * 2, q->h - bw * 2 };

	ui_round_rect(r, q, radius, (SDL_Color){
		(Uint8)(border >> 16), (Uint8)(border >> 8), (Uint8)border, 255 });
	/* Opaque enough that a card title behind it does not ghost through the
	 * list, which at 95% it did. Lifted off the background's own near-black:
	 * at 10,11,16 the panel was a hole in the screen rather than a surface on
	 * it, and every row drawn on it inherited that as looking unlit. */
	ui_round_rect(r, &in, radius - bw, (SDL_Color){ 22, 24, 32, 252 });
}

unsigned ui_mix(unsigned a, unsigned b, float t)
{
	int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
	int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
	int rr = (int)(ar + (br - ar) * t);
	int rg = (int)(ag + (bg - ag) * t);
	int rb = (int)(ab + (bb - ab) * t);
	return ((unsigned)rr << 16) | ((unsigned)rg << 8) | (unsigned)rb;
}

/* ---- the generated card ------------------------------------------------- */

#define CARD_W 512
#define CARD_H 656
#define CARD_RADIUS 22

static void round_corners(SDL_Surface *s, int radius)
{
	int x, y;
	for (y = 0; y < radius; y++) {
		Uint32 *top = (Uint32 *)((Uint8 *)s->pixels + (size_t)y * s->pitch);
		Uint32 *bot = (Uint32 *)((Uint8 *)s->pixels + (size_t)(s->h - 1 - y) * s->pitch);
		for (x = 0; x < radius; x++) {
			int dx = radius - x, dy = radius - y;
			if (dx * dx + dy * dy > radius * radius) {
				top[x] = 0; top[s->w - 1 - x] = 0;
				bot[x] = 0; bot[s->w - 1 - x] = 0;
			}
		}
	}
}

static void blit_line(SDL_Surface *dst, TTF_Font *f, const char *line, int *y,
                      int x)
{
	SDL_Surface *t = TTF_RenderUTF8_Blended(f, line, UI_TEXT);
	if (!t) return;
	SDL_BlitSurface(t, NULL, dst,
	                &(SDL_Rect){ x < 0 ? (dst->w - t->w) / 2 : x, *y, 0, 0 });
	*y += t->h + px(4);
	SDL_FreeSurface(t);
}

/* Break a title across lines that fit the card. `x` is where each line starts,
 * or -1 to center them. The line is built by appending in place and undoing the
 * append when it no longer fits, so there is no second buffer that could
 * truncate the first. Returns the y below the last line, which is where a rule
 * under the title goes. */
static int draw_wrapped(SDL_Surface *dst, TTF_Font *f, const char *title,
                        int box_w, int top_y, int x)
{
	char line[256];
	char word[96];
	const char *p = title;
	size_t len = 0;
	int y = top_y;
	int lines = 0;

	line[0] = '\0';
	while (*p && lines < 4) {
		size_t n = 0, keep;
		int w = 0;

		while (*p == ' ') p++;
		while (*p && *p != ' ' && n + 1 < sizeof word) word[n++] = *p++;
		while (*p && *p != ' ') p++;          /* drop the tail of a huge word */
		word[n] = '\0';
		if (!n) break;

		keep = len;
		if (len + (len ? 1 : 0) + n + 1 > sizeof line) {
			/* the line cannot hold another word at all */
			if (len) { blit_line(dst, f, line, &y, x); lines++; }
			snprintf(line, sizeof line, "%s", word);
			len = n;
			continue;
		}
		if (len) line[len++] = ' ';
		memcpy(line + len, word, n);
		len += n;
		line[len] = '\0';

		TTF_SizeUTF8(f, line, &w, NULL);
		if (w > box_w && keep) {
			line[keep] = '\0';                /* undo the append */
			blit_line(dst, f, line, &y, x);
			lines++;
			snprintf(line, sizeof line, "%s", word);
			len = n;
		}
	}
	if (line[0] && lines < 4) blit_line(dst, f, line, &y, x);
	return y;
}

/* The first letter of the title, enormous and barely there, running off the
 * bottom-right corner.
 *
 * It used to sit centered and upright at a third of the way down, which made it
 * the largest thing on the card - and on an alphabetized shelf it is the least
 * distinguishing: Castlevania, Contra and Crystalis sit next to each other and
 * were three identical Cs with the titles that tell them apart set small
 * underneath. Bled off the corner it is what it always was, a texture in the
 * system's color, and the title can have the space. */
static void draw_watermark(SDL_Surface *dst, const char *title, unsigned rgb)
{
	char ch[2] = { 0, 0 };
	const char *p = title;
	SDL_Surface *t;

	while (*p && (unsigned char)*p <= ' ') p++;
	if (!*p) return;
	ch[0] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
	if ((unsigned char)ch[0] > 127) return;   /* one glyph, and an ASCII one */

	if (!f_mark && font_path_kept[0])
		f_mark = TTF_OpenFont(font_path_kept, px(560));
	if (!f_mark) return;

	t = TTF_RenderUTF8_Blended(f_mark, ch, (SDL_Color){
		(Uint8)(rgb >> 16), (Uint8)(rgb >> 8), (Uint8)rgb, 55 });
	if (!t) return;
	SDL_SetSurfaceBlendMode(t, SDL_BLENDMODE_BLEND);
	/* Deliberately past both edges: what is wanted is the shoulder of the
	 * letter, not the letter. */
	SDL_BlitSurface(t, NULL, dst, &(SDL_Rect){
		dst->w - (int)(t->w * 0.62f), dst->h - (int)(t->h * 0.80f), 0, 0 });
	SDL_FreeSurface(t);
}

/* The card at any size: ui_make_card is a game's, ui_make_cover an album's.
 * One recipe, so an album with no art reads as the same kind of thing as a
 * game with none - except the corners, which an album's does not round: it
 * stands among square sleeves, where a game's stands among cards. */
static SDL_Texture *make_card(SDL_Renderer *r, const char *title, unsigned rgb,
                              int cw, int ch, bool round, int *w, int *h)
{
	/* Baked at the panel's pixels, and every size in here with it: callers
	 * take the card's size as its shape, the same as for box art. */
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, cw = px(cw), ch = px(ch),
	                                                32, SDL_PIXELFORMAT_ARGB8888);
	SDL_Texture *t;
	int y;
	if (!s) return NULL;

	/* A vertical gradient, darker at the foot, so a wall of generated cards
	 * still has some depth to it. */
	for (y = 0; y < ch; y++) {
		float k = (float)y / ch;
		Uint8 v = (Uint8)(38 - 18 * k);
		SDL_FillRect(s, &(SDL_Rect){ 0, y, cw, 1 },
		             SDL_MapRGBA(s->format, (Uint8)(v * 0.86f), (Uint8)(v * 0.92f),
		                         (Uint8)(v * 1.20f), 255));
	}

	draw_watermark(s, title, rgb);

	/* The system's color as a band rather than a wash: a generated card
	 * should read as "this system, no art" at a glance in the row. */
	SDL_FillRect(s, &(SDL_Rect){ 0, 0, cw, px(6) },
	             SDL_MapRGBA(s->format, (Uint8)(rgb >> 16), (Uint8)(rgb >> 8),
	                         (Uint8)rgb, 255));

	/* The title at the top and hard left, because that is the edge the eye
	 * runs down when the row is moving. A rule under it rather than across
	 * the card: it is the end of the name, not a divider between halves. */
	if (fonts[UI_F_CARD]) {
		/* Not at the top edge. Started at 76 it left the bottom half of the
		 * card empty for the one-line titles that are most of a shelf, and the
		 * bleed does not fill it - the face darkens toward the foot and takes
		 * the letter's tail with it. Four lines still fit below this. */
		int y = draw_wrapped(s, fonts[UI_F_CARD], title, cw - px(96),
		                     (int)(ch * 0.38f), px(48));

		SDL_FillRect(s, &(SDL_Rect){ px(48), y + px(12), px(90), px(3) },
		             SDL_MapRGBA(s->format, (Uint8)(rgb >> 16),
		                         (Uint8)(rgb >> 8), (Uint8)rgb, 220));
	}

	if (round) round_corners(s, px(CARD_RADIUS));
	t = SDL_CreateTextureFromSurface(r, s);
	SDL_FreeSurface(s);
	if (t) { *w = cw; *h = ch; SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND); }
	return t;
}

SDL_Texture *ui_make_card(SDL_Renderer *r, const char *title, unsigned rgb,
                          int *w, int *h)
{
	return make_card(r, title, rgb, CARD_W, CARD_H, true, w, h);
}

SDL_Texture *ui_make_cover(SDL_Renderer *r, const char *title, unsigned rgb,
                           int *w, int *h)
{
	return make_card(r, title, rgb, CARD_W, CARD_W, false, w, h);
}


/* ---- the play modes' marks ---------------------------------------------- */

/* Each mark is a few strokes and a few arrowheads in a unit square, y down,
 * rendered into a white texture with 4x4 supersampling - which is the whole of
 * the antialiasing - and tinted when drawn. Strokes are segments with round
 * ends; a curve is a few of them. */

typedef struct { float x0, y0, x1, y1; } g_seg;
typedef struct { float ax, ay, bx, by, cx, cy; } g_tri;

#define G_W   0.055f            /* half a stroke */
#define G_MAX 24

typedef struct {
	g_seg seg[G_MAX];
	g_tri tri[4];
	int   nseg, ntri;
	float w[G_MAX];              /* each stroke's half width */
} g_shape;

static void g_line(g_shape *s, float x0, float y0, float x1, float y1, float w)
{
	if (s->nseg == G_MAX) return;
	s->w[s->nseg] = w;
	s->seg[s->nseg++] = (g_seg){ x0, y0, x1, y1 };
}

/* A quarter of a circle as six strokes, from angle a0 to a1 (radians, y down). */
static void g_arc(g_shape *s, float cx, float cy, float r, float a0, float a1)
{
	int i;

	for (i = 0; i < 6; i++) {
		float t0 = a0 + (a1 - a0) * (float)i / 6.0f;
		float t1 = a0 + (a1 - a0) * (float)(i + 1) / 6.0f;

		g_line(s, cx + r * cosf(t0), cy + r * sinf(t0),
		       cx + r * cosf(t1), cy + r * sinf(t1), G_W);
	}
}

/* An arrowhead with its tip at (tx, ty), pointing along (dx, dy), a unit
 * vector. */
static void g_head(g_shape *s, float tx, float ty, float dx, float dy)
{
	float bx = tx - dx * 0.17f, by = ty - dy * 0.17f;
	float px = -dy * 0.13f, py = dx * 0.13f;

	if (s->ntri == 4) return;
	s->tri[s->ntri++] = (g_tri){ tx, ty, bx + px, by + py, bx - px, by - py };
}

static void g_build(ui_glyph g, g_shape *s)
{
	const float pi = 3.14159265f;

	memset(s, 0, sizeof *s);
	if (g == UI_GLYPH_LOCK) {
		/* A padlock: the shackle, a half circle on two legs, over a body
		 * filled with wide strokes whose round ends round its corners.
		 * Close enough together that the sides come out straight. */
		float y;

		g_arc(s, 0.50f, 0.34f, 0.17f, pi, 1.5f * pi);
		g_arc(s, 0.50f, 0.34f, 0.17f, 1.5f * pi, 2.0f * pi);
		g_line(s, 0.33f, 0.34f, 0.33f, 0.48f, G_W);
		g_line(s, 0.67f, 0.34f, 0.67f, 0.48f, G_W);
		for (y = 0.56f; y < 0.77f; y += 0.05f)
			g_line(s, 0.32f, y, 0.68f, y, 0.10f);
		return;
	}
	if (g == UI_GLYPH_SHUFFLE) {
		/* Two paths that cross, both arriving on the right. */
		g_line(s, 0.10f, 0.32f, 0.30f, 0.32f, G_W);
		g_line(s, 0.30f, 0.32f, 0.60f, 0.68f, G_W);
		g_line(s, 0.60f, 0.68f, 0.72f, 0.68f, G_W);
		g_head(s, 0.89f, 0.68f, 1.0f, 0.0f);
		g_line(s, 0.10f, 0.68f, 0.30f, 0.68f, G_W);
		g_line(s, 0.30f, 0.68f, 0.60f, 0.32f, G_W);
		g_line(s, 0.60f, 0.32f, 0.72f, 0.32f, G_W);
		g_head(s, 0.89f, 0.32f, 1.0f, 0.0f);
		return;
	}
	/* The loop: two arrows chasing each other round a rounded rectangle, one
	 * up the left and along the top, one down the right and along the
	 * bottom. */
	g_line(s, 0.12f, 0.60f, 0.12f, 0.42f, G_W);
	g_arc(s, 0.30f, 0.42f, 0.18f, pi, 1.5f * pi);
	g_line(s, 0.30f, 0.24f, 0.68f, 0.24f, G_W);
	g_head(s, 0.86f, 0.24f, 1.0f, 0.0f);
	g_line(s, 0.88f, 0.40f, 0.88f, 0.58f, G_W);
	g_arc(s, 0.70f, 0.58f, 0.18f, 0.0f, 0.5f * pi);
	g_line(s, 0.70f, 0.76f, 0.32f, 0.76f, G_W);
	g_head(s, 0.14f, 0.76f, -1.0f, 0.0f);
	if (g == UI_GLYPH_REPEAT_ONE) {
		/* A 1 in the middle, lighter than the loop and clear of it. */
		g_line(s, 0.50f, 0.38f, 0.50f, 0.62f, 0.04f);
		g_line(s, 0.50f, 0.38f, 0.445f, 0.43f, 0.04f);
	}
}

static int g_inside(const g_shape *s, float x, float y)
{
	int i;

	for (i = 0; i < s->nseg; i++) {
		const g_seg *g = &s->seg[i];
		float vx = g->x1 - g->x0, vy = g->y1 - g->y0;
		float len2 = vx * vx + vy * vy;
		float t = len2 > 0 ? ((x - g->x0) * vx + (y - g->y0) * vy) / len2 : 0;
		float dx, dy;

		if (t < 0) t = 0;
		if (t > 1) t = 1;
		dx = x - (g->x0 + t * vx);
		dy = y - (g->y0 + t * vy);
		if (dx * dx + dy * dy <= s->w[i] * s->w[i]) return 1;
	}
	for (i = 0; i < s->ntri; i++) {
		const g_tri *t = &s->tri[i];
		float d1 = (x - t->bx) * (t->ay - t->by) - (t->ax - t->bx) * (y - t->by);
		float d2 = (x - t->cx) * (t->by - t->cy) - (t->bx - t->cx) * (y - t->cy);
		float d3 = (x - t->ax) * (t->cy - t->ay) - (t->cx - t->ax) * (y - t->ay);
		int neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;

		if (!(neg && pos)) return 1;
	}
	return 0;
}

static SDL_Texture *g_render(SDL_Renderer *r, ui_glyph g, int size)
{
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32,
	                                                SDL_PIXELFORMAT_ARGB8888);
	SDL_Texture *t;
	g_shape shape;
	int x, y, i, j;

	if (!s) return NULL;
	g_build(g, &shape);
	for (y = 0; y < size; y++) {
		Uint32 *row = (Uint32 *)((Uint8 *)s->pixels + (size_t)y * s->pitch);

		for (x = 0; x < size; x++) {
			int hit = 0;

			for (j = 0; j < 4; j++)
				for (i = 0; i < 4; i++)
					hit += g_inside(&shape, ((float)x + ((float)i + 0.5f) / 4.0f) / (float)size,
					                ((float)y + ((float)j + 0.5f) / 4.0f) / (float)size);
			row[x] = (Uint32)(hit * 255 / 16) << 24 | 0x00FFFFFFu;
		}
	}
	t = SDL_CreateTextureFromSurface(r, s);
	SDL_FreeSurface(s);
	if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
	return t;
}

/* A handful of (mark, size) pairs is all a screen asks for. */
static struct { ui_glyph g; int size; SDL_Texture *tex; } g_cache[8];

static void glyphs_free(void)
{
	int i;

	for (i = 0; i < 8; i++)
		if (g_cache[i].tex) { SDL_DestroyTexture(g_cache[i].tex); g_cache[i].tex = NULL; }
}

void ui_glyph_draw(SDL_Renderer *r, ui_glyph g, int cx, int cy, int size,
                   SDL_Color col)
{
	SDL_Texture *t = NULL;
	int i, free_slot = -1, unit = size;

	if (g < 0 || g >= UI_GLYPH_COUNT || size <= 0) return;
	size = px(size);   /* the cache and the raster are in pixels */
	for (i = 0; i < 8; i++) {
		if (g_cache[i].tex && g_cache[i].g == g && g_cache[i].size == size) {
			t = g_cache[i].tex;
			break;
		}
		if (!g_cache[i].tex && free_slot < 0) free_slot = i;
	}
	if (!t) {
		if (free_slot < 0) { glyphs_free(); free_slot = 0; }
		t = g_render(r, g, size);
		if (!t) return;
		if (ts != 1.0f) SDL_SetTextureScaleMode(t, SDL_ScaleModeNearest);
		g_cache[free_slot].g = g;
		g_cache[free_slot].size = size;
		g_cache[free_slot].tex = t;
	}
	SDL_SetTextureColorMod(t, col.r, col.g, col.b);
	SDL_SetTextureAlphaMod(t, col.a);
	if (ts == 1.0f) {
		SDL_RenderCopy(r, t, NULL, &(SDL_Rect){ cx - size / 2, cy - size / 2, size, size });
		return;
	}
	SDL_RenderCopyF(r, t, NULL, &(SDL_FRect){ snap(cx - unit / 2.0f),
		snap(cy - unit / 2.0f), size / ts, size / ts });
}
