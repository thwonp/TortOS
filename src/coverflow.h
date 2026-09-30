/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef TORTOS_COVERFLOW_H
#define TORTOS_COVERFLOW_H

#include <SDL.h>
#include <stdbool.h>

/* Single-row Cover Flow: perspective-tilted cards with reflections,
 * rendered with SDL_RenderGeometry. Never draws more cards than there
 * are items: the row wraps around only when count >= CF_WINDOW; below
 * that it clamps, and a 1-item list is a single centered card. */

#define CF_HALF_WINDOW 3
#define CF_WINDOW (2 * CF_HALF_WINDOW + 1)

/* How many cards either side of the cursor the CALLER guarantees are already
 * decoded, and so how far a move may usefully be drawn.
 *
 * This once said such a move CANNOT be drawn, because decoding happened on the
 * render path and seven cold cards in one frame is why a long jump showed two
 * still pictures. src/texload.c moved decoding onto a worker, so the frames
 * come out now whatever the cards are - the reason changed, the number did not.
 * A card past this window has no texture yet, and flying past empty slots says
 * less than cutting does.
 *
 * The other limit is the eye's and never moved: ANIM_MS is 14.4 frames at
 * 60fps, so past about fourteen cards a move crosses more than one card per
 * frame, consecutive frames share no cards at all, and there is no motion left
 * to read. Eight is inside that and is what the texture policy actually keeps.
 * main.c's TEX_KEEP_NEAR is the guarantee and asserts it is not less. */
#define CF_WARM_CARDS 8

/* Where "wider than tall" starts, for cf_layout.wide_area.
 *
 * Measured against the eleven console photographs on 2026-09-16, which sort
 * into two groups with a wide gap between them: Game Boy Color at 0.72, Game
 * Boy at 0.82 and Favorites at 1.03, then nothing until NGPC at 1.39, running
 * up to Master System at 2.05. Anywhere in that gap is the same rule; 1.2 is
 * the middle of it. */
#define CF_WIDE_ART 1.2f

/* How much of a move the label's crossfade uses at each end, leaving the
 * middle empty. See cf_label: the card crosses where the text is, so the text
 * has to be gone before it gets there. */
#define CF_LABEL_EDGE 0.30f

typedef struct {
	float size;       /* card height as a fraction of screen height */
	float aspect;     /* card width / card height */
	float step;       /* neighbor spacing as a fraction of card width */
	float side_scale; /* scale of fully off-center cards */
	float center_y;   /* card center y as a fraction of screen height */
	float tilt;       /* max yaw in radians */
	float reflect;    /* reflection height as a fraction of card height */
	/* Clear air between the art and its reflection, as a fraction of the
	 * card's half height - about 25px on a focused card, and it shrinks with
	 * the side cards because it is in the card's own units rather than the
	 * screen's. A reflection that touches reads as the object continuing;
	 * a small gap reads as a surface it is standing on. */
	float reflect_gap;
	/* Lay the row down the screen instead of across it. The cards, the
	 * scaling and the reflections are unchanged - only which axis the
	 * neighbors are offset along, and `step` then counts card HEIGHTS rather
	 * than widths, because that is the direction they are spaced in. */
	bool vertical;
	/* Size the art to a constant AREA from its own aspect, instead of
	 * containing it inside the frame. See draw_card. Games only: the console
	 * photos all share one squared canvas, so there is nothing for it to fix
	 * there and turning it on would resize a shelf that is already tuned. */
	bool equal_area;
	/* The same rule for art WIDER THAN IT IS TALL - the consoles photographed
	 * lying down - as a FRACTION of the frame's area rather than a switch.
	 * 0 leaves them contained; 1.0 gives them the whole frame's area.
	 *
	 * A dial because the two shelves want different answers. The vertical one
	 * shows a single card with its neighbors pushed off screen, so it can
	 * take the whole area and does. The horizontal row has a neighbor either
	 * side to overlap, and full area buries them. See CF_WIDE_ART. */
	float wide_area;
	int side_alpha;   /* alpha of fully off-center cards (center is 255) */
	int strips;       /* vertical subdivisions per card */
} cf_layout;

extern const cf_layout CF_LAYOUT_SYSTEMS;
/* The same two rows stood on end, for the Vertical direction. Separate tables
 * rather than a flag applied to the others: the screen is 1024x768, so a card
 * sized to fill the width leaves no room above and below, and every number has
 * to be chosen again rather than reused. */
extern const cf_layout CF_LAYOUT_SYSTEMS_V;
extern const cf_layout CF_LAYOUT_GAMES_V;
extern const cf_layout CF_LAYOUT_GAMES;
/* The systems row again, one at a time and flat. Systems only: box art keeps
 * the angled row. */
extern const cf_layout CF_LAYOUT_SINGLE;
/* Muse's shelf, row and column: square frames for album covers.
 * See coverflow.c. */
extern const cf_layout CF_LAYOUT_ALBUMS;
extern const cf_layout CF_LAYOUT_ALBUMS_V;

/* Where the focused card sits on screen, so the caller can put a glow behind
 * it and lay text out against it without duplicating the geometry. */
void cf_focus_rect(const cf_layout *lay, int screen_w, int screen_h, SDL_Rect *out);

/* A move is a fixed-duration tween from `from` to `target` starting at `t0`,
 * rather than an ease toward a moving target. Distance changes the speed, not
 * the time: crossing the whole shelf takes exactly as long as stepping one
 * card, so the row never feels further away than it is. */
/* How a move is spread over its time. OUT_CUBIC peaks on the first frame and
 * decelerates, which is what makes a card step feel immediate. SMOOTH starts
 * and ends at rest and peaks at half again the average - slower to leave, but
 * a solid object that reaches full speed between two frames does not read as
 * turning, it reads as having cut. */
typedef enum {
	CF_EASE_OUT_CUBIC = 0,
	CF_EASE_SMOOTH
} cf_ease;

typedef struct {
	float pos;      /* continuous position, interpolated from -> target */
	float from;     /* where the move in flight started */
	float target;
	Uint32 t0;      /* when it started */
	bool active;    /* animation in flight */
	int last_cursor;
	bool primed;
	/* Which way the row last traveled, -1 or +1. Only a two-item ring needs
	 * it: there, both representatives of the other card sit exactly one step
	 * away, so which side it rests on is a genuine tie and the direction of
	 * travel is the only thing that can settle it sensibly. */
	int last_dir;
	/* Per-shelf timing. Zero means the default, which is what a card row
	 * wants; Vertical sets its own because it slides a whole screen rather
	 * than a card a few hundred pixels, and the same duration spent on the
	 * two is not the same thing to look at. */
	float anim_ms;
	cf_ease ease;
	/* Whether to skip ahead rather than queue when told to move again while
	 * already moving. See cf_set_cursor_dir. */
	bool chase;
	/* How far a move too long to draw may travel before it cuts, in cards.
	 * Per-shelf because a card means different things on each: the row shows
	 * seven at once, so eight of them read as travel, while Vertical shows
	 * ONE filling the screen and eight would be a full-screen blur. Capped by
	 * CF_WARM_CARDS however it is set - a glide is only free while the cards
	 * it crosses are already decoded. Zero disables it. */
	float glide;
	/* A move longer than the warm window: the tween is drawn only as far as
	 * `cut_to` and then jumps to `target`.
	 *
	 * `cut_to` is a DRAWING endpoint and nothing else. An earlier version cut
	 * by overwriting `target` itself, which broke the moment a second move
	 * arrived mid-tween: cf_set_cursor_dir accumulates with `target += raw`,
	 * so the next press added its step to the glide endpoint rather than to
	 * the real destination, and holding a jump walked `pos` tens of cards away
	 * from `cursor`. The shelf then drew one part of the list under another
	 * part's title, and stayed there, because an unchanged cursor returns
	 * early and nothing ever put it back. */
	float cut_to;
	bool  cutting;
} coverflow;

/* The index a pending cut will land on, or -1 when none is pending. The caller
 * uses it to decode that window while the departure is still being drawn, so
 * the move lands on a card that is already there. */
int cf_landing(const coverflow *cf, int count);

/* Which item a label under the shelf should be describing right now, and how
 * visible it should be: 1.0 at rest, falling to 0 at the midpoint of a move.
 *
 * A LABEL DRAWN FROM THE CURSOR ARRIVES BEFORE THE SHELF DOES. The cursor
 * changes the instant a button is pressed and the cards take ANIM_MS to catch
 * up, so the name of the system you are moving TO sits under the card you are
 * still looking at. The horizontal row hides it - seven cards move at once and
 * the eye is on them - but Vertical shows one card filling the screen, and
 * there the text is the only thing that jumps.
 *
 * Stateless, which is the point: the item is whatever `pos` is nearest, so it
 * changes at the halfway mark, and the alpha is zero exactly there. Nothing
 * has to remember the name it was showing, and a move interrupted halfway
 * needs no unwinding. */
float cf_label(const coverflow *cf, int count, int *index);

/* Catch a move `u` of the way through, from `from` to `to`. For the shot
 * harness: a still frame cannot show anything that only exists DURING a move,
 * and there are now several of those - the label crossfade, a card mid-slide,
 * the rail between two positions.
 *
 * A whole move is staged rather than just a position, because the parts read
 * different things: the rail reads `pos` and the crossfade reads the clock, so
 * nudging one alone draws a shelf part way along with nothing moving.
 *
 * The curve and the duration come from the shelf's own settings rather than
 * being named again by the caller, which is the whole reason this is here and
 * not in the harness: a harness that positions cards by a different curve than
 * the launcher draws a screen nobody will ever see. It replaced a copy of the
 * smoothstep in main.c that had Vertical's duration hardcoded beside it. */
void cf_stage(coverflow *cf, float from, float to, float u);

/* Whether a cut is in flight. The caller holds off evicting while it is, or it
 * would free the very cards the departure is still drawing: the cursor is
 * already at the destination and eviction is measured from the cursor. */
bool cf_cutting(const coverflow *cf);

/* Texture for item index; w/h receive its pixel size. May return NULL.
 *
 * `content_bottom` receives how far down the texture its opaque pixels reach,
 * as a fraction of height: 1.0 for art that fills its canvas, less for art
 * padded with transparency below the subject. The reflection is drawn from
 * there rather than from the card's edge - see draw_card. */
typedef SDL_Texture *(*cf_tex_fn)(void *ctx, int index, int *w, int *h,
                                  float *content_bottom);

void cf_reset(coverflow *cf, int cursor);
/* Move toward cursor (shortest path when wrapping). */
void cf_set_cursor(coverflow *cf, int cursor, int count);
/* Same, with the direction the user actually pressed: -1, 0 or +1. A ring of
 * two has both neighbors one step away, so it is the only size where the
 * shortest-path arithmetic cannot work out which way to turn. */
void cf_set_cursor_dir(coverflow *cf, int cursor, int count, int dir);
/* Step animation and draw. Returns true while still animating. */
bool cf_draw(coverflow *cf, SDL_Renderer *r, int screen_w, int screen_h,
             int count, cf_tex_fn get_tex, void *ctx, const cf_layout *lay);

#endif
