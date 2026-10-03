/* SPDX-License-Identifier: MIT */
#include "coverflow.h"

#include <math.h>
#include <string.h>

/* The systems row: three cards, so they are large and the neighbors sit
 * close. center_y is high because the reflection owns the bottom of the
 * screen and the shelf rail sits under it.
 *
 * reflect is the reflection baseline as a multiple of the card half-height,
 * measured below the card center (see draw_card). */
/* Flat, with the neighbors half off the edge - horizontal only, because the
 * the vertical shelves have their own layouts.
 *
 * NO YAW, because the art already has an angle. The console photographs are
 * shot in 3/4 view, so a coverflow yaw laid on top of them is a second
 * perspective disagreeing with the one in the image. A flat card lets the
 * photograph's own angle be the only one.
 *
 * A BIG CENTER AND SMALL NEIGHBORS, on screen rather than off the edge. The
 * neighbors are a hint of what is next, not competition for the middle.
 *
 * `step` is in CARD WIDTHS, which makes it the counterintuitive one: a wider
 * card needs FEWER of them to reach the same distance, so growing the center
 * shrinks this number even though the gap in pixels is unchanged. On a
 * 1024-wide screen a 0.81 card is 622 tall and 485 wide, and 0.82 puts the
 * neighbor centers 400px out - far enough that a 0.38 neighbor (184 wide) sits
 * fully on screen with about 20px to spare.
 *
 * THE CARD RECT MAY OVERLAP THE TEXT BELOW IT, and that is fine. The frame is
 * 0.78 aspect against art that is square, so the bottom of the rect is empty
 * space rather than console. Sizing the card to keep its RECT clear of the
 * system name at y=618 costs real size to protect nothing - measured the hard
 * way, by shrinking it to 0.78 and being told the 0.81 looked right.
 *
 * The text positions are fixed and the card is free: the title renders 52px
 * tall and the count sits at 690, so 618 is the only place the name fits with
 * its usual 20px of air. Neither moves. */
const cf_layout CF_LAYOUT_SYSTEMS = {
	.size = 0.81f, .aspect = 0.78f, .step = 0.82f, .side_scale = 0.38f,
	.center_y = 0.415f, .tilt = 0.0f, .reflect = 1.34f,
	.side_alpha = 140, .strips = 16, .wide_area = 0.80f,
};

/* One system filling the screen, flat, sliding in from off the edge.
 *
 * Every difference from the row above is a number here: no yaw, no shrinking
 * or fading of the neighbors, and a step wide enough to put them past the
 * bezel. Nothing in cf_draw knows this mode exists.
 *
 * side_scale and side_alpha are 1.0 and 255 because they are NOT unused just
 * because the neighbors are off screen: during a move the incoming card is
 * partly on screen at a fractional distance, and any other values would have
 * it slide in shrunken and translucent and grow into place.
 *
 * aspect is square because this mode is paired with the photographs. Art of
 * another shape still fits - draw_card contains it - it just leaves the frame
 * unfilled on two sides.
 *
 * step 1.8 puts a neighbor's near edge about 130px past the screen at this
 * size, so nothing peeks in at rest, and the extra distance is what makes the
 * slide read as coming from outside rather than from just off the edge. */
const cf_layout CF_LAYOUT_SINGLE = {
	.size = 0.70f, .aspect = 1.00f, .step = 1.80f, .side_scale = 1.00f,
	.center_y = 0.40f, .tilt = 0.0f, .reflect = 1.15f,
	.side_alpha = 255, .strips = 16,
};

/* The games row: box art, so the cards are taller and there are more of them
 * in view. The title is drawn above the row, which is why it sits slightly
 * lower than the systems row. */
/* Vertical: ONE CARD AT A TIME, sliding in from off the edge.
 *
 * 768 of height against 1024 of width means a stack cannot show three cards
 * the way the horizontal row does, so it stops trying and gives the whole
 * screen to one. The neighbors are not shrunk or faded, they are simply put
 * past the edge by a step wider than the screen - the same thing
 * CF_LAYOUT_SINGLE does, on the other axis.
 *
 * The card frame is taller than the art is: 0.88 is 668px tall and 0.78 aspect
 * makes it 521 wide, and a square console photograph contained in that ends at
 * y=609, which is what keeps it clear of the system name at 618. The frame
 * overlapping that line costs nothing because the bottom of the frame is
 * empty. */
const cf_layout CF_LAYOUT_SYSTEMS_V = {
	.size = 0.88f, .aspect = 0.78f, .step = 1.30f, .side_scale = 1.00f,
	.center_y = 0.45f, .tilt = 0.0f, .reflect = 1.34f,
	.vertical = true, .side_alpha = 255, .strips = 16, .wide_area = 1.00f,
};

/* Both games layouts' share of the screen beyond the stage (cf_layout.grow). */
#define GAMES_GROW 0.60f

/* Box art at the SAME SIZE the horizontal row uses, unlike the systems row
 * above. A console is one of eleven and can afford to dominate the screen; box
 * art is one of hundreds, and blown up to match it just looks oversized. The
 * step has to grow to compensate - it counts card heights, so a smaller card
 * needs more of them to put the neighbors past a 768 screen. */
const cf_layout CF_LAYOUT_GAMES_V = {
	.size = 0.60f, .aspect = 0.72f, .step = 1.50f, .side_scale = 1.00f,
	.center_y = 0.47f, .tilt = 0.0f, .reflect = 1.52f,
	.vertical = true, .side_alpha = 255, .strips = 16,
	.equal_area = true, .grow = GAMES_GROW,
};

const cf_layout CF_LAYOUT_GAMES = {
	.size = 0.60f, .aspect = 0.72f, .step = 0.74f, .side_scale = 0.62f,
	.center_y = 0.47f, .tilt = 0.82f, .reflect = 1.52f,
	.side_alpha = 150, .strips = 16,
	.equal_area = true, .grow = GAMES_GROW,
};

/* Muse's shelf: album covers, which are square, where the games layouts are
 * frames for tall boxes. A square contained in a 0.72 frame gets the frame's
 * WIDTH, 391px of a 461px-tall frame - covers came out smaller than the box
 * art around them, and Eric asked for them larger. A square frame the same
 * height gives them all of it, 461px, and the center moves down so the top
 * clears the artist line under the album's name. Otherwise the games rows'
 * own numbers, since a shelf of albums should move like a shelf of games. */
const cf_layout CF_LAYOUT_ALBUMS = {
	.size = 0.60f, .aspect = 1.00f, .step = 0.74f, .side_scale = 0.62f,
	.center_y = 0.54f, .tilt = 0.82f, .reflect = 1.52f,
	.side_alpha = 150, .strips = 16,
};

const cf_layout CF_LAYOUT_ALBUMS_V = {
	.size = 0.60f, .aspect = 1.00f, .step = 1.50f, .side_scale = 1.00f,
	.center_y = 0.54f, .tilt = 0.0f, .reflect = 1.52f,
	.vertical = true, .side_alpha = 255, .strips = 16,
};

/* The card's height on this screen: the stage's, plus its share of the rest. */
static float card_h(const cf_layout *lay, int screen_h)
{
	int spare = screen_h > CF_STAGE_H ? screen_h - CF_STAGE_H : 0;

	return CF_STAGE_H * lay->size + spare * lay->grow;
}

void cf_focus_rect(const cf_layout *lay, int screen_w, int screen_h, SDL_Rect *out)
{
	int top = (screen_h - CF_STAGE_H) / 2;
	float ch = card_h(lay, screen_h);
	float cw = ch * lay->aspect;
	out->w = (int)cw;
	out->h = (int)ch;
	out->x = (int)(screen_w * 0.5f - cw * 0.5f);
	out->y = (int)(top + CF_STAGE_H * lay->center_y - ch * 0.5f);
}

/* Every move takes this long, whatever its distance. Crossing the shelf is not
 * a longer journey than stepping one card, it is a faster one.
 *
 * This replaced an exponential ease toward the target with a speed cap over
 * it, and a teleport past six cards. The ease alone converged in time
 * proportional to log(distance), which was nearly constant already; the cap
 * made anything past a card and a half take time proportional to DISTANCE, so
 * a letter jump across a big shelf became a wait, and the teleport existed to
 * hide the worst of it. One duration removes the need for both. */
#define ANIM_MS 240.0f

/* Ease in, cubic: starts at rest and is at its fastest when it ends.
 *
 * That is backwards for a step and exactly right for a cut. The end of this
 * move is where the discontinuity is, and a cut made while the eye is tracking
 * fast motion is close to invisible - it is the ordinary cut on action -
 * whereas one made at rest is the most visible cut available, because the eye
 * has settled and has nothing left to do but notice it. */
static float ease_in(float u) { return u * u * u; }

/* Ease out, cubic: about seven eighths of the distance is covered in the first
 * half of the time, so a single-card step still reads as immediate even though
 * it formally takes as long as a forty-card one. */
static float ease_out(float u)
{
	float k = 1.0f - u;
	return 1.0f - k * k * k;
}

/* Smoothstep: zero velocity at both ends, peak 1.5x average in the middle. */
static float ease_smooth(float u)
{
	if (u < 0.0f) u = 0.0f;
	if (u > 1.0f) u = 1.0f;
	return u * u * (3.0f - 2.0f * u);
}

static float ease_apply(cf_ease e, float u)
{
	return e == CF_EASE_SMOOTH ? ease_smooth(u) : ease_out(u);
}

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

/* Wrap smoothly like an infinite carousel, but never draw the same item
 * twice. A 1-item list is a single static card.
 *
 * The half-window used to be (count-1)/2, which reads as "keep the span
 * 2*half+1 within the item count" and is right for odd counts and wrong for
 * every even one. Two items gave half = 0, the draw loop ran from 0 to 0, and
 * a shelf holding two games drew exactly one card while the counter under it
 * said 1/2. Reported 2026-08-29 and reproduced on the host immediately.
 *
 * The span cannot be both odd and equal to an even count, so the duplicate is
 * dropped where it actually happens - in the slot collection - rather than
 * prevented by shrinking the window until it cannot occur. */
static bool cf_loops(int count) { return count >= 2; }

static int cf_half(int count)
{
	int h = count - 1;
	return h < CF_HALF_WINDOW ? h : CF_HALF_WINDOW;
}

void cf_reset(coverflow *cf, int cursor)
{
	memset(cf, 0, sizeof *cf);
	cf->pos = cf->from = cf->target = (float)cursor;
	cf->last_cursor = cursor;
	cf->primed = true;
}

/* The clock every move is measured against. A shot freezes it, because a
 * move staged at one tick and drawn a few milliseconds later is a different
 * picture each run, and the shots are compared byte for byte. */
static bool   clock_frozen;
static Uint32 clock_at;

void cf_clock_freeze(void) { clock_at = SDL_GetTicks(); clock_frozen = true; }

static Uint32 cf_now(void) { return clock_frozen ? clock_at : SDL_GetTicks(); }

void cf_set_cursor_dir(coverflow *cf, int cursor, int count, int dir)
{
	if (!cf->primed) { cf_reset(cf, cursor); return; }
	if (cursor == cf->last_cursor) return;

	bool loops = cf_loops(count);
	int raw = cursor - cf->last_cursor;
	if (loops) {
		/* take the shortest way around the ring so an end<->beginning move
		 * is a single smooth step, not a slide across the whole list */
		while (raw > count / 2) raw -= count;
		while (raw < -count / 2) raw += count;
		/* A ring of TWO has both neighbors one step away, so "shortest way
		 * around" has no answer and the rule above returns the literal
		 * difference every time: +1, -1, +1, -1. The row then rocks right and
		 * left rather than turning, however the cards are drawn - which is
		 * why this looked like a drawing bug and is not one. When the caller
		 * knows which way the press was, believe it over the arithmetic. */
		if (count == 2 && dir) raw = dir;
	}
	if (raw) cf->last_dir = raw > 0 ? 1 : -1;
	cf->last_cursor = cursor;

	/* A press arriving while a cut is still being drawn: the departure on
	 * screen belongs to a jump that is already over. Land it before taking
	 * the new one, or the shelf creeps while the cursor races - ease_in is
	 * cubic, so at the 90ms key repeat a 240ms departure has covered about 5%
	 * of its travel, and holding the button would leave the cards nearly
	 * still under a title running through the list. */
	if (cf->cutting) {
		cf->pos = cf->target;
		cf->cutting = false;
	}
	cf->target += (float)raw;
	/* Never more than one step behind the destination.
	 *
	 * Every press adds to `target` whether it came from a key repeat or from
	 * a fast thumb, so a shelf whose animation is slower than the input can
	 * be given work it will still be playing out long after the button is
	 * released - measured at 13 items behind on a slow shelf. Rate-limiting the
	 * repeat does not help, because nothing stops a person pressing faster
	 * than the limit.
	 *
	 * So the animation does not queue: it goes where it has been told and
	 * animates the last step of getting there. The cursor stays exact, the
	 * turn shown is always the one that matters, and `pos` stays beside
	 * `cursor` - which also keeps the art that is being drawn inside the
	 * window warmed around the cursor rather than decoding on the render
	 * path. Only shelves that ask for it: a card row is fast enough to keep
	 * up, and skipping there would lose the glide. */
	if (cf->chase) {
		if (cf->target - cf->pos > 1.0f)  cf->pos = cf->target - 1.0f;
		if (cf->target - cf->pos < -1.0f) cf->pos = cf->target + 1.0f;
	}
	/* Start the tween from where the cards ARE, not from where the last one
	 * was headed. Holding a direction retargets every key repeat, and taking
	 * the live position each time is what makes that one continuous glide
	 * rather than a stutter back to the old start. */
	cf->from = cf->pos;
	/* A MOVE LONGER THAN THE WARM WINDOW IS NOT WORTH DRAWING.
	 *
	 * Not the same claim this made when it was written. Then it said such a
	 * move could not be drawn at all, because decoding sat on the render path
	 * and seven cold cards in a frame meant the frame never arrived - a letter
	 * jump showed the card it started near, then the card it landed on, and
	 * nothing between. texload.c fixed that, and the frames arrive now.
	 *
	 * What is left is two things the worker does not change. The cards out
	 * there have no texture yet, so drawing the move would fly past empty
	 * slots; and a fixed duration turns distance into speed, so past about
	 * fourteen cards it crosses more than one per frame and stops being motion
	 * at all. Both say the same thing: show the part that means something and
	 * cut the rest.
	 *
	 * The part that means something is the departure, because the warm cards
	 * are all behind the cursor - the destination is by definition the half
	 * not decoded yet. So the motion accelerates rather than settles and the
	 * cut lands at full speed, where the eye is least able to see it, while
	 * the caller warms the destination underneath; see cf_landing. A chase
	 * shelf holds `pos` within one step already and never gets here. */
	cf->cutting = false;
	{
		float span = cf->target - cf->from;
		float g = cf->glide > (float)CF_WARM_CARDS ? (float)CF_WARM_CARDS
		                                           : cf->glide;
		if (!cf->chase && g > 0.0f && (span > (float)CF_WARM_CARDS ||
		                               span < -(float)CF_WARM_CARDS)) {
			cf->cutting = true;
			cf->cut_to = cf->from + (span > 0.0f ? g : -g);
		}
	}
	cf->t0 = cf_now();
	cf->active = true;
}

float cf_label(const coverflow *cf, int count, int *index)
{
	float u, ms;
	float where;
	int i;

	if (index) *index = 0;
	if (count <= 0) return 0.0f;

	/* FROM TIME, NOT FROM POSITION. The first version read the fade off
	 * `pos`, which is the eased position - and Vertical eases with
	 * smoothstep, so the shelf crawls at both ends and races through the
	 * middle. The alpha inherited that shape: it hung near full while nothing
	 * moved, fell off a cliff, then POPPED back to full and crept the rest of
	 * the way. The fade out looked slow and the fade in did not look like a
	 * fade at all.
	 *
	 * `u` is the same linear clock step_anim measures the ease against, so
	 * the crossfade is even whatever curve the cards are riding.
	 *
	 * It also fixes something the position version got wrong for free: a move
	 * across several cards is ONE fade, out and back, rather than a flash per
	 * card crossed. Holding the d-pad strobed the label before. */
	if (!cf->active) {
		where = cf->pos;
		u = -1.0f;
	} else {
		ms = cf->anim_ms > 0.0f ? cf->anim_ms : ANIM_MS;
		u = (float)(cf_now() - cf->t0) / ms;
		if (u < 0.0f) u = 0.0f;
		if (u > 1.0f) u = 1.0f;
		/* The name swaps at the halfway mark, where it is invisible. */
		where = u < 0.5f ? cf->from : cf->target;
	}

	i = (int)floorf(where + 0.5f) % count;
	if (i < 0) i += count;
	if (index) *index = i;

	if (u < 0.0f) return 1.0f;

	/* GONE BEFORE THE CARD ARRIVES, BACK AFTER IT HAS LEFT.
	 *
	 * Spanning the whole move was wrong for the reason Eric spotted: Vertical
	 * slides the console straight through y=618, where the name is. The text
	 * is drawn over the card, but a third of its opacity over a lit console
	 * reads as being behind it, and the fade-in finished while the card was
	 * still crossing - so the name never appeared to arrive, it just resolved
	 * out of a smear.
	 *
	 * So the crossfade lives in the first and last CF_LABEL_EDGE of the move
	 * and the middle is empty. The card crosses an empty strip, the swap
	 * happens where nothing is drawn anyway, and the new name fades up on a
	 * clear background once the card has gone by. */
	if (u <= CF_LABEL_EDGE)        return 1.0f - u / CF_LABEL_EDGE;
	if (u >= 1.0f - CF_LABEL_EDGE) return (u - (1.0f - CF_LABEL_EDGE)) / CF_LABEL_EDGE;
	return 0.0f;
}

void cf_stage(coverflow *cf, float from, float to, float u)
{
	float ms = cf->anim_ms > 0.0f ? cf->anim_ms : ANIM_MS;

	if (u < 0.0f) u = 0.0f;
	if (u > 1.0f) u = 1.0f;
	cf->from = from;
	cf->target = to;
	cf->pos = from + (to - from) * ease_apply(cf->ease, u);
	/* Backdated, so the move is already this far along by the clock the label
	 * and step_anim both measure against. */
	cf->t0 = cf_now() - (Uint32)(u * ms);
	cf->active = true;
	cf->cutting = false;
	cf->primed = true;
	cf->last_cursor = (int)floorf(to + 0.5f);
}

int cf_landing(const coverflow *cf, int count)
{
	int i;
	if (!cf->cutting || count <= 0) return -1;
	i = (int)floorf(cf->target + 0.5f) % count;
	if (i < 0) i += count;
	return i;
}

bool cf_cutting(const coverflow *cf) { return cf->cutting; }

static void step_anim(coverflow *cf, int count)
{
	bool loops = cf_loops(count);
	if (loops) {
		/* `from` rides along with pos and target through a wrap, or the
		 * interpolation below would be measuring against a point that is now
		 * a whole revolution away. */
		while (cf->pos >= (float)count) {
			cf->pos -= count; cf->target -= count; cf->from -= count;
			if (cf->cutting) cf->cut_to -= count;
		}
		while (cf->pos < 0.0f) {
			cf->pos += count; cf->target += count; cf->from += count;
			if (cf->cutting) cf->cut_to += count;
		}
	}
	if (!cf->active) return;
	{
		float ms = cf->anim_ms > 0.0f ? cf->anim_ms : ANIM_MS;
		float u = (float)(cf_now() - cf->t0) / ms;
		if (u >= 1.0f) {
			/* Whether or not this was a cut, the move ends where the cursor
			 * has been since the press. `target` was never moved. */
			cf->cutting = false;
			cf->pos = cf->target;
			cf->active = false;
			return;
		}
		cf->pos = cf->cutting
			? cf->from + (cf->cut_to - cf->from) * ease_in(u)
			: cf->from + (cf->target - cf->from) * ease_apply(cf->ease, u);
	}
}

/* Weak-perspective projection of a card-local point. The card is yawed
 * about its vertical axis; x rotates into depth and everything is scaled
 * by F/(F+z), which squashes the far edge horizontally and vertically. */
typedef struct {
	float ox, oy;   /* card center on screen */
	float cosa, sina;
	float focal;
} cf_proj;

static void proj_point(const cf_proj *p, float lx, float ly, float *sx, float *sy)
{
	float xr = lx * p->cosa;
	float zr = lx * p->sina;
	float s = p->focal / (p->focal + zr);
	*sx = p->ox + xr * s;
	*sy = p->oy + ly * s;
}

static void render_quad(SDL_Renderer *r, SDL_Texture *tex,
                        const float xy[8], const float uv[8],
                        const SDL_Color col[4])
{
	SDL_Vertex v[4];
	for (int i = 0; i < 4; i++) {
		v[i].position.x = xy[i * 2];
		v[i].position.y = xy[i * 2 + 1];
		v[i].tex_coord.x = uv[i * 2];
		v[i].tex_coord.y = uv[i * 2 + 1];
		v[i].color = col[i];
	}
	static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
	SDL_RenderGeometry(r, tex, v, 4, idx, 6);
}

static void draw_card(SDL_Renderer *r, SDL_Texture *tex, int tw, int th,
                      float cb, float ox, float oy, float hw, float hh,
                      float ang, Uint8 alpha, const cf_layout *lay)
{
	/* Fit the texture to the card.
	 *
	 * CONTAIN gives every cover the frame's proportions and nobody else's, so
	 * only art shaped like the frame ever fills it. Measured on this card
	 * 2026-09-12, on one shelf at one setting: an NES cover is 0.70, close to
	 * the frame's 0.72, and draws 323x461. A US SNES cover is 1.41 - those
	 * cases were wider than they were tall - and draws 332x236, barely half
	 * the area. Square Game Boy and TurboGrafx art draws 332x331, about three
	 * quarters. The shelf looked inconsistent because it was.
	 *
	 * EQUAL AREA solves ahw/ahh = tex_ar against ahw*ahh = hw*hh: the card
	 * keeps the frame's area and takes the art's shape. Art already shaped
	 * like the frame barely moves, which is why NES is unchanged and SNES
	 * grows to 464x330.
	 *
	 * It reads each cover rather than each system, which matters more than it
	 * sounds: 14 of 314 SNES covers here are Japanese, and a Super Famicom box
	 * is TALL - 0.53, not 1.41. Widening the SNES frame would have helped 300
	 * covers and cut those 14 to a quarter of their area. This rule never sees
	 * the difference because it never asks what system it is looking at. */
	float ahw = hw, ahh = hh;
	if (tw > 0 && th > 0) {
		float tex_ar = (float)tw / (float)th;
		/* A CONSOLE LYING DOWN GETS THE FRAME'S AREA TOO, for the same
		 * reason a wide box cover does, and it took cropping the art before
		 * anything could see it. Every fancy card was a 384x384 canvas with
		 * the console centered in transparent padding, so the shelf
		 * contain-fitted a SQUARE and never learned the console's shape: a
		 * Master System drew 466x227 inside a 485x622 frame, two thirds the
		 * area of a Game Boy Color, with 395px of frame height unused.
		 * Measured 2026-09-16, and cards.h records the art convention that
		 * follows from it.
		 *
		 * 1.2 IS A GAP, NOT A GUESS. The eleven consoles sort into two groups
		 * with nothing between them: Game Boy Color 0.72, Game Boy 0.82,
		 * Favorites 1.03, then NGPC 1.39 and up to Master System at 2.05. The
		 * three below stay exactly as they were - they already fill their
		 * frame, and growing them walks into the system name at y=618. */
		if (lay->equal_area || (lay->wide_area > 0.0f && tex_ar > CF_WIDE_ART)) {
			float area = hw * hh * (lay->equal_area ? 1.0f : lay->wide_area);
			ahw = sqrtf(area * tex_ar);
			ahh = sqrtf(area / tex_ar);
		} else {
			float frame_ar = hw / hh;
			if (tex_ar > frame_ar) ahh = hw / tex_ar;
			else ahw = hh * tex_ar;
		}
	}

	cf_proj p = {
		.ox = ox, .oy = oy,
		.cosa = cosf(ang), .sina = sinf(ang),
		.focal = hw * 6.0f + 1.0f,
	};

	SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

	int n = lay->strips > 0 ? lay->strips : 16;
	/* The reflection reaches a common baseline (lay->reflect * hh below the
	 * card center) no matter how tall the fitted art is: short art gets a
	 * longer reflection so it reflects to the same depth as tall art. The
	 * art itself stays vertically centered. */
	/* Where the art's opaque pixels stop, in local units. Reflecting from the
	 * card's bottom edge instead put the mirror below transparent padding: the
	 * console art is squared and centered, so a Genesis carries 102 of its 384
	 * rows empty underneath and its reflection began a further 102 away again
	 * - a gap of about 196px on screen, against 10px for a Game Boy, which
	 * fills its canvas. Same reflection setting, wildly different result.
	 * Measured 2026-09-08. */
	float y_cb = -ahh + cb * 2.0f * ahh + lay->reflect_gap * hh;
	/* How far past the mirror the reflection runs, as a fraction of the art's
	 * full height, so that it still reaches the common baseline the layout
	 * asks for however high the mirror sits. With cb = 1 this is exactly what
	 * it was before: art that fills its canvas is unchanged. */
	float f = lay->reflect;
	if (ahh > 0.001f) {
		f = (lay->reflect * hh - y_cb) / (2.0f * ahh);
		if (f < 0.02f) f = 0.02f;
		/* Cap at what is actually above the mirror: sampling past the top of
		 * the texture gives a negative texcoord, which the Mali GLES driver
		 * drops entirely - no reflection at all rather than a short one. */
		if (f > cb) f = cb;
	}
	Uint8 ra = (Uint8)(alpha * 90 / 255);
	SDL_Color body[4] = {
		{ 255, 255, 255, alpha }, { 255, 255, 255, alpha },
		{ 255, 255, 255, alpha }, { 255, 255, 255, alpha },
	};

	for (int i = 0; i < n; i++) {
		float u0 = (float)i / n, u1 = (float)(i + 1) / n;
		float lx0 = -ahw + u0 * 2.0f * ahw;
		float lx1 = -ahw + u1 * 2.0f * ahw;

		float tlx, tly, trx, try_, brx, bry, blx, bly;
		proj_point(&p, lx0, -ahh, &tlx, &tly);
		proj_point(&p, lx1, -ahh, &trx, &try_);
		proj_point(&p, lx1, +ahh, &brx, &bry);
		proj_point(&p, lx0, +ahh, &blx, &bly);

		float xy[8] = { tlx, tly, trx, try_, brx, bry, blx, bly };
		float uv[8] = { u0, 0, u1, 0, u1, 1, u0, 1 };
		render_quad(r, tex, xy, uv, body);

		if (f > 0.0f) {
			/* Mirror from where the CONTENT ends, extended along the card's
			 * own (already foreshortened) vertical direction. The mirror
			 * corners are found by walking cb of the way down the strip,
			 * which is exact: only x rotates into depth, so the depth scale
			 * is constant across a strip's height and the interpolation is
			 * linear. */
			/* Down cb of the strip to where the art stops, then the gap. The
			 * texture still starts at cb: the reflection is displaced, not
			 * cropped, so the first thing it shows is still the last thing
			 * the art showed. */
			float m = cb + lay->reflect_gap * hh / (2.0f * ahh);
			float mlx = tlx + m * (blx - tlx), mly = tly + m * (bly - tly);
			float mrx = trx + m * (brx - trx), mry = try_ + m * (bry - try_);
			float rblx = mlx + f * (blx - tlx);
			float rbly = mly + f * (bly - tly);
			float rbrx = mrx + f * (brx - trx);
			float rbry = mry + f * (bry - try_);
			float vb = cb - f;
			float rxy[8] = { mlx, mly, mrx, mry, rbrx, rbry, rblx, rbly };
			float ruv[8] = { u0, cb, u1, cb, u1, vb, u0, vb };
			SDL_Color rcol[4] = {
				{ 255, 255, 255, ra }, { 255, 255, 255, ra },
				{ 255, 255, 255, 0 }, { 255, 255, 255, 0 },
			};
			render_quad(r, tex, rxy, ruv, rcol);
		}
	}
}

bool cf_draw(coverflow *cf, SDL_Renderer *r, int screen_w, int screen_h,
             int count, cf_tex_fn get_tex, void *ctx, const cf_layout *lay)
{
	if (count <= 0) return false;
	step_anim(cf, count);

	bool loops = cf_loops(count);
	int half = cf_half(count);
	int top = (screen_h - CF_STAGE_H) / 2;
	float ch = card_h(lay, screen_h);
	float cw = ch * lay->aspect;
	/* Spacing counts the card's extent along the axis it is stacked on. */
	float step = (lay->vertical ? ch : cw) * lay->step;
	float cx = screen_w * 0.5f;
	float cy = top + CF_STAGE_H * lay->center_y;

	int base = (int)floorf(cf->pos + 0.5f);

	/* collect visible slots, then draw far-to-near so the center card wins */
	struct slot { int item; float d; } slots[CF_WINDOW];
	int ns = 0;
	for (int k = -half; k <= half; k++) {
		int i = base + k;
		float d = (float)i - cf->pos;
		if (fabsf(d) > half + 0.5f) continue;
		int item = i;
		if (loops) {
			item = i % count;
			if (item < 0) item += count;
		} else if (i < 0 || i >= count) {
			continue;
		}
		/* One slot per item, nearest wins. With the window now allowed to be
		 * wider than the list, wrapping offers the same item on both sides -
		 * two items put the other one at -1 and +1 - and drawing it twice
		 * would be worse than the bug this replaced. */
		{
			int dup = -1, q;
			for (q = 0; q < ns; q++)
				if (slots[q].item == item) { dup = q; break; }
			if (dup >= 0) {
				float held = slots[dup].d;
				/* Nearest wins. On an exact tie - which is every resting frame
				 * of a two-item ring, where both copies sit one step out - put
				 * the card BEHIND the direction of travel, so the one you just
				 * moved past is the one you see. Choosing arbitrarily instead
				 * meant it sat left whichever way you went, and so had to jump
				 * across at the end of every leftward move. */
				if (fabsf(d) < fabsf(held) ||
				    (fabsf(d) == fabsf(held) && cf->last_dir &&
				     (d < 0) == (cf->last_dir > 0)))
					slots[dup].d = d;
				continue;
			}
		}
		slots[ns].item = item;
		slots[ns].d = d;
		ns++;
	}
	/* insertion sort by |d| descending */
	for (int a = 1; a < ns; a++) {
		struct slot s = slots[a];
		int b = a - 1;
		while (b >= 0 && fabsf(slots[b].d) < fabsf(s.d)) {
			slots[b + 1] = slots[b];
			b--;
		}
		slots[b + 1] = s;
	}

	for (int a = 0; a < ns; a++) {
		float d = slots[a].d;
		float ad = fabsf(d);
		float c = 1.0f - clampf(ad, 0.0f, 1.0f);
		float scale = lay->side_scale + (1.0f - lay->side_scale) * c;
		float ang = -lay->tilt * clampf(d, -1.0f, 1.0f);
		Uint8 alpha = (Uint8)(lay->side_alpha + (255 - lay->side_alpha) * c);
		int tw = 0, th = 0;
		float cb = 1.0f;
		SDL_Texture *tex = get_tex(ctx, slots[a].item, &tw, &th, &cb);
		if (!tex) continue;
		draw_card(r, tex, tw, th, cb,
		          lay->vertical ? cx : cx + d * step,
		          lay->vertical ? cy + d * step : cy,
		          cw * 0.5f * scale, ch * 0.5f * scale, ang, alpha, lay);
	}
	return cf->active;
}

/* Direction unknown - the cursor moved by something other than a press, such
 * as a letter jump or a list rebuild. Only a two-item ring cares. */
void cf_set_cursor(coverflow *cf, int cursor, int count)
{
	cf_set_cursor_dir(cf, cursor, count, 0);
}
