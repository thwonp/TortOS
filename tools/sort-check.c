/* SPDX-License-Identifier: MIT */
/* What order does a shelf end up in?
 *
 * Sorting is the kind of code where a wrong sign compiles, runs, and looks
 * plausible until you are staring at a shelf wondering why the game you played
 * yesterday is at the bottom. There is no error to catch and no crash to
 * bisect - the list is simply in the wrong order, and on a 261-game shelf
 * nobody can tell by eye whether the fourth entry belongs where it is.
 *
 * The property that matters most here is not "biggest first". It is that the
 * order is a FUNCTION OF THE DATA: sort the same list twice and get the same
 * list. Ties are where that fails, because qsort is free to leave equal
 * elements in any order, so every comparator falls back to name and this file
 * is what says so.
 *
 * Links src/stats.c and src/db.c, because two of the four orders read the
 * session rows. It does NOT link src/library.c: game_entry is a struct in the
 * header and nothing here scans a directory.
 */
#include "../src/sort.h"
#include "../src/db.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fails;
static void ck(int cond, const char *what)
{
	if (!cond) { printf("  FAIL %s\n", what); fails++; }
}

#define DEV "/tmp/tortos-sort-dev.db"
#define LIB "/tmp/tortos-sort-lib.db"

static void scrub(void)
{
	const char *base[] = { DEV, LIB };
	const char *suf[] = { "", "-wal", "-shm" };
	char p[160];
	unsigned i, j;

	db_shutdown();
	for (i = 0; i < 2; i++)
		for (j = 0; j < 3; j++) {
			snprintf(p, sizeof p, "%s%s", base[i], suf[j]);
			unlink(p);
		}
	db_init(DEV, LIB, NULL);
}

/* A session at a chosen moment, written directly - see stats-check for why
 * stats_begin cannot place one in the past. */
static void seed(long start, const char *tag, const char *file, long secs)
{
	static unsigned uniq;
	char k[700], v[64];

	snprintf(k, sizeof k, "sess.%ld.%u.%s\t%s", start, ++uniq, tag, file);
	snprintf(v, sizeof v, "%ld\tquit", secs);
	db_set_str(db_lib(), k, v);
}

static void put(game_entry *e, const char *name, long added)
{
	memset(e, 0, sizeof *e);
	snprintf(e->name, sizeof e->name, "%s", name);
	snprintf(e->file, sizeof e->file, "%s.zip", name);
	e->added = added;
}

static const char *first(const game_entry *v) { return v[0].name; }

static void the_table_is_consistent(void)
{
	int i;

	printf("the table of orders:\n");
	ck(SORT_COUNT == 4, "four orders");
	ck(!strcmp(SORTS[0].name, "name"), "name is index 0, the untouched default");
	for (i = 0; i < SORT_COUNT; i++)
		ck(sort_index(SORTS[i].name) == i, SORTS[i].label);
	ck(sort_index("nonsense") == 0, "an unknown setting falls back to name");
	ck(sort_index(NULL) == 0, "and so does a missing one");
	ck(sort_step(0, -1) == SORT_COUNT - 1, "stepping back from the first wraps");
	ck(sort_step(SORT_COUNT - 1, 1) == 0, "and forward from the last wraps");
}

static void by_name(void)
{
	game_entry v[3];

	printf("by name:\n");
	scrub();
	put(&v[0], "Zelda", 0);
	put(&v[1], "Contra", 0);
	put(&v[2], "metroid", 0);
	sort_apply(v, 3, 0, "NES");
	ck(!strcmp(v[0].name, "Contra"), "alphabetical");
	ck(!strcmp(v[1].name, "metroid"), "and case does not decide it");
	ck(!strcmp(v[2].name, "Zelda"), "Zelda last");
}

static void by_play_time(void)
{
	game_entry v[4];
	long now = 1000000;

	printf("by play time:\n");
	scrub();
	seed(now, "NES", "Contra.zip", 600);
	seed(now, "NES", "Contra.zip", 300);      /* folded: 900 */
	seed(now, "NES", "Metroid.zip", 1200);
	/* Zelda and Adventure have no sessions at all. */
	put(&v[0], "Contra", 0);
	put(&v[1], "Metroid", 0);
	put(&v[2], "Zelda", 0);
	put(&v[3], "Adventure", 0);
	sort_apply(v, 4, 1, "NES");

	ck(!strcmp(v[0].name, "Metroid"), "most played first");
	ck(!strcmp(v[1].name, "Contra"), "then the one whose sessions folded to 900");
	/* THE POINT OF THE NAME FALLBACK. Both have zero seconds, so the
	 * comparator has nothing numeric to say and qsort may do as it likes
	 * unless something else decides. */
	ck(!strcmp(v[2].name, "Adventure") && !strcmp(v[3].name, "Zelda"),
	   "unplayed games land last, in name order rather than any order");
}

static void by_last_played(void)
{
	game_entry v[3];

	printf("by last played:\n");
	scrub();
	seed(1000, "NES", "Contra.zip", 60);
	seed(9000, "NES", "Contra.zip", 60);      /* the later one wins */
	seed(5000, "NES", "Metroid.zip", 6000);   /* far longer, but older */
	put(&v[0], "Metroid", 0);
	put(&v[1], "Contra", 0);
	put(&v[2], "Zelda", 0);
	sort_apply(v, 3, 2, "NES");

	ck(!strcmp(v[0].name, "Contra"), "most recently started first");
	ck(!strcmp(v[1].name, "Metroid"),
	   "and a longer session does not make an older one recent");
	ck(!strcmp(v[2].name, "Zelda"), "never played, so last");
}

static void by_recently_added(void)
{
	game_entry v[4];

	printf("by recently added:\n");
	scrub();
	put(&v[0], "Old", 100);
	put(&v[1], "New", 900);
	put(&v[2], "Bravo", 500);
	put(&v[3], "Alpha", 500);       /* same second as Bravo */
	sort_apply(v, 4, 3, "NES");

	ck(!strcmp(v[0].name, "New"), "newest first");
	ck(!strcmp(v[1].name, "Alpha") && !strcmp(v[2].name, "Bravo"),
	   "two copied in the same second read in name order");
	ck(!strcmp(v[3].name, "Old"), "oldest last");
}

/* Sort the same shelf twice and get the same shelf. A list that reshuffles
 * when nothing changed reads as broken even when the top of it is right. */
static void sorting_twice_changes_nothing(void)
{
	game_entry v[5], again[5];
	int order, i;

	printf("the order is a function of the data:\n");
	scrub();
	seed(2000, "NES", "B.zip", 100);
	for (order = 0; order < SORT_COUNT; order++) {
		put(&v[0], "E", 7); put(&v[1], "B", 7); put(&v[2], "A", 7);
		put(&v[3], "D", 7); put(&v[4], "C", 7);
		sort_apply(v, 5, order, "NES");
		memcpy(again, v, sizeof v);
		sort_apply(again, 5, order, "NES");
		for (i = 0; i < 5; i++)
			ck(!strcmp(v[i].name, again[i].name), SORTS[order].label);
	}
}

static void nothing_to_sort(void)
{
	game_entry one;

	printf("edges:\n");
	scrub();
	sort_apply(NULL, 0, 0, "NES");
	sort_apply(NULL, 5, 1, "NES");
	put(&one, "Solo", 0);
	sort_apply(&one, 1, 2, "NES");
	ck(!strcmp(first(&one), "Solo"), "one game survives being sorted");
	sort_apply(&one, 1, 99, "NES");
	ck(!strcmp(first(&one), "Solo"), "and an order that does not exist is name");
}

/* Favorites: games from several systems on one shelf. Each one's play time is
 * under its own system's tag, and its owner has to travel with it - see
 * sort_apply_owned. `added` is set from the owner, so a game that arrives at
 * the end paired with the wrong owner shows it. */
static void favorites_carry_their_owners(void)
{
	const char *tags[] = { "FAV", "NES", "SNES", "GB" };
	game_entry v[3];
	int owner[3] = { 1, 2, 3 }, order, i, paired;

	printf("favorites:\n");
	scrub();
	seed(1000000, "NES", "Contra.zip", 100);
	seed(1000000, "SNES", "Contra.zip", 900);   /* the same file, another system */
	seed(1000000, "GB", "Tetris.zip", 500);
	put(&v[0], "Contra", 1000);
	put(&v[1], "Contra", 2000);
	put(&v[2], "Tetris", 3000);

	sort_apply_owned(v, owner, 3, 1, tags);
	ck(owner[0] == 2 && owner[1] == 3 && owner[2] == 1,
	   "by play time, each game's own system's time, not the shelf's");
	for (order = 0; order < SORT_COUNT; order++) {
		sort_apply_owned(v, owner, 3, order, tags);
		for (i = 0, paired = 1; i < 3; i++)
			if (v[i].added != owner[i] * 1000) paired = 0;
		ck(paired, SORTS[order].label);
	}
}

int main(void)
{
	if (!db_available()) {
		printf("sort-check: no libsqlite3 here, so nothing was checked\n");
		return 77;
	}
	the_table_is_consistent();
	by_name();
	by_play_time();
	by_last_played();
	by_recently_added();
	sorting_twice_changes_nothing();
	nothing_to_sort();
	favorites_carry_their_owners();
	db_shutdown();

	if (fails) { printf("\n%d sort check(s) failed\n", fails); return 1; }
	printf("\nok: a shelf sorts the way it says it does\n");
	return 0;
}
