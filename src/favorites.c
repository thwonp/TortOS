/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* Favorites. See favorites.h for the keying; this is the storage.
 *
 * One row per favorite in the library database, keyed "fav.<tag>\t<file>".
 * The tab survives the move for the reason it was chosen: a ROM filename may
 * legally contain almost anything except a tab or a newline - spaces,
 * brackets, commas and parentheses are routine in No-Intro names - so it is
 * the one separator that never needs escaping, in a key as much as in a line.
 *
 * A linear scan over at most FAV_MAX entries. The list is read once per drawn
 * game card, which sounds like a lot until you notice that a shelf shows
 * about seven cards and a favorite list is realistically a dozen entries.
 * Sorting it or hashing it would be machinery bought for a problem nobody
 * has.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"
#include "favorites.h"

typedef struct {
	char tag[FAV_TAG_MAX];
	char file[FAV_FILE_MAX];
} fav_entry;

static fav_entry g_fav[FAV_MAX];
static int g_count;

static int find(const char *tag, const char *file)
{
	int i;
	if (!tag || !file) return -1;
	for (i = 0; i < g_count; i++)
		if (!strcmp(g_fav[i].tag, tag) && !strcmp(g_fav[i].file, file))
			return i;
	return -1;
}

static bool fav_row(const char *key, const char *value, void *ctx)
{
	const char *pair = key + strlen("fav.");
	const char *tab = strchr(pair, '\t');

	(void)value; (void)ctx;
	if (g_count >= FAV_MAX) return false;
	if (!tab || tab == pair || !tab[1]) return true;      /* not our shape */
	/* Rejected rather than truncated. A tag cut to fit would still match
	 * something on the next lookup - just not the system it came from - and a
	 * favorite that silently points at the wrong shelf is worse than one that
	 * failed to load. */
	if ((size_t)(tab - pair) >= FAV_TAG_MAX || strlen(tab + 1) >= FAV_FILE_MAX)
		return true;
	snprintf(g_fav[g_count].tag, FAV_TAG_MAX, "%.*s", (int)(tab - pair), pair);
	snprintf(g_fav[g_count].file, FAV_FILE_MAX, "%s", tab + 1);
	if (find(g_fav[g_count].tag, g_fav[g_count].file) < 0) g_count++;
	return true;
}

void fav_load(void)
{
	g_count = 0;
	db_each_prefix(db_lib(), "fav.", fav_row, NULL);
}

/* Deletes go first, then the whole set is written back.
 *
 * The file this replaced was rewritten whole on every toggle so that what was
 * on the card could not drift out of step with what is in memory. The same
 * guarantee here needs the removals to be explicit, because a row nobody
 * writes is a row that stays. Collecting them before touching anything is
 * deliberate: deleting while enumerating the same prefix is asking the
 * question and changing the answer at once. */
struct fav_gone { char key[FAV_TAG_MAX + FAV_FILE_MAX + 8]; struct fav_gone *next; };

static bool fav_stale(const char *key, const char *value, void *ctx)
{
	struct fav_gone **head = ctx;
	const char *pair = key + strlen("fav.");
	const char *tab = strchr(pair, '\t');
	char tag[FAV_TAG_MAX];
	struct fav_gone *g;

	(void)value;
	if (tab && (size_t)(tab - pair) < FAV_TAG_MAX) {
		snprintf(tag, sizeof tag, "%.*s", (int)(tab - pair), pair);
		if (find(tag, tab + 1) >= 0) return true;         /* still favorited */
	}
	if (!(g = malloc(sizeof *g))) return false;
	snprintf(g->key, sizeof g->key, "%s", key);
	g->next = *head;
	*head = g;
	return true;
}

bool fav_save(void)
{
	struct fav_gone *gone = NULL, *g;
	char key[FAV_TAG_MAX + FAV_FILE_MAX + 8];
	int i;

	db_each_prefix(db_lib(), "fav.", fav_stale, &gone);
	while ((g = gone)) {
		gone = g->next;
		db_del(db_lib(), g->key);
		free(g);
	}
	for (i = 0; i < g_count; i++) {
		/* Bounded explicitly. The fields cannot overflow key - it is sized
		 * from both maxima plus the prefix - but nothing in the types says
		 * so, and an unbounded %s here warns on the device compiler while
		 * passing on the development one. */
		snprintf(key, sizeof key, "fav.%.*s\t%.*s",
		         FAV_TAG_MAX - 1, g_fav[i].tag,
		         FAV_FILE_MAX - 1, g_fav[i].file);
		if (!db_set_str(db_lib(), key, "1")) return false;
	}
	return true;
}

bool fav_is(const char *tag, const char *file)
{
	return find(tag, file) >= 0;
}

bool fav_toggle(const char *tag, const char *file)
{
	int i = find(tag, file);

	if (i >= 0) {
		/* Order does not matter - the shelf sorts itself - so closing the gap
		 * with the last entry beats shuffling everything down. */
		g_fav[i] = g_fav[--g_count];
		return false;
	}
	if (g_count >= FAV_MAX || !tag || !file || !*tag || !*file) return false;
	if (strlen(tag) >= FAV_TAG_MAX || strlen(file) >= FAV_FILE_MAX) return false;
	memcpy(g_fav[g_count].tag, tag, strlen(tag) + 1);
	memcpy(g_fav[g_count].file, file, strlen(file) + 1);
	g_count++;
	return true;
}

int fav_count(void) { return g_count; }

bool fav_at(int i, const char **tag, const char **file)
{
	if (i < 0 || i >= g_count) return false;
	if (tag)  *tag  = g_fav[i].tag;
	if (file) *file = g_fav[i].file;
	return true;
}
