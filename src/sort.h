/* SPDX-License-Identifier: MIT */
#ifndef TORTOS_SORT_H
#define TORTOS_SORT_H

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "library.h"
#include "stats.h"

/* What order a shelf is in.
 *
 * PER SYSTEM, not global. Alphabetical is the only sane way through 261
 * Genesis ROMs, and the worst way through the six-game shelf you are actually
 * working your way down - the two shelves want different answers and there is
 * no reason they should have to agree. Stored as `sort.<TAG>`, keyed on the
 * tag for the reason `display.<TAG>` is: renaming a folder or reordering
 * systems.cfg must not hand a system somebody else's setting.
 *
 * EVERY ORDER FALLS BACK TO NAME - the shown title, then the filename
 * (lib_order). Two games with no play time, or copied onto
 * the card in the same second, must not trade places between two draws of the
 * same shelf - a list that reshuffles when nothing changed reads as broken
 * even when the top of it is right. The fallback is what makes the order a
 * function of the data rather than of whatever qsort felt like.
 *
 * Descending on the three numeric orders, which puts a zero at the bottom for
 * free: a game with no sessions has nothing to say about play time, and the
 * bottom is where "nothing to say" belongs. No special case needed for it.
 *
 * What is NOT here, and why: times launched, which tracks play time closely
 * enough that it would be a second copy of the same shelf; and favorites
 * first, which already has a shelf of its own. Publisher, year, genre and
 * region are not here because nothing on the card knows them - the box art
 * scrapers key on filename rather than a catalog, so those would mean
 * carrying metadata with no source. */
typedef struct {
	const char *name;   /* what the setting stores */
	const char *label;  /* what the menu row shows */
} sort_order;

static const sort_order SORTS[] = {
	{ "name",   "Name"           },
	{ "played", "Play Time"      },
	{ "recent", "Last Played"    },
	{ "added",  "Recently Added" },
};
#define SORT_COUNT ((int)(sizeof SORTS / sizeof SORTS[0]))

static inline int sort_index(const char *name)
{
	int i;

	if (name)
		for (i = 0; i < SORT_COUNT; i++)
			if (!strcmp(SORTS[i].name, name)) return i;
	return 0;                                   /* by name, as it always was */
}

static inline int sort_step(int i, int d)
{
	return (i + d % SORT_COUNT + SORT_COUNT) % SORT_COUNT;
}

/* Whether an order needs the session rows read before it can be applied. */
static inline bool sort_needs_stats(int order)
{
	return order == 1 || order == 2;
}

static int sort_by_name(const void *pa, const void *pb)
{
	return lib_order(pa, pb);
}

/* The three numeric orders, all shaped the same: bigger first, name breaks a
 * tie. Written out rather than folded into one comparator with a key function,
 * because qsort gives a comparator no way to carry which key it wants and a
 * file-static holding it would be a global that exists for one call. */
static int sort_by_played(const void *pa, const void *pb)
{
	const game_entry *a = pa, *b = pb;

	if (a->play_secs != b->play_secs)
		return a->play_secs < b->play_secs ? 1 : -1;
	return sort_by_name(pa, pb);
}

static int sort_by_recent(const void *pa, const void *pb)
{
	const game_entry *a = pa, *b = pb;

	if (a->last_played != b->last_played)
		return a->last_played < b->last_played ? 1 : -1;
	return sort_by_name(pa, pb);
}

static int sort_by_added(const void *pa, const void *pb)
{
	const game_entry *a = pa, *b = pb;

	if (a->added != b->added)
		return a->added < b->added ? 1 : -1;
	return sort_by_name(pa, pb);
}

/* Put `n` entries in `order`.
 *
 * The play-time and last-played keys are read ONCE PER GAME into the entry
 * before sorting, rather than looked up from inside the comparator. stats
 * finds a row by walking its list, so a lookup is linear in the number of
 * sessions - paying that inside a comparator would multiply it by n log n for
 * no reason. One pass costs n lookups and the sort is then plain field
 * comparisons.
 *
 * This re-folds the session rows, which the play-time screen also uses. That
 * screen re-folds on entry under its own window, so neither reads the other's
 * answer - but they are one set of rows and not two. See stats_lookup. */
static inline void sort_apply(game_entry *v, int n, int order, const char *tag)
{
	int i;

	if (!v || n <= 0) return;
	if (order < 0 || order >= SORT_COUNT) order = 0;

	if (sort_needs_stats(order)) {
		stats_summarize(STATS_ALL, false, (long)time(NULL));
		for (i = 0; i < n; i++) {
			v[i].play_secs = 0;
			v[i].last_played = 0;
			stats_lookup(tag, v[i].file, &v[i].play_secs, &v[i].last_played);
		}
	}

	switch (order) {
	case 1:  qsort(v, (size_t)n, sizeof *v, sort_by_played); break;
	case 2:  qsort(v, (size_t)n, sizeof *v, sort_by_recent); break;
	case 3:  qsort(v, (size_t)n, sizeof *v, sort_by_added);  break;
	default: qsort(v, (size_t)n, sizeof *v, sort_by_name);   break;
	}
}

/* The same, for a shelf whose games come from several systems - Favorites.
 *
 * Two things differ, and getting either wrong is silent. A game's play time is
 * under ITS OWN system's tag, `tags[owner[i]]`, and never under the shelf's:
 * asked by the shelf's tag, every Favorites game has none, and the play-time
 * orders fall back to name without saying so. And `owner` runs parallel to the
 * games and is the only record of which core runs each one, so it has to move
 * with them: sorted apart, a game launches under another system's core.
 *
 * qsort moves elements, not pairs, so for the length of the sort each game
 * rides in a pair with its owner, the game first - which is what lets the
 * ordinary comparators read a pair as the game it starts with. False when
 * there was no memory, and nothing moved. */
static inline bool sort_apply_owned(game_entry *v, int *owner, int n, int order,
                                    const char *const *tags)
{
	struct pair { game_entry e; int owner; } *p;
	int i;

	if (!v || !owner || n <= 0) return true;
	if (order < 0 || order >= SORT_COUNT) order = 0;

	if (sort_needs_stats(order)) {
		stats_summarize(STATS_ALL, false, (long)time(NULL));
		for (i = 0; i < n; i++) {
			v[i].play_secs = 0;
			v[i].last_played = 0;
			stats_lookup(tags[owner[i]], v[i].file,
			             &v[i].play_secs, &v[i].last_played);
		}
	}

	p = malloc(sizeof *p * (size_t)n);
	if (!p) return false;
	for (i = 0; i < n; i++) { p[i].e = v[i]; p[i].owner = owner[i]; }
	qsort(p, (size_t)n, sizeof *p,
	      order == 1 ? sort_by_played : order == 2 ? sort_by_recent
	    : order == 3 ? sort_by_added  : sort_by_name);
	for (i = 0; i < n; i++) { v[i] = p[i].e; owner[i] = p[i].owner; }
	free(p);
	return true;
}

#endif
