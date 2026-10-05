/* SPDX-License-Identifier: MIT */
/* What a shelf shows and the order it shows it in: src/titles.c.
 *
 *   - a gamelist's name, from the games table, beats everything;
 *   - on an fbneo shelf, FBNeo's set description comes next, brackets cut;
 *   - otherwise the title lib_scan made from the filename stays;
 *   - one folder's titles never reach another's;
 *   - the shelf is sorted by title, then by name when two titles match. */
#include "../src/titles.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fails;

static void ck(int cond, const char *what)
{
	if (!cond) { printf("  FAIL %s\n", what); fails++; }
}

#define LIB "/tmp/tortos-titles-check.db"
#define DAT "/tmp/tortos-titles-check.tsv"

static void scrub(void)
{
	unlink(LIB);
	unlink(LIB "-wal");
	unlink(LIB "-shm");
	unlink(DAT);
}

/* A shelf the way lib_scan leaves it: name, file, and the filename's title. */
static void add(game_list *l, const char *file)
{
	game_entry *g = &l->items[l->count++];
	char *dot;

	memset(g, 0, sizeof *g);
	snprintf(g->file, sizeof g->file, "%s", file);
	snprintf(g->name, sizeof g->name, "%s", file);
	if ((dot = strrchr(g->name, '.'))) *dot = '\0';
	lib_title(g->name, g->title, sizeof g->title);
}

static const char *title_of(const game_list *l, const char *file)
{
	for (int i = 0; i < l->count; i++)
		if (!strcmp(l->items[i].file, file)) return l->items[i].title;
	return "(not on the shelf)";
}

static void titled(db *d, const char *folder, const char *file, const char *title)
{
	game_meta m;

	memset(&m, 0, sizeof m);
	snprintf(m.title, sizeof m.title, "%s", title);
	db_game_set(d, folder, file, &m);
}

int main(void)
{
	game_entry arcade_items[8], snes_items[8];
	game_list arcade = { arcade_items, 0, true, 0, {{0}} };
	game_list snes = { snes_items, 0, true, 0, {{0}} };
	FILE *f;
	db *d;

	scrub();
	d = db_open(LIB, DB_LIBRARY);
	if (!d) {
		printf("titles-check: no libsqlite3 on this machine, so nothing was checked\n");
		return 77;
	}
	f = fopen(DAT, "w");
	fputs("1943\t1943: The Battle of Midway (Euro)\n"
	      "joust\tJoust (Green label)\n"
	      "mk3\tMortal Kombat 3 (rev 2.1)\n"
	      "no tab here\n"
	      "zzz\tZed\n", f);
	fclose(f);

	add(&arcade, "mk3.zip");
	add(&arcade, "1943.zip");
	add(&arcade, "JOUST.ZIP");
	add(&arcade, "unknown.zip");
	titled(d, "Arcade", "mk3.zip", "MK3 from a gamelist");
	titled(d, "Neo Geo", "1943.zip", "from the wrong folder");
	titles_apply(&arcade, d, "Arcade", DAT);
	ck(!strcmp(title_of(&arcade, "mk3.zip"), "MK3 from a gamelist"),
	   "a gamelist's name beats FBNeo's");
	ck(!strcmp(title_of(&arcade, "1943.zip"), "1943: The Battle of Midway"),
	   "FBNeo's description, brackets cut, and not another folder's title");
	ck(!strcmp(title_of(&arcade, "JOUST.ZIP"), "Joust"), "a set is found whatever its case");
	ck(!strcmp(title_of(&arcade, "unknown.zip"), "unknown"),
	   "a set FBNeo does not list keeps its filename");
	ck(arcade.count == 4 && !strcmp(arcade.items[0].file, "1943.zip") &&
	   !strcmp(arcade.items[1].file, "JOUST.ZIP") &&
	   !strcmp(arcade.items[2].file, "mk3.zip") &&
	   !strcmp(arcade.items[3].file, "unknown.zip"), "sorted by the shown title");

	/* No table for a shelf that is not on fbneo, even with a set's name. */
	add(&snes, "Contra (USA).sfc");
	add(&snes, "mk3.sfc");
	add(&snes, "Contra (Japan).sfc");
	add(&snes, "Axelay (USA).sfc");
	titled(d, "SNES", "Axelay (USA).sfc", "Zzz Axelay");
	titles_apply(&snes, d, "SNES", NULL);
	ck(!strcmp(title_of(&snes, "mk3.sfc"), "mk3"), "no FBNeo titles off an fbneo shelf");
	ck(!strcmp(title_of(&snes, "Axelay (USA).sfc"), "Zzz Axelay"),
	   "a gamelist's name on any shelf, as the list wrote it");
	ck(!strcmp(snes.items[0].file, "Contra (Japan).sfc") &&
	   !strcmp(snes.items[1].file, "Contra (USA).sfc") &&
	   !strcmp(snes.items[2].file, "mk3.sfc") &&
	   !strcmp(snes.items[3].file, "Axelay (USA).sfc"),
	   "two dumps of one title keep their order by name, and the title decides the rest");

	/* The player's own name, from the X menu (plorpos-gkd.86.4). */
	{
		game_entry items[8];
		game_list l = { items, 0, true, 0, {{0}} };
		game_meta m;

		ck(db_game_rename(d, "Arcade", "mk3.zip", "My MK"), "a game a gamelist named can be renamed");
		ck(db_game_rename(d, "Arcade", "unknown.zip", "Aaa Mystery"),
		   "and one nothing has a row for");
		add(&l, "mk3.zip"); add(&l, "1943.zip"); add(&l, "unknown.zip");
		titles_apply(&l, d, "Arcade", DAT);
		ck(!strcmp(title_of(&l, "mk3.zip"), "My MK"), "the rename beats the gamelist's name");
		ck(!strcmp(title_of(&l, "unknown.zip"), "Aaa Mystery"), "and the filename");
		ck(!strcmp(l.items[0].file, "1943.zip") && !strcmp(l.items[1].file, "unknown.zip") &&
		   !strcmp(l.items[2].file, "mk3.zip"), "and sorts by the new names");
		ck(!db_game_get(d, "Arcade", "unknown.zip", &m),
		   "a row only a rename made is not a scraped game");

		memset(&m, 0, sizeof m);
		snprintf(m.title, sizeof m.title, "MK3 again");
		snprintf(m.synopsis, sizeof m.synopsis, "Fight.");
		ck(db_game_import(d, "Arcade", "mk3.zip", &m, true) == 1, "a gamelist imported again, overwriting");
		snprintf(m.title, sizeof m.title, "Mystery from a list");
		ck(db_game_import(d, "Arcade", "unknown.zip", &m, false) == 1,
		   "an import fills a rename-only row whole, not as a row it must leave alone");
		ck(db_game_get(d, "Arcade", "unknown.zip", &m) && !strcmp(m.synopsis, "Fight."),
		   "with all of it");
		l.count = 0;
		add(&l, "mk3.zip"); add(&l, "unknown.zip");
		titles_apply(&l, d, "Arcade", DAT);
		ck(!strcmp(title_of(&l, "mk3.zip"), "My MK") && !strcmp(title_of(&l, "unknown.zip"), "Aaa Mystery"),
		   "and neither import undid a rename");

		ck(db_game_rename(d, "Arcade", "mk3.zip", ""), "an empty name clears it");
		ck(db_game_rename(d, "Arcade", "unknown.zip", NULL), "and so does none");
		l.count = 0;
		add(&l, "mk3.zip"); add(&l, "unknown.zip");
		titles_apply(&l, d, "Arcade", DAT);
		ck(!strcmp(title_of(&l, "mk3.zip"), "MK3 again"), "back to the gamelist's name");
		ck(!strcmp(title_of(&l, "unknown.zip"), "Mystery from a list"), "both of them");
	}

	db_close(d);
	scrub();
	if (fails) { printf("\n%d FAILED\n", fails); return 1; }
	printf("ok: every shelf shows and sorts by the game's own title\n");
	return 0;
}
