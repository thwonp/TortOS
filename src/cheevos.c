/* SPDX-License-Identifier: MIT */
/* See cheevos.h for the division of labor with Diatom, and why the network
 * half of this is a host-side tool. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atomic.h"
#include "cheevos.h"
#include "db.h"

static cheevo g_ach[CHV_MAX];
static int    g_n;
static int    g_game, g_console;
static char   g_title[CHV_TITLE];
static char   g_src[1024];         /* the file loaded, for chv_write_active */

/* The earned store, whole-library. Sized so a player who finishes a hundred
 * games never meets the cap; at 12 bytes a row that is 48KB, which on a device
 * with a gigabyte is not worth a smarter structure. */
#define CHV_EARNED_MAX 4096
static struct { int game, id; bool synced; } g_earned[CHV_EARNED_MAX];
static int  g_nearned;
static bool g_earned_dirty;

void chv_path(const char *roms_root, const char *folder, const char *name,
              char *out, size_t n)
{
	snprintf(out, n, "%s/%s/.cheevos/%s.set", roms_root, folder, name);
}

static bool is_earned(int game, int id)
{
	int i;
	for (i = 0; i < g_nearned; i++)
		if (g_earned[i].game == game && g_earned[i].id == id) return true;
	return false;
}

void chv_clear(void)
{
	memset(g_ach, 0, sizeof g_ach);
	g_n = 0;
	g_game = g_console = 0;
	g_title[0] = '\0';
	g_src[0] = '\0';
}

/* "#!\ttortos-cheevos 1\tgame=1459\tconsole=7\ttitle=Blaster Master"
 * Fields are looked up by name rather than by position, so the generator can
 * add one without this having to be changed in step. */
static void parse_header(char *line)
{
	char *save = NULL, *f;

	for (f = strtok_r(line, "\t", &save); f; f = strtok_r(NULL, "\t", &save)) {
		if      (!strncmp(f, "game=", 5))    g_game = atoi(f + 5);
		else if (!strncmp(f, "console=", 8)) g_console = atoi(f + 8);
		else if (!strncmp(f, "title=", 6))
			snprintf(g_title, sizeof g_title, "%s", f + 6);
	}
}

/* "#:\t24698\t0\tBringing the Heat-Seeker\tUse a homing missile." */
static void parse_meta(char *line)
{
	char *save = NULL, *f;
	cheevo *c;

	if (g_n >= CHV_MAX) return;
	c = &g_ach[g_n];
	memset(c, 0, sizeof *c);

	f = strtok_r(line, "\t", &save);              /* "#:" */
	if (!f) return;
	if (!(f = strtok_r(NULL, "\t", &save))) return;
	c->id = atoi(f);
	if (c->id <= 0) return;
	if ((f = strtok_r(NULL, "\t", &save))) c->points = atoi(f);
	if ((f = strtok_r(NULL, "\t", &save)))
		snprintf(c->title, sizeof c->title, "%s", f);
	if ((f = strtok_r(NULL, "\t", &save)))
		snprintf(c->desc, sizeof c->desc, "%s", f);

	c->earned = is_earned(g_game, c->id);
	g_n++;
}

bool chv_load(const char *set_path)
{
	char *line = NULL;
	size_t cap = 0;
	FILE *f;

	chv_clear();
	if (!set_path || !*set_path) return false;

	f = fopen(set_path, "r");
	if (!f) return false;
	snprintf(g_src, sizeof g_src, "%s", set_path);

	/* getline, not a fixed buffer: a condition line has been measured at
	 * 30,897 characters. This only reads the comment lines, but it has to get
	 * PAST the long ones intact, and a short buffer would split one and feed
	 * its tail back as another record. */
	while (getline(&line, &cap, f) > 0) {
		line[strcspn(line, "\r\n")] = '\0';
		if (!strncmp(line, "#!", 2))      parse_header(line);
		else if (!strncmp(line, "#:", 2)) parse_meta(line);
	}
	free(line);
	fclose(f);

	if (g_n == 0) { chv_clear(); return false; }
	return true;
}

int chv_count(void) { return g_n; }
int chv_game(void) { return g_game; }
int chv_console(void) { return g_console; }
const char *chv_game_title(void) { return g_title; }

const cheevo *chv_at(int i)
{
	return (i >= 0 && i < g_n) ? &g_ach[i] : NULL;
}

int chv_earned(void)
{
	int i, k = 0;
	for (i = 0; i < g_n; i++) if (g_ach[i].earned || g_ach[i].earned_now) k++;
	return k;
}

int chv_new_this_session(void)
{
	int i, k = 0;
	for (i = 0; i < g_n; i++) if (g_ach[i].earned_now) k++;
	return k;
}

int chv_points_earned(void)
{
	int i, p = 0;
	for (i = 0; i < g_n; i++)
		if (g_ach[i].earned || g_ach[i].earned_now) p += g_ach[i].points;
	return p;
}

int chv_points_total(void)
{
	int i, p = 0;
	for (i = 0; i < g_n; i++) p += g_ach[i].points;
	return p;
}

bool chv_write_active(const char *path)
{
	char  *line = NULL;
	size_t cap = 0;
	FILE  *in, *out;
	int    kept = 0;

	if (!g_src[0] || !path || !*path) return false;
	in = fopen(g_src, "r");
	if (!in) return false;
	/* Atomically too, though this one is scratch: Diatom is handed the path
	 * moments after and a torn file would be a half a set it takes at face
	 * value. Cheap, and one rule rather than two. */
	out = atomic_open(path, 0644);
	if (!out) { fclose(in); return false; }

	while (getline(&line, &cap, in) > 0) {
		int id;
		char *tab;
		int i;

		if (line[0] == '#') continue;             /* Diatom skips these anyway */
		tab = strchr(line, '\t');
		if (!tab) continue;
		id = atoi(line);
		if (id <= 0) continue;

		/* Already earned: leave it out. Sending it would have Diatom fire it
		 * again the moment its conditions happen to be true, and a player who
		 * beat a boss last week would be told they just did. */
		for (i = 0; i < g_n; i++)
			if (g_ach[i].id == id &&
			    (g_ach[i].earned || g_ach[i].earned_now)) break;
		if (i < g_n) continue;

		fputs(line, out);
		kept++;
	}
	free(line);
	fclose(in);

	/* Nothing left to watch. Discard rather than commit, so the previous
	 * game's active set is not left standing under this game's name. */
	if (kept == 0) { atomic_abort(out, path); remove(path); return false; }
	return atomic_commit(out, path);
}

bool chv_note_earned(int game, int id, bool synced)
{
	int i;

	if (game <= 0 || id <= 0) return false;
	for (i = 0; i < g_nearned; i++) {
		if (g_earned[i].game != game || g_earned[i].id != id) continue;
		/* Already known. Learning it is on the account is still news to the
		 * store, even though the achievement is not. */
		if (synced && !g_earned[i].synced) {
			g_earned[i].synced = true;
			g_earned_dirty = true;
		}
		return false;
	}
	if (g_nearned >= CHV_EARNED_MAX) return false;
	g_earned[g_nearned].game   = game;
	g_earned[g_nearned].id     = id;
	g_earned[g_nearned].synced = synced;
	g_nearned++;
	g_earned_dirty = true;

	/* If it belongs to the game that is loaded, the list on screen should say
	 * so without waiting for a reload. */
	if (game == g_game)
		for (i = 0; i < g_n; i++)
			if (g_ach[i].id == id) { g_ach[i].earned = true; break; }
	return true;
}

int chv_pending_count(void)
{
	int i, k = 0;
	for (i = 0; i < g_nearned; i++) if (!g_earned[i].synced) k++;
	return k;
}

bool chv_pending_at(int n, int *game, int *id)
{
	int i;

	for (i = 0; i < g_nearned; i++) {
		if (g_earned[i].synced) continue;
		if (n-- > 0) continue;
		*game = g_earned[i].game;
		*id   = g_earned[i].id;
		return true;
	}
	return false;
}

void chv_mark_synced(int game, int id)
{
	int i;

	for (i = 0; i < g_nearned; i++)
		if (g_earned[i].game == game && g_earned[i].id == id &&
		    !g_earned[i].synced) {
			g_earned[i].synced = true;
			g_earned_dirty = true;
		}
}

bool chv_note_unlock(int id)
{
	int i;

	for (i = 0; i < g_n; i++) {
		if (g_ach[i].id != id) continue;
		if (g_ach[i].earned || g_ach[i].earned_now) return false;
		g_ach[i].earned_now = true;
		/* Pending: the send happens where the launcher owns the screen, not
		 * here. This runs inside the wait loop, which is also the power
		 * button's watchdog, and a blocking request would make the device
		 * stop answering it. */
		chv_note_earned(g_game, id, false);
		return true;
	}
	/* An id we do not have metadata for. It still happened, so it is still
	 * recorded - a set refreshed mid-session is the obvious way to get here,
	 * and losing the unlock would be worse than not being able to name it. */
	return g_game ? chv_note_earned(g_game, id, false) : false;
}

static bool chv_earned_row(const char *key, const char *value, void *ctx)
{
	const char *rest = key + strlen("chv.");
	int game, id;

	(void)ctx;
	if (g_nearned >= CHV_EARNED_MAX) return false;
	/* Both halves or the row is not one. chv_earned_save writes both. */
	if (sscanf(rest, "%d.%d", &game, &id) != 2) return true;
	if (game <= 0 || id <= 0) return true;
	g_earned[g_nearned].game   = game;
	g_earned[g_nearned].id     = id;
	g_earned[g_nearned].synced = value[0] == 's';
	g_nearned++;
	return true;
}

void chv_earned_load(void)
{
	g_nearned = 0;
	g_earned_dirty = false;
	db_each_prefix(db_lib(), "chv.", chv_earned_row, NULL);
}

bool chv_earned_save(void)
{
	char key[48];
	int i;

	if (!g_earned_dirty) return true;
	/* Every row, every time. Nothing here is ever removed - the load is the
	 * only thing that resets the count - so a write-all cannot leave a stale
	 * row behind, and there is no deletion pass to get wrong.
	 *
	 * This is the only record of anything earned that the account has not
	 * seen, and it is written on every unlock - including the ones that
	 * happen just before somebody presses the power button. */
	for (i = 0; i < g_nearned; i++) {
		snprintf(key, sizeof key, "chv.%d.%d", g_earned[i].game, g_earned[i].id);
		if (!db_set_str(db_lib(), key, g_earned[i].synced ? "s" : "p"))
			return false;
	}
	g_earned_dirty = false;
	return true;
}
