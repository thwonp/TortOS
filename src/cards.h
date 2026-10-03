/* SPDX-License-Identifier: MIT */
#ifndef TORTOS_CARDS_H
#define TORTOS_CARDS_H

#include <stdbool.h>
#include <string.h>

/* Which art the shelf wears.
 *
 * A set is a directory under res/cards/ holding files named by column 6 of
 * systems.cfg, so adding one is dropping in a folder - no config change and no
 * code change. The art does NOT have to match the default set's shape: cf_draw
 * contain-fits every texture and lengthens the reflection for short art, which
 * is why a set of square photographs sits on the same shelf as portrait cards
 * without touching the layout.
 *
 * ONE CONVENTION, AND IT IS LOAD-BEARING: art wider than it is tall is CROPPED
 * to its own edges, and art taller than wide keeps whatever canvas it has.
 *
 * The shelf reads a texture's shape to decide how big to draw it
 * (cf_layout.wide_area, CF_WIDE_ART), and padding lies about shape. Every
 * fancy card used to be a 384x384 canvas with the console centered in
 * transparency, so a Master System - 369x180 of actual console - was drawn as
 * a SQUARE: 466x227 inside a 485x622 frame, two thirds the area of a Game Boy
 * Color, with 395px of frame height empty. Cropping the nine wide ones on
 * 2026-09-16 is what let the shelf see them.
 *
 * The tall ones were deliberately NOT cropped, and that is the part worth
 * knowing before you tidy it. Game Boy Color's ink is 267x369 in that square,
 * and cropping it would let contain-fit grow it from 466px tall to the frame's
 * full 622 - straight through the system name at y=618. They already fill
 * their frame; the padding is what holds them where they belong.
 *
 * Accents stay in systems.cfg and do not vary by set. They were sampled from
 * the classic art, and the obvious idea of resampling them per set does not
 * survive contact with the photographs: measured over their opaque pixels the
 * hardware runs 0-13% saturation, Genesis at 0, because consoles are gray and
 * black plastic. There is no color in them to sample. */
#define CARDS_DEFAULT "classic"

typedef struct {
	/* What the setting stores. Separate from `dir` because a preset is art
	 * AND how it is presented, and two presets can draw the same directory a
	 * different way. Keying the setting on the directory would make those two
	 * indistinguishable once written down. */
	const char *id;
	const char *dir;   /* directory under res/cards/ */
	const char *name;  /* what the menu row calls it */
	/* Whether the art says which system it is. The classic cards have the
	 * name drawn into the image, so the shelf must not print it again; a set
	 * of bare photographs does not, and without this the shelf is unlabeled
	 * and you are asked to tell a Master System from a Genesis by silhouette.
	 * A property of the set, so a new one declares which kind it is rather
	 * than the shelf special-casing a directory by name. */
	bool labeled;
	/* Clear air between the art and its reflection, as a fraction of the
	 * card's half height - so it scales with the card rather than being a
	 * fixed number of screen pixels, which would sit wrong on a side card
	 * that is scaled and yawed.
	 *
	 * Zero for a drawn card, because a card IS a thing resting on a
	 * reflective surface and contact is what that looks like. A photograph of
	 * a console is an object above the surface, and the gap is what says so.
	 * Applies to system art only: box art is card-like whatever the theme. */
	float reflect_gap;
} card_set;

static const card_set CARD_SETS[] = {
	{ "classic", "classic", "Plain Jane",  true,  0.00f },
	{ "fancy",   "fancy",   "Fancy Pants", false, 0.21f },   /* about 50px */
};
#define CARD_SET_COUNT ((int)(sizeof CARD_SETS / sizeof CARD_SETS[0]))

/* The set a stored name selects, by index. An unknown name is the default
 * rather than an error: the setting outlives the directory it points at, and a
 * set that has been removed should leave a working shelf behind. */
static inline int cards_index(const char *id)
{
	int i;

	if (id && *id)
		for (i = 0; i < CARD_SET_COUNT; i++)
			if (!strcmp(CARD_SETS[i].id, id)) return i;
	return 0;
}

/* Stepping wraps, because two entries with a stop at each end would make the
 * row feel broken in one direction half the time. */
static inline int cards_step(int i, int d)
{
	int n = CARD_SET_COUNT;

	if (n <= 0) return 0;
	i = (i + d) % n;
	return i < 0 ? i + n : i;
}

/* THE ORIGIN IS THE TOP LEFT, and the index grows rightward and downward:
 * index 0 at the left of the row, and at the top of the vertical shelf, so
 * down advances there the way right does here. cf_draw, ui_rail_v and the
 * d-pad all follow it.
 *
 * Until 2026-09-30 (plorpos-gkd.45) the vertical keys and rail ran bottom-up,
 * a rule left over from when Vertical was a cube, while cf_draw had long
 * stacked the cards top-down - so the next card came in from below on UP,
 * which read as inverted. Anything that reads as backwards in one direction
 * should be checked against this rule before being corrected.
 *
 * Which way a shelf runs. Its own setting rather than more entries in the
 * table above, because it is orthogonal to the art: all three themes read
 * sensibly either way, so folding it in would mean six presets to name and
 * keep consistent instead of three plus a direction. Applies to the games
 * shelf as well as the systems one. */
#define CARDS_DIR_DEFAULT "horizontal"

typedef struct {
	const char *id;
	const char *name;
	bool vertical;
} card_dir;

static const card_dir CARD_DIRS[] = {
	{ "horizontal", "Horizontal", false },
	{ "vertical",   "Vertical",   true  },
	/* "both" (Cubic) was removed 2026-09-30 (plorpos-gkd.29). A stored "both"
	 * is an unknown id and reads as the default, Horizontal. */
};
#define CARD_DIR_COUNT ((int)(sizeof CARD_DIRS / sizeof CARD_DIRS[0]))

static inline int cards_dir_index(const char *id)
{
	int i;

	if (id && *id)
		for (i = 0; i < CARD_DIR_COUNT; i++)
			if (!strcmp(CARD_DIRS[i].id, id)) return i;
	return 0;
}

static inline int cards_dir_step(int i, int d)
{
	int n = CARD_DIR_COUNT;

	if (n <= 0) return 0;
	i = (i + d) % n;
	return i < 0 ? i + n : i;
}

#endif
