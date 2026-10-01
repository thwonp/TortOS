/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef TORTOS_LIBRARY_H
#define TORTOS_LIBRARY_H

#include <stdbool.h>
#include <stddef.h>
#include <strings.h>

#define LIB_NAME 256
#define LIB_PATH 544

typedef struct {
	char name[LIB_NAME];  /* the filename without its extension. Box art is
	                       * looked up by this, so it stays exactly as the
	                       * file is named. */
	char title[LIB_NAME]; /* what the shelf shows and sorts by: a gamelist's
	                       * name for the game, else (Arcade, Neo Geo) FBNeo's,
	                       * else `name` with the trailing region and dump tags
	                       * cut off, so a card says "Chrono Trigger" rather
	                       * than the cataloging that follows it. titles.c. */
	char file[LIB_PATH];  /* launch path relative to Roms/<folder>: a filename,
	                       * or "<folder>/<disc>" for a disc-folder game */
	/* The ROM's mtime, for the recently-added sort order.
	 *
	 * The scan did not stat files before this - readdir's d_type answers the
	 * only question it had - so this is real added work. Measured on the
	 * device with the page cache dropped: a readdir sweep of all 3380 files
	 * is 0.20s and a stat sweep is 0.23s, so about 30ms for the whole card.
	 * exfat keeps the timestamps in the directory entry, which is why the
	 * block is already read by the time the stat is asked for. */
	long added;
	/* Sort keys, filled by sort_apply just before a sort that needs them and
	 * meaningless otherwise. They live here rather than in a parallel array
	 * so the entries can be moved by qsort with their keys attached - which
	 * is the whole reason the keys are precomputed. */
	long play_secs;
	long last_played;
} game_entry;

/* How many of the left-out names a scan keeps for the log. */
#define LIB_SKIPS_SHOWN 3

typedef struct {
	game_entry *items;
	int count;
	bool scanned;
	/* What the scan left out: files of a type this system does not take, and
	 * folders with nothing in them to launch. Counted, with the first few by
	 * name, so the log can answer "I copied it and it is not on the shelf" -
	 * which it could not, since a skip was silent. Dot entries and .media
	 * are not counted: nobody put those there expecting a game. */
	int skipped;
	char skipped_eg[LIB_SKIPS_SHOWN][LIB_NAME];
} game_list;

/* Scan Roms/<folder> for files whose extension appears in exts (a comma or
 * space separated list, without dots; empty means take everything). */
/* Whether a filename is a disc image rather than a cartridge. */
bool lib_is_disc(const char *name);

bool lib_scan(const char *roms_root, const char *folder, const char *exts,
              game_list *out);
void lib_free(game_list *l);

/* The order a shelf is read in: by title, and by name where two titles are
 * the same - two dumps of one game share a title, and without the name their
 * order would be whatever qsort left. Every order falls back to this. */
static inline int lib_order(const game_entry *a, const game_entry *b)
{
	int c = strcasecmp(a->title, b->title);

	return c ? c : strcasecmp(a->name, b->name);
}

/* Into lib_order, after the titles change (titles_apply). */
void lib_sort(game_list *l);

/* The display title for a ROM's name: everything up to the first bracketed
 * group that follows a space, so "Contra (USA)" reads as "Contra". Exposed
 * because the play-time screen shows names too and had been printing the
 * cataloging with them - the rule belongs in one place, not two.
 *
 * Give it a name with no extension. `out` should take LIB_NAME bytes. */
void lib_title(const char *name, char *out, size_t n);

#endif
