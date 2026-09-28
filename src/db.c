/* SPDX-License-Identifier: MIT */
/* See db.h. No SDL and no platform header, deliberately, so the check can link
 * it without a window - the same reason atomic.c is kept clean. */
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cards.h"
#include "db.h"

/* --- libsqlite3, declared rather than included ---------------------------
 * There is no sqlite3.h on the device or in the sysroot. These nine entry
 * points are the whole surface this file uses, and the C ABI for them has been
 * stable since 3.x. The device ships 3.12.2. */
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

static int  (*sq_open_v2)(const char *, sqlite3 **, int, const char *);
static int  (*sq_close)(sqlite3 *);
static int  (*sq_exec)(sqlite3 *, const char *, void *, void *, char **);
static int  (*sq_prepare)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
static int  (*sq_step)(sqlite3_stmt *);
static int  (*sq_finalize)(sqlite3_stmt *);
static int  (*sq_bind_text)(sqlite3_stmt *, int, const char *, int, void *);
static int  (*sq_bind_int)(sqlite3_stmt *, int, int);
static const unsigned char *(*sq_column_text)(sqlite3_stmt *, int);
static void (*sq_free)(void *);

#define SQ_OK    0
#define SQ_ROW   100
#define SQ_DONE  101
#define SQ_OPEN_RW_CREATE 0x00000006      /* READWRITE | CREATE */
#define SQ_TRANSIENT ((void *)-1)

struct db { sqlite3 *h; };

static int loaded;      /* 0 not tried, 1 ok, -1 failed */

static void *dl_sqlite(void)
{
	static const char *names[] = {
		"libsqlite3.so.0",      /* what the device actually ships */
		"libsqlite3.so",
		"libsqlite3.dylib",     /* so the check runs on a dev machine */
	};
	for (unsigned i = 0; i < sizeof names / sizeof *names; i++) {
		void *h = dlopen(names[i], RTLD_NOW);
		if (h) return h;
	}
	return NULL;
}

bool db_available(void)
{
	void *h;

	if (loaded) return loaded > 0;
	loaded = -1;
	if (!(h = dl_sqlite())) return false;

	sq_open_v2    = dlsym(h, "sqlite3_open_v2");
	sq_close      = dlsym(h, "sqlite3_close");
	sq_exec       = dlsym(h, "sqlite3_exec");
	sq_prepare    = dlsym(h, "sqlite3_prepare_v2");
	sq_step       = dlsym(h, "sqlite3_step");
	sq_finalize   = dlsym(h, "sqlite3_finalize");
	sq_bind_text  = dlsym(h, "sqlite3_bind_text");
	sq_bind_int   = dlsym(h, "sqlite3_bind_int");
	sq_column_text = dlsym(h, "sqlite3_column_text");
	sq_free       = dlsym(h, "sqlite3_free");

	if (!sq_open_v2 || !sq_close || !sq_exec || !sq_prepare || !sq_step ||
	    !sq_finalize || !sq_bind_text || !sq_bind_int || !sq_column_text)
		return false;
	loaded = 1;
	return true;
}

/* --- the defaults, compiled in ------------------------------------------
 * Shipped in the binary rather than as a .db in the payload, so there is no
 * second artifact to drift from this code and a deleted database self-heals
 * into a working one. These values are what config/tortos.cfg shipped. */
static const db_default device_defaults[] = {
	/* A RUNG, 0..PLAT_VOL_MAX, which is what a nudge stores. config/tortos.cfg
	 * shipped 40 percent and the launcher converted; keeping the percentage
	 * here would put two units under one key, and 15 percent would then be
	 * indistinguishable from rung 15. (40 * 20 + 50) / 100 = 8. */
	{ "volume",     "8"  },
	{ "brightness", "7"  },
	{ "audioout",   "auto" },  /* auto | speaker - see src/audioout.h */
	{ "autooff",    "60" },    /* Auto Sleep, seconds without input, 0 is off -
	                              NextUI's Screen timeout default */
	{ "keepawakeusb", "0" },   /* NextUI's Keep awake over USB, default off */
	{ "suspendtimeout", "30" }, /* light sleep to real suspend, seconds, never
	                              0 - NextUI's Suspend timeout default */
	{ "autopoweroff", "0"  },  /* Auto Off, seconds without input, 0 is off -
	                              mutually exclusive with autooff */
	{ "wifi",       "0"  },    /* what THIS device was last doing, not the */
	{ "bluetooth",  "0"  },    /* shipped default - seeding merges the two */
	{ "cards",      CARDS_DEFAULT },
	{ "cards_dir",  CARDS_DIR_DEFAULT },
};

static const db_default library_defaults[] = {
	{ "timezone",       "America/New_York" },
	{ "startup_system", "NES" },

	/* Turbo, per system tag. `x:a~3,y:b~3` says X is a turbo A and Y a turbo
	 * B, three frames pressed and three released - about ten presses a second
	 * at 60 Hz.
	 *
	 * Listed only where BOTH of those are spare. X is the north button and Y
	 * is the west one; a two-button console uses south and east and leaves
	 * them both. A three-button Genesis takes west for its A - measured
	 * 2026-09-06 with Streets of Rage 2 - so it has one free button where
	 * turbo needs two, and SNES uses all four. MD and SFC are absent for that
	 * reason and not by oversight. docs/turbo.md has the table. */
	{ "turbo.NES",  "x:a~3,y:b~3" },
	{ "turbo.SMS",  "x:a~3,y:b~3" },
	{ "turbo.PCE",  "x:a~3,y:b~3" },
	{ "turbo.GB",   "x:a~3,y:b~3" },
	{ "turbo.GBC",  "x:a~3,y:b~3" },
	{ "turbo.NGP",  "x:a~3,y:b~3" },
	{ "turbo.NGPC", "x:a~3,y:b~3" },
	{ "turbo.GBA",  "x:a~3,y:b~3" },
	{ "turbo.GG",   "x:a~3,y:b~3" },

	/* Core options, from config/coreopts.cfg. The segment after "coreopt." is
	 * the system tag and an EMPTY one means global, which is why the first key
	 * has two dots. A tagged entry overrides a global of the same name. */
	{ "coreopt..mgba_sgb_borders",  "OFF" },
	{ "coreopt.GB.mgba_gb_model",   "Game Boy" },
	{ "coreopt.GB.mgba_gb_colors",  "DMG Green" },
};

const db_default *db_defaults(db_scope scope, size_t *count)
{
	if (scope == DB_DEVICE) {
		*count = sizeof device_defaults / sizeof *device_defaults;
		return device_defaults;
	}
	*count = sizeof library_defaults / sizeof *library_defaults;
	return library_defaults;
}

/* --- schema --------------------------------------------------------------
 * WAL and synchronous=NORMAL for both. NORMAL leaves the same ~33 s writeback
 * window an unsynced write has, which is the right trade for a volume level:
 * losing the last nudge to a power cut costs nothing, and FULL costs 5x per
 * write to prevent it. Measured, see tools/storeprobe.c. */
static const char SCHEMA[] =
	"PRAGMA journal_mode=WAL;"
	"PRAGMA synchronous=NORMAL;"
	"CREATE TABLE IF NOT EXISTS settings("
	"  key TEXT PRIMARY KEY NOT NULL,"
	"  value TEXT NOT NULL"
	");";

/* The card's games, one row each. Library scope only - a handheld has no
 * business holding a synopsis for a card that is not in it. See db.h. */
static const char GAMES_SCHEMA[] =
	"CREATE TABLE IF NOT EXISTS games("
	"  folder TEXT NOT NULL,"
	"  file TEXT NOT NULL,"
	"  year TEXT,"
	"  publisher TEXT,"
	"  developer TEXT,"
	"  players TEXT,"
	"  genres TEXT,"
	"  esrb TEXT,"
	"  note TEXT,"
	"  synopsis TEXT,"
	"  scraped INTEGER,"
	"  PRIMARY KEY(folder, file)"
	");";

static bool run(db *d, const char *sql)
{
	char *err = NULL;
	int rc = sq_exec(d->h, sql, NULL, NULL, &err);
	if (err && sq_free) sq_free(err);
	return rc == SQ_OK;
}

db *db_open(const char *path, db_scope scope)
{
	db *d;
	const db_default *def;
	size_t n, i;

	if (!path || !*path || !db_available()) return NULL;
	if (!(d = calloc(1, sizeof *d))) return NULL;

	if (sq_open_v2(path, &d->h, SQ_OPEN_RW_CREATE, NULL) != SQ_OK) {
		if (d->h) sq_close(d->h);
		free(d);
		return NULL;
	}
	if (!run(d, SCHEMA)) { db_close(d); return NULL; }
	if (scope == DB_LIBRARY && !run(d, GAMES_SCHEMA)) { db_close(d); return NULL; }

	/* The device database holds the RetroAchievements session token, so it
	 * asks for 0600. Sidecars included: a -wal holding the same pages at 0644
	 * would be the same secret in a different file. After the schema and not
	 * before, because journal_mode=WAL is what creates them.
	 *
	 * ON THE CARD THIS ACHIEVES NOTHING. /mnt/SDCARD is exfat mounted
	 * fmask=0022; exfat has no Unix permissions, so everything on it reads
	 * back 0755 whatever was asked for. Measured 2026-09-06.
	 *
	 * So do not delete this as dead, and do not trust it as protection. It is
	 * honored on ext4, on a development host, and on any card that is not
	 * exfat. */
	if (scope == DB_DEVICE) {
		char side[1100];
		chmod(path, 0600);
		snprintf(side, sizeof side, "%s-wal", path); chmod(side, 0600);
		snprintf(side, sizeof side, "%s-shm", path); chmod(side, 0600);
	}

	/* Seed only what is absent, so a default never overwrites a choice. That
	 * is the bug the old tortos.cfg had until 2026-08: it was applied over
	 * the top of levels.cfg at every boot, putting the config ahead of the
	 * player. */
	def = db_defaults(scope, &n);
	for (i = 0; i < n; i++)
		if (!db_has(d, def[i].key))
			db_set_str(d, def[i].key, def[i].value);

	return d;
}

void db_close(db *d)
{
	if (!d) return;
	if (d->h) sq_close(d->h);
	free(d);
}

bool db_get_str(db *d, const char *key, char *out, size_t n, const char *fallback)
{
	sqlite3_stmt *st = NULL;
	const unsigned char *v;
	bool got = false;

	if (out && n) out[0] = '\0';
	if (d && key && out && n &&
	    sq_prepare(d->h, "SELECT value FROM settings WHERE key=?;", -1, &st, NULL) == SQ_OK) {
		sq_bind_text(st, 1, key, -1, SQ_TRANSIENT);
		if (sq_step(st) == SQ_ROW && (v = sq_column_text(st, 0))) {
			snprintf(out, n, "%s", (const char *)v);
			got = true;
		}
		sq_finalize(st);
	}
	if (!got && out && n) snprintf(out, n, "%s", fallback ? fallback : "");
	return got;
}

int db_get_int(db *d, const char *key, int fallback)
{
	char buf[64];
	char *end;
	long v;

	if (!db_get_str(d, key, buf, sizeof buf, NULL) || !buf[0]) return fallback;
	v = strtol(buf, &end, 10);
	/* A value that is not a number is not a zero. Saying so beats silently
	 * turning a typo into a setting. */
	if (end == buf || *end) return fallback;
	return (int)v;
}

bool db_has(db *d, const char *key)
{
	sqlite3_stmt *st = NULL;
	bool found = false;

	if (!d || !key) return false;
	if (sq_prepare(d->h, "SELECT 1 FROM settings WHERE key=?;", -1, &st, NULL) != SQ_OK)
		return false;
	sq_bind_text(st, 1, key, -1, SQ_TRANSIENT);
	found = sq_step(st) == SQ_ROW;
	sq_finalize(st);
	return found;
}

bool db_set_str(db *d, const char *key, const char *val)
{
	sqlite3_stmt *st = NULL;
	bool ok;

	if (!d || !key || !val) return false;
	/* INSERT OR REPLACE rather than UPSERT: 3.12.2 predates ON CONFLICT,
	 * and the device is the oldest sqlite this has to run against. */
	if (sq_prepare(d->h, "INSERT OR REPLACE INTO settings(key,value) VALUES(?,?);",
	               -1, &st, NULL) != SQ_OK)
		return false;
	sq_bind_text(st, 1, key, -1, SQ_TRANSIENT);
	sq_bind_text(st, 2, val, -1, SQ_TRANSIENT);
	ok = sq_step(st) == SQ_DONE;
	sq_finalize(st);
	return ok;
}

bool db_set_int(db *d, const char *key, int val)
{
	char buf[32];
	snprintf(buf, sizeof buf, "%d", val);
	return db_set_str(d, key, buf);
}

bool db_del(db *d, const char *key)
{
	sqlite3_stmt *st = NULL;
	bool ok;

	if (!d || !key) return false;
	if (sq_prepare(d->h, "DELETE FROM settings WHERE key=?;", -1, &st, NULL) != SQ_OK)
		return false;
	sq_bind_text(st, 1, key, -1, SQ_TRANSIENT);
	ok = sq_step(st) == SQ_DONE;
	sq_finalize(st);
	return ok;
}

/* --- the games table ------------------------------------------------------ */

static void col_str(sqlite3_stmt *st, int i, char *out, size_t n)
{
	const unsigned char *v = sq_column_text(st, i);
	snprintf(out, n, "%s", v ? (const char *)v : "");
}

bool db_game_get(db *d, const char *folder, const char *file, game_meta *out)
{
	sqlite3_stmt *st = NULL;
	bool got = false;

	if (out) memset(out, 0, sizeof *out);
	if (!d || !folder || !file || !out) return false;
	if (sq_prepare(d->h,
	               "SELECT year,publisher,developer,players,genres,esrb,note,synopsis"
	               " FROM games WHERE folder=? AND file=?;", -1, &st, NULL) != SQ_OK)
		return false;
	sq_bind_text(st, 1, folder, -1, SQ_TRANSIENT);
	sq_bind_text(st, 2, file, -1, SQ_TRANSIENT);
	if (sq_step(st) == SQ_ROW) {
		col_str(st, 0, out->year,      sizeof out->year);
		col_str(st, 1, out->publisher, sizeof out->publisher);
		col_str(st, 2, out->developer, sizeof out->developer);
		col_str(st, 3, out->players,   sizeof out->players);
		col_str(st, 4, out->genres,    sizeof out->genres);
		col_str(st, 5, out->esrb,      sizeof out->esrb);
		col_str(st, 6, out->note,      sizeof out->note);
		col_str(st, 7, out->synopsis,  sizeof out->synopsis);
		got = true;
	}
	sq_finalize(st);
	return got;
}

bool db_game_set(db *d, const char *folder, const char *file, const game_meta *m)
{
	sqlite3_stmt *st = NULL;
	bool ok;

	if (!d || !folder || !file || !m) return false;
	/* INSERT OR REPLACE for the same reason db_set_str uses it: the device
	 * ships sqlite 3.12.2, which predates ON CONFLICT. */
	if (sq_prepare(d->h,
	               "INSERT OR REPLACE INTO games(folder,file,year,publisher,developer,"
	               "players,genres,esrb,note,synopsis,scraped)"
	               " VALUES(?,?,?,?,?,?,?,?,?,?,?);", -1, &st, NULL) != SQ_OK)
		return false;
	sq_bind_text(st,  1, folder,        -1, SQ_TRANSIENT);
	sq_bind_text(st,  2, file,          -1, SQ_TRANSIENT);
	sq_bind_text(st,  3, m->year,       -1, SQ_TRANSIENT);
	sq_bind_text(st,  4, m->publisher,  -1, SQ_TRANSIENT);
	sq_bind_text(st,  5, m->developer,  -1, SQ_TRANSIENT);
	sq_bind_text(st,  6, m->players,    -1, SQ_TRANSIENT);
	sq_bind_text(st,  7, m->genres,     -1, SQ_TRANSIENT);
	sq_bind_text(st,  8, m->esrb,       -1, SQ_TRANSIENT);
	sq_bind_text(st,  9, m->note,       -1, SQ_TRANSIENT);
	sq_bind_text(st, 10, m->synopsis,   -1, SQ_TRANSIENT);
	/* When, so a re-scrape can skip what it already has and a stale row can be
	 * told from one that was never written. Not in game_meta: nothing on a
	 * screen asks for it. */
	sq_bind_int(st, 11, (int)time(NULL));
	ok = sq_step(st) == SQ_DONE;
	sq_finalize(st);
	return ok;
}

void db_each_prefix(db *d, const char *prefix, db_each_fn fn, void *ctx)
{
	sqlite3_stmt *st = NULL;
	const unsigned char *k, *v;
	char hi[256];
	size_t n;

	if (!d || !prefix || !fn) return;
	/* A range rather than LIKE: no escaping question for keys that contain
	 * the wildcards, and it uses the primary key index. The upper bound is
	 * the prefix with its last byte incremented, which is the next key that
	 * cannot share it. */
	n = strlen(prefix);
	if (!n || n + 1 >= sizeof hi) return;
	memcpy(hi, prefix, n + 1);
	hi[n - 1]++;

	if (sq_prepare(d->h, "SELECT key,value FROM settings "
	                     "WHERE key >= ? AND key < ? ORDER BY key;",
	               -1, &st, NULL) != SQ_OK)
		return;
	sq_bind_text(st, 1, prefix, -1, SQ_TRANSIENT);
	sq_bind_text(st, 2, hi, -1, SQ_TRANSIENT);
	while (sq_step(st) == SQ_ROW) {
		k = sq_column_text(st, 0);
		v = sq_column_text(st, 1);
		if (!fn(k ? (const char *)k : "", v ? (const char *)v : "", ctx)) break;
	}
	sq_finalize(st);
}

/* Whether a key's VALUE is a credential, by the name it was given.
 *
 * A convention rather than a list, so a key added next year is covered by
 * having been named honestly rather than by somebody remembering this
 * function. Two exist today: ra.token and ss.password. */
static bool secret_key(const char *k)
{
	size_t n = k ? strlen(k) : 0;
	const char *suffix[] = { ".token", ".password" };
	size_t i, m;

	for (i = 0; i < sizeof suffix / sizeof suffix[0]; i++) {
		m = strlen(suffix[i]);
		if (n >= m && strcmp(k + n - m, suffix[i]) == 0) return true;
	}
	return false;
}

void db_dump(db *d, FILE *out)
{
	sqlite3_stmt *st = NULL;
	const unsigned char *k, *v;

	if (!d || !out) return;
	if (sq_prepare(d->h, "SELECT key,value FROM settings ORDER BY key;",
	               -1, &st, NULL) != SQ_OK) {
		fprintf(out, "  (cannot read settings)\n");
		return;
	}
	while (sq_step(st) == SQ_ROW) {
		k = sq_column_text(st, 0);
		v = sq_column_text(st, 1);
		/* THE SECRETS ARE COUNTED, NOT PRINTED. This tool exists because
		 * there is no sqlite3 on the device, so it is run over adb and its
		 * output lands in a terminal, a log, a paste or a screen share. It
		 * printed ra.token from the day the account arrived and would have
		 * printed ss.password from today - and unlike a token, that one is
		 * the credential itself and is reusable.
		 *
		 * A length is still worth having: it is what tells "nothing stored"
		 * apart from "stored, and something is wrong with it". */
		if (secret_key(k ? (const char *)k : "")) {
			fprintf(out, "  %-16s [hidden, %d characters]\n",
			        (const char *)k, v ? (int)strlen((const char *)v) : 0);
			continue;
		}
		fprintf(out, "  %-16s %s\n", k ? (const char *)k : "?",
		        v ? (const char *)v : "");
	}
	sq_finalize(st);
}

/* --- the two open handles ------------------------------------------------ */
static db *g_dev, *g_lib;
static char g_boot_env[1024];

bool db_init(const char *device_path, const char *library_path,
             const char *boot_env_path)
{
	db_shutdown();
	g_dev = db_open(device_path, DB_DEVICE);
	g_lib = db_open(library_path, DB_LIBRARY);
	snprintf(g_boot_env, sizeof g_boot_env, "%s", boot_env_path ? boot_env_path : "");
	return g_dev && g_lib;
}

void db_shutdown(void)
{
	db_close(g_dev); g_dev = NULL;
	db_close(g_lib); g_lib = NULL;
	g_boot_env[0] = '\0';
}

db *db_dev(void) { return g_dev; }
db *db_lib(void) { return g_lib; }

/* --- boot.env, see db.h -------------------------------------------------- */
bool db_write_boot_env(void)
{
	static char last[512];
	char body[512], tz[64];
	char tmp[1100];
	FILE *f;
	int n;

	if (!g_dev || !g_lib || !g_boot_env[0]) return false;

	/* Shell-sourced, so every value is quoted and nothing is computed here.
	 * The launcher already resolved "the player's choice, else the shipped
	 * default" by seeding, which is why this is four lines and launch.sh no
	 * longer needs a fallback for each one. */
	db_get_str(g_lib, "timezone", tz, sizeof tz, "America/New_York");
	n = snprintf(body, sizeof body,
	             "# Written by TortOS from the settings database. Derived, not\n"
	             "# authoritative: delete it and the next settings change\n"
	             "# rewrites it. Sourced by launch.sh before tortos.elf runs.\n"
	             "BRIGHTNESS='%d'\n"
	             "WIFI='%d'\n"
	             "BLUETOOTH='%d'\n"
	             /* A single quote in a timezone name would break the shell that
	              * sources this. None exist, and a value that could is dropped
	              * rather than escaped: a wrong TZ is cheaper than an
	              * unbootable launch.sh. */
	             "TIMEZONE='%s'\n",
	             db_get_int(g_dev, "brightness", 7),
	             db_get_int(g_dev, "wifi", 0) ? 1 : 0,
	             db_get_int(g_dev, "bluetooth", 0) ? 1 : 0,
	             strchr(tz, '\'') ? "UTC" : tz);
	if (n < 0 || n >= (int)sizeof body) return false;

	/* NOTHING TO DO IF NOTHING CHANGED, and this is not an optimization for
	 * its own sake. levels_save calls this on every nudge of the volume
	 * rocker, including while a game is running - and volume is not one of the
	 * four values here. Writing anyway put a file replacement back on the path
	 * the database was meant to take it off: measured at 3.03 ms median and
	 * 9.27 ms at the tail, against a 16.7 ms frame. */
	/* Unless it is not there. The memo is about avoiding a rewrite, not about
	 * refusing to create the file: this thing's whole license to be a cache is
	 * that deleting it costs nothing, and skipping here would make a deleted
	 * boot.env stay deleted until some unrelated setting moved. */
	{
		struct stat sb;
		if (!strcmp(body, last) && stat(g_boot_env, &sb) == 0) return true;
	}

	/* Written to a temporary and renamed, but NOT fsynced. rename is what
	 * makes it atomic, so launch.sh can never source half a file; the fsync
	 * would only add durability, and this file is derived - losing it to a
	 * power cut costs one boot at the shipped defaults and the next settings
	 * change rewrites it. That is the whole reason it is allowed to be a
	 * cache. */
	snprintf(tmp, sizeof tmp, "%s.new", g_boot_env);
	if (!(f = fopen(tmp, "w"))) return false;
	if (fputs(body, f) == EOF) { fclose(f); unlink(tmp); return false; }
	if (fclose(f) != 0) { unlink(tmp); return false; }
	if (rename(tmp, g_boot_env) != 0) { unlink(tmp); return false; }

	snprintf(last, sizeof last, "%s", body);
	return true;
}
