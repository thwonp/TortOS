/* SPDX-License-Identifier: MIT */
/* Does the on-device gamelist.xml import read what a scraper writes, and agree
 * with tools/gamelist-import.py, which does the same job from a computer?
 *
 *   gamelist-check ROMS META
 *
 * ROMS is a Roms/ tree (tools/fixtures/gamelist/Roms by default, via make) and
 * META is the script's --meta output for that same tree: every record in it
 * must be what gl_parse makes of the same game, field for field.
 *
 * Links src/gamelist.c and src/db.c and NOT SDL. */
#include "../src/gamelist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;

static void ck(int cond, const char *what)
{
	if (!cond) { printf("  FAIL %s\n", what); fails++; }
}

#define DEV "/tmp/tortos-gamelist-check-device.db"
#define LIB "/tmp/tortos-gamelist-check-library.db"

static void scrub(void)
{
	const char *suffix[] = { "", "-wal", "-shm" };
	char p[256];
	int i;

	for (i = 0; i < 3; i++) {
		snprintf(p, sizeof p, "%s%s", DEV, suffix[i]); unlink(p);
		snprintf(p, sizeof p, "%s%s", LIB, suffix[i]); unlink(p);
	}
}

/* Every game one parse produced, to look up by file name. */
#define MAX_GAMES 4096
typedef struct { char file[256]; game_meta m; } got;
static got *games;
static int ngames;

static void collect(void *ctx, const char *file, const game_meta *m)
{
	(void)ctx;
	if (ngames >= MAX_GAMES) return;
	snprintf(games[ngames].file, sizeof games[ngames].file, "%s", file);
	games[ngames].m = *m;
	ngames++;
}

static const game_meta *find(const char *file)
{
	int i;

	for (i = 0; i < ngames; i++)
		if (!strcmp(games[i].file, file)) return &games[i].m;
	return NULL;
}

static int parse_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	static char buf[8 << 20];
	size_t n;

	ngames = 0;
	if (!f) return -2;
	n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = '\0';
	return gl_parse(buf, n, collect, NULL);
}

static void fixture_values(const char *roms)
{
	char path[1024];
	const game_meta *m;

	snprintf(path, sizeof path, "%s/Game Boy Color/gamelist.xml", roms);
	ck(parse_file(path) == 3, "three usable games (no <game/>, no path-less game)");

	m = find("Alpha & Omega.gbc");
	ck(m != NULL, "entity in <path> decoded");
	if (m) {
		ck(!strcmp(m->title, "Alpha & Omega"), "title from <name>, decoded and trimmed");
		ck(!strcmp(m->year, "1999"), "year is the first four digits");
		ck(!strcmp(m->publisher, "Rock & Roll Inc."), "publisher decoded and trimmed");
		ck(!strcmp(m->developer, "Dev Tabbed"), "a tab in a field becomes a space");
		ck(!strcmp(m->genres, "Action, Puzzle"), "genre");
		ck(!strcmp(m->players, "1-2"), "players");
		ck(!strcmp(m->note, "17"), "rating 0.85 is 17 out of 20");
		ck(!strcmp(m->esrb, ""), "no esrb in a gamelist");
		ck(!strcmp(m->synopsis,
		           "Line one, caf\xc3\xa9 \xe2\x80\x99quoted\xe2\x80\x99.\nLine two <b>."),
		   "synopsis: numeric entities, CRLF read as LF, named entities");
	}
	m = find("Beta.gbc");
	ck(m != NULL, "a subfolder path keeps only the file name");
	if (m) {
		ck(!strcmp(m->synopsis, "Raw <b>bold</b> & stuff"), "CDATA taken raw");
		ck(!strcmp(m->note, ""), "rating 0 is unrated, not 0");
		ck(!strcmp(m->year, ""), "a releasedate with no year gives none");
		ck(!strcmp(m->genres, ""), "self-closing <genre/> is empty");
		ck(!strcmp(m->title, ""), "no <name> is no title");
	}
	m = find("Gamma.gbc");
	ck(m != NULL && !m->synopsis[0], "game with only a path");

	ck(gl_parse("not xml", 7, collect, NULL) == -1, "no <gameList> is not a gamelist");
}

static void import(const char *roms)
{
	systems_cfg sys = { 0 };
	gl_result r;
	game_meta m;

	/* Game Boy Color: gamelist.xml. Game Boy: only miyoogamelist.xml. SNES:
	 * not a gamelist. Genesis: none at all. gb/ is on the card but is no
	 * system's folder, so it is never looked at. */
	snprintf(sys.systems[0].folder, CFG_STR, "Game Boy Color");
	snprintf(sys.systems[1].folder, CFG_STR, "Game Boy");
	snprintf(sys.systems[2].folder, CFG_STR, "SNES");
	snprintf(sys.systems[3].folder, CFG_STR, "Genesis");
	sys.count = 5;                      /* the fifth has no folder: Favorites */

	scrub();
	ck(db_init(DEV, LIB, NULL), "databases open");
	/* Delta already has a row with no title - a ScreenScraper hit from
	 * before titles, say. */
	memset(&m, 0, sizeof m);
	snprintf(m.publisher, sizeof m.publisher, "kept");
	db_game_set(db_lib(), "Game Boy", "Delta.gb", &m);
	gl_import(db_lib(), roms, &sys, false, &r);
	ck(r.lists == 2 && r.wrote == 4 && r.skipped == 0 && r.bad == 0,
	   "first import writes all four, Delta's title included");
	ck(r.unreadable == 1 && !strcmp(r.first_unreadable, "SNES"),
	   "the SNES file is reported, not fatal");
	ck(db_game_get(db_lib(), "Game Boy", "Delta.gb", &m) &&
	   !strcmp(m.title, "Delta"), "miyoogamelist.xml used when there is no gamelist.xml");
	ck(!strcmp(m.publisher, "kept") && !strcmp(m.year, ""),
	   "a title fills its gap and nothing else on the row moves");

	/* Something already there is left alone, a title included. */
	db_game_get(db_lib(), "Game Boy Color", "Alpha & Omega.gbc", &m);
	snprintf(m.title, sizeof m.title, "Old");
	db_game_set(db_lib(), "Game Boy Color", "Alpha & Omega.gbc", &m);
	gl_import(db_lib(), roms, &sys, false, &r);
	ck(r.wrote == 0 && r.skipped == 4, "second import skips all four");
	ck(db_game_get(db_lib(), "Game Boy", "Delta.gb", &m) &&
	   !strcmp(m.publisher, "kept"), "a skipped row is untouched");
	ck(db_game_get(db_lib(), "Game Boy Color", "Alpha & Omega.gbc", &m) &&
	   !strcmp(m.title, "Old"), "and so is a title it already had");

	gl_import(db_lib(), roms, &sys, true, &r);
	ck(r.wrote == 4 && r.skipped == 0, "overwrite writes all four");
	ck(db_game_get(db_lib(), "Game Boy", "Delta.gb", &m) &&
	   !strcmp(m.publisher, ""), "overwrite replaces the row");
	ck(db_game_get(db_lib(), "Game Boy Color", "Alpha & Omega.gbc", &m) &&
	   !strcmp(m.title, "Alpha & Omega"), "the title with it");
	ck(db_game_get(db_lib(), "Game Boy Color", "Beta.gbc", &m) && !m.title[0],
	   "and a game the list gives no name keeps none");
	db_shutdown();
	scrub();
}

/* C field as the script would have it after --meta's own snprintf: equal, or
 * - where the script's value is longer than the buffer - a prefix of it that
 * stops on a character boundary. */
static bool same(const char *c, const char *py)
{
	size_t n = strlen(c);

	return !strcmp(c, py) || (strlen(py) > n && !strncmp(c, py, n));
}

/* The script's records, each against gl_parse's view of the same game. */
static void parity(const char *roms, const char *meta)
{
	FILE *f = fopen(meta, "rb");
	char head[2048], folder[256] = "", path[1024];
	static char syn[1 << 20];
	int records = 0;

	ck(f != NULL, "script output readable");
	if (!f) return;
	while (fgets(head, sizeof head, f)) {
		char *fld[11], *p = head, what[512];
		const game_meta *m;
		size_t len;
		int i;

		for (i = 0; i < 10 && (fld[i] = p) && (p = strchr(p, '\t')); i++)
			*p++ = '\0';
		if (i < 10) { ck(0, "script record has eleven fields"); break; }
		fld[10] = p;
		len = strtoul(fld[10], NULL, 10);
		if (len >= sizeof syn || fread(syn, 1, len, f) != len) {
			ck(0, "script synopsis readable");
			break;
		}
		syn[len] = '\0';
		fgetc(f);
		if (strcmp(folder, fld[0])) {
			snprintf(folder, sizeof folder, "%s", fld[0]);
			snprintf(path, sizeof path, "%s/%s/gamelist.xml", roms, folder);
			if (access(path, F_OK))
				snprintf(path, sizeof path, "%s/%s/miyoogamelist.xml", roms, folder);
			parse_file(path);
		}
		records++;
		m = find(fld[1]);
		snprintf(what, sizeof what, "%s/%s: in both", fld[0], fld[1]);
		ck(m != NULL, what);
		if (!m) continue;
#define FIELD(name, n) \
		snprintf(what, sizeof what, "%s/%s: %s", fld[0], fld[1], #name); \
		ck(same(m->name, fld[n]), what);
		FIELD(year, 2) FIELD(publisher, 3) FIELD(developer, 4)
		FIELD(players, 5) FIELD(genres, 6) FIELD(esrb, 7) FIELD(note, 8)
		FIELD(title, 9)
#undef FIELD
		snprintf(what, sizeof what, "%s/%s: synopsis", fld[0], fld[1]);
		ck(same(m->synopsis, syn), what);
	}
	fclose(f);
	printf("  parity: %d record(s) compared\n", records);
	ck(records > 0, "the script wrote something to compare");
}

int main(int argc, char **argv)
{
	const char *roms = argc > 1 ? argv[1] : "tools/fixtures/gamelist/Roms";

	games = calloc(MAX_GAMES, sizeof *games);
	if (!games) return 1;
	if (argc > 3) {
		/* Parity only, on someone's real card: ROMS META --parity. */
		parity(roms, argv[2]);
	} else {
		fixture_values(roms);
		import(roms);
		if (argc > 2) parity(roms, argv[2]);
	}
	if (fails) { printf("gamelist-check: %d failure(s)\n", fails); return 1; }
	printf("ok: gamelist import reads what a scraper writes\n");
	return 0;
}
