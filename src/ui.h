/* SPDX-License-Identifier: MIT */
#ifndef TORTOS_UI_H
#define TORTOS_UI_H

#include <SDL.h>
#include <SDL_ttf.h>
#include <stdbool.h>

/* The look: near-black, one accent per system, and nothing on screen that is
 * not either a card, the name of what is under the cursor, or a rail saying
 * where you are in the list. */

#define UI_BG_R 7
#define UI_BG_G 8
#define UI_BG_B 12

/* Four steps, brightest first. A list is read at all of them at once, so the
 * gaps have to be wide enough to rank the rows and narrow enough that the
 * quietest step is still text rather than texture. SOFT is where an ordinary
 * unselected row sits: selection is said by the highlight behind it, not by
 * dimming everything else down to it. */
#define UI_TEXT      ((SDL_Color){ 237, 237, 242, 255 })
#define UI_TEXT_SOFT ((SDL_Color){ 198, 201, 214, 255 })
#define UI_TEXT_DIM  ((SDL_Color){ 148, 153, 172, 255 })

/* TortOS cyan: 0x3DD6FF. The one accent - menu chrome, volume OSD, and the
 * mark's center cell, where tools/markdef.py names it CYAN. The per-system
 * accents come from systems.cfg and are a separate thing. */
#define UI_CYAN_R 61
#define UI_CYAN_G 214
#define UI_CYAN_B 255

/* An earned achievement, in the cheevos list and on its detail card.
 *
 * The TortOS cyan rather than the system accent, which is what this used to
 * be. The accent only ever reached the SELECTED row's points, so every other
 * earned row was separated from an unearned one by a single brightness step
 * and the list could not be scanned at all. Worse on the dark accents - Game
 * Boy 7E9B47, Master System C12216, NES C4443A - against a near-black panel.
 * Cyan is the one hue none of the eleven accents occupies, so an earned row
 * reads the same on every system. */
#define UI_EARNED     ((SDL_Color){ UI_CYAN_R, UI_CYAN_G, UI_CYAN_B, 255 })
#define UI_EARNED_RGB 0x3DD6FFu

/* One thickness for every horizontal indicator: the settings line across the
 * top and the position rail across the bottom are the same bar in two places,
 * so they are the same weight. */
#define UI_BAR_H 6

/* The settings line is tinted by which setting it is. Brightness is the color
 * of light, volume is the launcher's own cyan -- fixed per function rather
 * than taken from the system accent, which would make one control change
 * color as you scrolled past it. */
#define UI_OSD_BRIGHT ((SDL_Color){ 255, 206, 128, 255 })
#define UI_OSD_VOLUME ((SDL_Color){  61, 214, 255, 255 })

bool ui_init(SDL_Renderer *r, const char *font_path);
void ui_quit(void);

/* The type scale. One base size, a multiplier per role, and a global user
 * scale over the top -- so retuning a role is one number here instead of a
 * size hunted down at every call site, and the whole scale can move together,
 * which on a panel this dense is usually what is wanted.
 *
 * UI_F_CARD is drawn into the 512px-wide generated card rather than onto the
 * screen, so it is sized for the card and not for the panel. */
typedef enum {
	UI_F_TITLE,   /* the name of the thing under the cursor */
	UI_F_MENU,    /* menu rows */
	UI_F_LABEL,   /* headings, slot names */
	UI_F_META,    /* counts, timestamps -- the quiet line */
	UI_F_CARD,
	UI_F_BADGE,   /* the battery's number, small enough for the corner */
	UI_F_COUNT
} ui_font_role;

/* The font for a role. One size: the Text Size setting is gone, and src/ui.c
 * says why. */
TTF_Font *ui_font(ui_font_role role);
/* Baseline-to-baseline distance for a role, the unit menu rows are laid out in. */
int ui_font_line(ui_font_role role);

/* The height of a line's box, which is not its line skip: menu_draw sizes a
 * one-line note by this, and a screen that wants to know whether its rows fit
 * has to ask the same question the same way. */
int ui_font_height(ui_font_role role);

/* A font's own metrics, in layout units like everything else here. Fonts
 * are opened at the panel's pixel size (see plat_scale), so SDL_ttf's answers
 * are in pixels; these are what to ask instead. Descent is negative, as
 * SDL_ttf's is. Cap is the height of 'H' above the baseline - the top of the
 * ink - or the ascent if the font has no 'H'. */
int ui_font_box(TTF_Font *f);
int ui_font_ascent(TTF_Font *f);
int ui_font_descent(TTF_Font *f);
int ui_font_cap(TTF_Font *f);

/* Draw text with its top-left at (x,y). anchor: -1 left, 0 center, 1 right,
 * applied to x. Returns the drawn width. Rendering is cached per (font,
 * string), so redrawing the same title every frame costs one blit. */
int ui_text(SDL_Renderer *r, TTF_Font *f, const char *s, int x, int y,
            int anchor, SDL_Color col);

/* The same color at a fraction of its opacity, for text that fades in or out.
 * Clamped, so a caller doing its own easing cannot overshoot into a wrapped
 * alpha byte. */
static inline SDL_Color ui_fade(SDL_Color c, float k)
{
	if (k < 0.0f) k = 0.0f;
	if (k > 1.0f) k = 1.0f;
	c.a = (Uint8)(c.a * k + 0.5f);
	return c;
}
int ui_text_width(TTF_Font *f, const char *s);

/* ui_text with every digit the same width, the way tabular figures are: for a
 * number that changes while it is on screen, so a clock ticking from 1:19 to
 * 1:20 does not shift what is beside it. Josefin Sans has proportional digits
 * and no tnum feature to turn on - measured 2026-09-28, a 1 is 329 units wide
 * and a 0 is 623 - so each digit is drawn centered in a cell as wide as the
 * widest one, and everything else in the string as it always is. */
int ui_text_tabular(SDL_Renderer *r, TTF_Font *f, const char *s, int x, int y,
                    int anchor, SDL_Color col);
int ui_text_tabular_width(TTF_Font *f, const char *s);

/* The ping-pong offset a marquee is at, in pixels, for `phase` ms into it.
 * Exposed so a panel that scrolls itself vertically keeps the same timing as
 * a title that scrolls sideways. */
int ui_pingpong(int over, unsigned phase);

/* How many ms until ui_pingpong's answer for `over` next changes, from `phase`:
 * 0 while it is moving, the rest of the hold while it is standing still.
 *
 * So a caller that only draws when something changes can sleep through the
 * holds. A marquee ping-pongs for as long as its title is focused, and without
 * this it would keep the whole screen redrawing at the refresh rate the entire
 * time, through 1.4s of stillness at one end and 0.9s at the other. */
unsigned ui_pingpong_wait(int over, unsigned phase);

/* Copy `src` into `dst`, shortened with an ellipsis until it fits `maxw`
 * pixels in `f`. Bytes are stepped back one at a time and then walked off any
 * UTF-8 continuation, so a multi-byte character is never cut in half.
 *
 * One implementation because there is one rule. This lived inside notice.c,
 * where a name too long for an overlay had to be trimmed; the box art screen
 * needs exactly the same thing for a ROM name too long for a panel, and a
 * second copy would be a second set of decisions about where the dots go. */
void ui_fit_text(TTF_Font *f, const char *src, char *dst, size_t dstn,
                 int maxw);

/* Draw `s` left-aligned at (x,y), clipped to `w` pixels, sliding it back and
 * forth when it does not fit.
 *
 * Truncation destroys information permanently; this only delays it. `phase` is
 * milliseconds since the subject last changed - the caller owns that clock,
 * because only the caller knows what "the subject" is (a shelf cursor moving,
 * a menu selection changing).
 *
 * IT WAITS BEFORE IT MOVES, and that is the whole design. Started immediately
 * it is motion competing with a coverflow already easing under the player's
 * thumb - worse than a clipped title. Waiting means flicking through a shelf
 * never triggers it and stopping on a game always does, which turns it from
 * decoration into an answer to a question just asked by stopping.
 *
 * Ping-pong rather than a loop: a loop needs a gap and a wrap, and on a short
 * string you cannot tell where the title ended and restarted. This rests at
 * the beginning, so the idle state is always canonical.
 *
 * Costs nothing this launcher was not already paying. Every screen redraws
 * every frame, and ui_text caches the rendered texture per (font, string,
 * color) - so sliding it is a moving destination rect, not a re-render. */
void ui_text_marquee(SDL_Renderer *r, TTF_Font *f, const char *s,
                     int x, int y, int w, unsigned phase, SDL_Color col);

/* The same travel, but one way and round again: hold at the top, scroll at the
 * same pace to `cycle`, then start over from nothing.
 *
 * For a body that REPEATS - the caller lays the content out twice with a rule
 * between the copies, so the wrap at `cycle` lands on a picture identical to
 * the one at zero and cannot be seen. Ping-pong is right for a row of text too
 * wide for its column, where running backwards is obviously a rewind; it is
 * wrong for fifteen lines of prose, where it means reading the end backwards
 * to get to the beginning.
 *
 * The hold happens once a lap, at the top, which is where a reader wants it. */
int ui_scrollthrough(int cycle, unsigned phase);

/* The panel border, in pixels. In the header because layout outside ui.c has
 * to know it: centering anything inside a panel means centering against the
 * INNER edge, since the border is a visible frame and the eye reads the space
 * within it. Centering against the outer edge is arithmetically right and looks
 * high by exactly this many pixels. */
#define UI_PANEL_BORDER 12

/* An additive radial glow, tinted, centered on rect and spilling past it.
 * This is what tells you which card has focus without drawing a frame
 * around anything. */
void ui_glow(SDL_Renderer *r, const SDL_Rect *rect, unsigned rgb, int alpha,
             float spread);

/* The color of item `index` on a rail, so its marker can be a window onto a
 * strip of every item's color rather than a bar that changes all at once. */
typedef unsigned (*ui_rail_hue)(void *ctx, int index);

/* The position rail across the bottom: a faint full-width track with a bright
 * accent segment showing where the shelf sits in a list of `count`.
 *
 * `index` is a FLOAT and wants cf.pos, the shelf's own continuous position -
 * not the cursor, which is already at the destination on the frame of the
 * press. Whole values give the resting positions exactly. See rail_at.
 *
 * `hue` is optional and wins where it is given: the marker is then colored by
 * what it is OVER rather than by `rgb`, so a move carries one system's color
 * into the next instead of swapping it. Pass NULL, and `rgb` alone, for a list
 * whose items all share a color - every games rail, since a game is drawn in
 * its system's accent. */
void ui_rail(SDL_Renderer *r, int screen_w, int screen_h, float index, int count,
             unsigned rgb, ui_rail_hue hue, void *ctx);
/* The same, down the left edge, for a row that runs vertically. A horizontal
 * bar under a vertical stack says the wrong thing: the eye reads it as the
 * axis the cards move along. */
void ui_rail_v(SDL_Renderer *r, int screen_w, int screen_h, float index, int count,
               unsigned rgb, ui_rail_hue hue, void *ctx);

/* A filled rounded rectangle. SDL has no such primitive; this is the middle as
 * one rect and the two caps as one inset row each, so a panel costs a few
 * dozen fills rather than one per scanline. */
void ui_round_rect(SDL_Renderer *r, const SDL_Rect *q, int radius, SDL_Color col);

/* A menu slab: near-opaque, so a list drawn on it reads against a paused game
 * frame or a lit shelf without either showing through, with a hairline of the
 * system accent around it. `border` is 0xRRGGBB. */
void ui_panel(SDL_Renderer *r, const SDL_Rect *q, int radius, unsigned border);

/* A card for a game with no art: a tinted slab with the title on it. Owned by
 * the caller. */
SDL_Texture *ui_make_card(SDL_Renderer *r, const char *title, unsigned rgb,
                          int *w, int *h);
/* The same, square and square-cornered, for an album with no cover. */
SDL_Texture *ui_make_cover(SDL_Renderer *r, const char *title, unsigned rgb,
                           int *w, int *h);

/* The play modes' marks, in the shapes everybody already reads: two arrows
 * chasing round a loop for repeat, the same with a 1 in it for repeat one, and
 * two crossing arrows for shuffle - and a padlock, for the Mute Switch's hold
 * (TortOS-7cv). Drawn here from a few lines of geometry
 * rather than taken from a font or an icon set. Centered on (cx, cy), `size`
 * pixels square, in `col`; each shape and size is rendered once and kept. */
typedef enum {
	UI_GLYPH_REPEAT, UI_GLYPH_REPEAT_ONE, UI_GLYPH_SHUFFLE, UI_GLYPH_LOCK,
	UI_GLYPH_COUNT
} ui_glyph;
void ui_glyph_draw(SDL_Renderer *r, ui_glyph g, int cx, int cy, int size,
                   SDL_Color col);

/* rgb interpolation, for easing the background tint between systems */
unsigned ui_mix(unsigned a, unsigned b, float t);

#endif
