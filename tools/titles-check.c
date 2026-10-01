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

	db_close(d);
	scrub();
	if (fails) { printf("\n%d FAILED\n", fails); return 1; }
	printf("ok: every shelf shows and sorts by the game's own title\n");
	return 0;
}
