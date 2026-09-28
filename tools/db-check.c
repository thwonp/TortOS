/* Does the settings store keep its promises?
 *
 * Fourteen config files became two databases, and the properties that used to
 * be obvious from looking at a text file now have to be asserted. Chiefly:
 *
 *   - a shipped default never overwrites a choice the player made. That is not
 *     hypothetical - tortos.cfg was applied over the top of levels.cfg at every
 *     boot until 2026-08, which put the config ahead of the player.
 *   - a missing or unreadable database comes back as a working one, because
 *     the defaults are compiled in and there is no .db in the payload.
 *   - a value that is not a number is not a zero.
 *   - the two scopes stay apart. Flattening them would move a session token
 *     between handhelds, which is the one thing the old layout was careful of.
 *   - the games table exists on the card and nowhere else, and a row of scraped
 *     text comes back exactly as it went in.
 *
 * Links src/db.c and NOT SDL. If it ever needs SDL, the split has failed.
 */
#include "../src/db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;

static void ck(int cond, const char *what)
{
	if (!cond) { printf("  FAIL %s\n", what); fails++; }
}

#define DEV "/tmp/tortos-db-check-device.db"
#define LIB "/tmp/tortos-db-check-library.db"

static void scrub(void)
{
	const char *suffix[] = { "", "-wal", "-shm" };
	char p[256];
	for (unsigned i = 0; i < 3; i++) {
		snprintf(p, sizeof p, "%s%s", DEV, suffix[i]); unlink(p);
		snprintf(p, sizeof p, "%s%s", LIB, suffix[i]); unlink(p);
	}
}

static void opens_and_seeds(void)
{
	db *d;
	char buf[64];

	printf("a fresh database arrives with the shipped defaults:\n");
	d = db_open(DEV, DB_DEVICE);
	ck(d != NULL, "it opens where nothing existed");
	if (!d) return;

	/* A rung, not the percentage config/tortos.cfg shipped: one key, one unit. */
	ck(db_get_int(d, "volume", -1) == 8, "volume seeded as a rung");
	ck(db_get_int(d, "brightness", -1) == 7, "brightness seeded");
	db_get_str(d, "audioout", buf, sizeof buf, "");
	ck(!strcmp(buf, "auto"), "audioout seeded to auto");
	/* NextUI's Screen timeout default, a minute - what auto_off_load()
	 * returns when no file exists. Seeding 0 here would quietly change the
	 * shipped default to "never", which is exactly what it did the first
	 * time. */
	ck(db_get_int(d, "autooff", -1) == 60, "auto sleep seeded to a minute");
	ck(db_get_int(d, "suspendtimeout", -1) == 30, "suspend timeout seeded to 30s");
	ck(db_get_int(d, "keepawakeusb", -1) == 0, "keep awake over USB seeded off");
	db_close(d);
}

static void a_default_never_beats_a_choice(void)
{
	db *d;

	printf("a default never overwrites a choice:\n");
	d = db_open(DEV, DB_DEVICE);
	ck(db_set_int(d, "volume", 12), "the player turns it down");
	db_close(d);

	d = db_open(DEV, DB_DEVICE);              /* seeds again on every open */
	ck(db_get_int(d, "volume", -1) == 12, "and it is still 12 after a reopen");
	ck(db_get_int(d, "brightness", -1) == 7, "while an untouched key keeps its default");
	db_close(d);
}

static void round_trips(void)
{
	db *d = db_open(DEV, DB_DEVICE);
	char buf[64];

	printf("values survive a close and reopen:\n");
	ck(db_set_str(d, "probe.text", "hello world"), "a string with a space writes");
	ck(db_set_int(d, "probe.int", -17), "a negative integer writes");
	db_close(d);

	d = db_open(DEV, DB_DEVICE);
	db_get_str(d, "probe.text", buf, sizeof buf, "");
	ck(!strcmp(buf, "hello world"), "the string comes back whole");
	ck(db_get_int(d, "probe.int", 0) == -17, "so does the integer");

	printf("a value that is not a number is not a zero:\n");
	db_set_str(d, "probe.junk", "seven");
	ck(db_get_int(d, "probe.junk", -99) == -99, "it falls back instead of reading 0");
	db_set_str(d, "probe.trailing", "12x");
	ck(db_get_int(d, "probe.trailing", -99) == -99, "and trailing rubbish is not 12");

	printf("absent keys and deletion:\n");
	ck(!db_has(d, "probe.never"), "a key nobody wrote is absent");
	ck(db_get_int(d, "probe.never", 5) == 5, "and reads as the fallback");
	db_get_str(d, "probe.never", buf, sizeof buf, "spare");
	ck(!strcmp(buf, "spare"), "including for strings");
	ck(db_has(d, "probe.text"), "a key that was written is present");
	ck(db_del(d, "probe.text"), "it deletes");
	ck(!db_has(d, "probe.text"), "and is gone afterwards");
	db_close(d);
}

static void a_lost_database_self_heals(void)
{
	db *d;

	printf("a deleted database comes back working:\n");
	scrub();
	d = db_open(DEV, DB_DEVICE);
	ck(d != NULL, "it reopens from nothing");
	ck(db_get_int(d, "volume", -1) == 8, "with the shipped default, not an empty value");
	ck(!db_has(d, "probe.int"), "and nothing from the old file");
	db_close(d);
}

/* The scraped text: a card's, not a handheld's, and intact on the way back. */
static void the_games_table(void)
{
	db *dev, *lib;
	game_meta m, got;
	size_t i;

	printf("the games table:\n");
	dev = db_open(DEV, DB_DEVICE);
	lib = db_open(LIB, DB_LIBRARY);
	if (!dev || !lib) { ck(0, "both open"); return; }

	memset(&m, 0, sizeof m);
	snprintf(m.year, sizeof m.year, "1993");
	snprintf(m.publisher, sizeof m.publisher, "Sega");
	snprintf(m.developer, sizeof m.developer, "Treasure");
	snprintf(m.players, sizeof m.players, "2");
	snprintf(m.genres, sizeof m.genres, "Action,Shooter");
	snprintf(m.esrb, sizeof m.esrb, "E");
	snprintf(m.note, sizeof m.note, "18");
	/* Right up to the cap, because that is the length nobody tries by hand. */
	for (i = 0; i < sizeof m.synopsis - 1; i++)
		m.synopsis[i] = (char)('a' + (i % 26));

	ck(db_game_set(lib, "Genesis", "Gunstar Heroes (USA).zip", &m),
	   "a row goes into the card's database");
	ck(!db_game_set(dev, "Genesis", "Gunstar Heroes (USA).zip", &m),
	   "and cannot go into the handheld's, which has no such table");

	memset(&got, 0xAB, sizeof got);
	ck(db_game_get(lib, "Genesis", "Gunstar Heroes (USA).zip", &got), "and comes back");
	ck(!strcmp(got.year, "1993") && !strcmp(got.publisher, "Sega") &&
	   !strcmp(got.developer, "Treasure") && !strcmp(got.players, "2") &&
	   !strcmp(got.genres, "Action,Shooter") && !strcmp(got.esrb, "E") &&
	   !strcmp(got.note, "18"), "with every field as written");
	ck(!strcmp(got.synopsis, m.synopsis), "including a synopsis at the cap");

	memset(&got, 0xAB, sizeof got);
	ck(!db_game_get(lib, "Genesis", "a game nobody has.zip", &got),
	   "a game with no row says so");
	ck(got.year[0] == '\0' && got.synopsis[0] == '\0',
	   "and zeroes what it was handed, so a screen can draw it either way");

	/* Scraped twice is one row, not two: the second write replaces. */
	snprintf(m.year, sizeof m.year, "1994");
	ck(db_game_set(lib, "Genesis", "Gunstar Heroes (USA).zip", &m), "a re-scrape writes");
	ck(db_game_get(lib, "Genesis", "Gunstar Heroes (USA).zip", &got) &&
	   !strcmp(got.year, "1994"), "and replaces rather than duplicating");

	db_close(dev);
	db_close(lib);
}

static void the_scopes_stay_apart(void)
{
	db *dev, *lib;
	char buf[64];
	size_t nd, nl, i, j;
	const db_default *dd, *ld;

	printf("the two scopes do not bleed into each other:\n");
	dev = db_open(DEV, DB_DEVICE);
	lib = db_open(LIB, DB_LIBRARY);
	ck(dev && lib, "both open");
	if (!dev || !lib) return;

	db_get_str(lib, "timezone", buf, sizeof buf, "");
	ck(!strcmp(buf, "America/New_York"), "the library scope seeds its own defaults");
	ck(!db_has(dev, "timezone"), "which the device scope does not get");
	ck(!db_has(lib, "volume"), "and the device's do not leak the other way");

	/* A key in both would be ambiguous the first time a card moved between
	 * handhelds, which is exactly the failure the split exists to prevent. */
	dd = db_defaults(DB_DEVICE, &nd);
	ld = db_defaults(DB_LIBRARY, &nl);
	for (i = 0; i < nd; i++)
		for (j = 0; j < nl; j++)
			ck(strcmp(dd[i].key, ld[j].key) != 0,
			   "no key is declared in both scopes");

	/* Writing the same name either side must stay two separate values. */
	db_set_str(dev, "shared.name", "device");
	db_set_str(lib, "shared.name", "library");
	db_get_str(dev, "shared.name", buf, sizeof buf, "");
	ck(!strcmp(buf, "device"), "the same key in both files holds two values");
	db_get_str(lib, "shared.name", buf, sizeof buf, "");
	ck(!strcmp(buf, "library"), "and neither one wins");

	db_close(dev);
	db_close(lib);
}

/* boot.env is the one thing outside this process that depends on the schema,
 * and it is sourced by the script that boots the device. A key it reads from
 * the wrong scope silently exports the fallback forever - which is exactly the
 * bug this caught while it was being written. */
static void boot_env_says_what_the_shell_needs(void)
{
	const char *path = "/tmp/tortos-db-check-boot.env";
	char line[256];
	int seen_bright = 0, seen_wifi = 0, seen_bt = 0, seen_tz = 0;
	FILE *f;

	printf("boot.env carries the five values launch.sh reads:\n");
	scrub();
	unlink(path);
	ck(db_init(DEV, LIB, path), "both databases open");

	/* Values a shell must see, not the defaults, so a wrong scope shows up. */
	db_set_int(db_dev(), "brightness", 3);
	db_set_int(db_dev(), "wifi", 1);
	db_set_int(db_dev(), "bluetooth", 1);
	db_set_str(db_lib(), "timezone", "Europe/Berlin");

	ck(db_write_boot_env(), "it writes");
	f = fopen(path, "r");
	ck(f != NULL, "and the file is there");
	if (!f) { db_shutdown(); return; }
	while (fgets(line, sizeof line, f)) {
		if (!strcmp(line, "BRIGHTNESS='3'\n"))          seen_bright = 1;
		if (!strcmp(line, "WIFI='1'\n"))                seen_wifi = 1;
		if (!strcmp(line, "BLUETOOTH='1'\n"))           seen_bt = 1;
		if (!strcmp(line, "TIMEZONE='Europe/Berlin'\n")) seen_tz = 1;
	}
	fclose(f);
	ck(seen_bright, "brightness is the stored value, not the default");
	ck(seen_wifi,   "wifi is the stored value");
	ck(seen_bt,     "bluetooth is the stored value, from the device scope");
	ck(seen_tz,     "timezone is the stored value, from the library scope");

	/* The format is a contract with two readers written in another language:
	 * launch.sh sources the file, and tortos-bootbright.sh - which runs from
	 * the rootfs before launch.sh - seds one value out of it. A quoting change
	 * here would break the boot, so both are exercised rather than eyeballed. */
	{
		char cmd[512];
		FILE *r;
		char got[128] = { 0 };

		snprintf(cmd, sizeof cmd,
		         "sh -c '. %s; printf \"%%s %%s\" \"$BRIGHTNESS\" \"$TIMEZONE\"'",
		         path);
		r = popen(cmd, "r");
		if (r) { if (!fgets(got, sizeof got, r)) got[0] = '\0'; pclose(r); }
		ck(!strcmp(got, "3 Europe/Berlin"), "a POSIX shell can source it");

		snprintf(cmd, sizeof cmd,
		         "sed -n \"s/^BRIGHTNESS='\\(.*\\)'$/\\1/p\" %s", path);
		got[0] = '\0';
		r = popen(cmd, "r");
		if (r) { if (!fgets(got, sizeof got, r)) got[0] = '\0'; pclose(r); }
		ck(!strcmp(got, "3\n"), "and bootbright's sed extracts the brightness");
	}

	/* Derived, not authoritative: losing it must cost nothing. This is also
	 * the case the unchanged-content memo below could have broken - a deleted
	 * file whose content has not changed still has to come back. */
	unlink(path);
	ck(db_write_boot_env(), "it rewrites after being deleted");
	ck(!access(path, F_OK), "and the file is really there again");

	/* Unchanged content must not rewrite. levels_save calls this on every
	 * nudge of the volume rocker, while a game is running, and volume is not
	 * one of the four values in here. */
	{
		struct stat a, b;
		stat(path, &a);
		sleep(1);                      /* mtime has one-second resolution */
		ck(db_write_boot_env(), "an unchanged write reports success");
		stat(path, &b);
		ck(a.st_mtime == b.st_mtime, "and does not touch the file");

		db_set_int(db_dev(), "brightness", 9);
		ck(db_write_boot_env(), "a changed value writes");
		stat(path, &b);
		ck(a.st_mtime != b.st_mtime, "and does touch it");
	}
	unlink(path);
	db_shutdown();
	ck(db_dev() == NULL && db_lib() == NULL, "shutdown clears both handles");
}

/* Six config files became a namespace rather than a table each, so the prefix
 * scan is what makes them readable at all. The bound matters: "turbo." must
 * not pick up "turbos.x", and an empty middle segment - "coreopt..global" -
 * has to come back like any other. */
static bool collect(const char *key, const char *value, void *ctx)
{
	char *out = ctx;
	strncat(out, key, 200);
	strncat(out, "=", 2);
	strncat(out, value, 200);
	strncat(out, ";", 2);
	return true;
}

static void prefix_scan(void)
{
	db *d;
	char got[512] = { 0 };

	printf("the prefix scan brings back a namespace and nothing else:\n");
	scrub();
	d = db_open(LIB, DB_LIBRARY);
	if (!d) { ck(0, "library opens"); return; }

	/* Its own namespace, not turbo. or coreopt. - those are seeded, and an
	 * assertion written against an empty database would be asserting that the
	 * defaults are absent. That is how this check failed when it was written. */
	db_set_str(d, "zz.NES", "a");
	db_set_str(d, "zz.GB", "b");
	db_set_str(d, "zzs.NOT", "c");        /* one byte past the prefix */
	db_set_str(d, "zz", "d");             /* the prefix minus its dot */
	db_each_prefix(d, "zz.", collect, got);
	ck(!strcmp(got, "zz.GB=b;zz.NES=a;"), "only the namespace, in key order");

	got[0] = '\0';
	db_set_str(d, "yy..global", "g");
	db_set_str(d, "yy.GB.local", "l");
	db_each_prefix(d, "yy.", collect, got);
	ck(!strcmp(got, "yy..global=g;yy.GB.local=l;"),
	   "an empty tag segment scans like any other");

	/* And the real namespaces carry what the defaults declared. */
	got[0] = '\0';
	db_each_prefix(d, "turbo.", collect, got);
	ck(strstr(got, "turbo.NGPC=x:a~3,y:b~3;") != NULL,
	   "the seeded turbo maps are readable through the same scan");

	got[0] = '\0';
	db_each_prefix(d, "nothing.", collect, got);
	ck(!got[0], "an empty namespace yields nothing rather than everything");
	db_close(d);
}

/* A settings failure must not take the launcher down: launch.sh restarts it,
 * and five exits inside five seconds each makes it call poweroff. So every
 * getter has to survive there being no database at all. */
static void no_database_is_survivable(void)
{
	char buf[32];

	printf("with no database open at all:\n");
	db_shutdown();
	ck(db_dev() == NULL && db_lib() == NULL, "the handles are NULL");
	ck(db_get_int(NULL, "volume", 8) == 8, "an integer reads its fallback");
	db_get_str(NULL, "audioout", buf, sizeof buf, "auto");
	ck(!strcmp(buf, "auto"), "a string reads its fallback");
	ck(!db_has(NULL, "volume"), "nothing is present");
	ck(!db_set_int(NULL, "volume", 3), "a write fails rather than crashing");
	ck(!db_write_boot_env(), "and the export declines rather than crashing");
	db_each_prefix(NULL, "turbo.", collect, buf);   /* must simply not run */
}

static void every_default_is_readable(void)
{
	db *d;
	const db_default *def;
	size_t n, i;
	char buf[128];

	printf("every declared default is present and reads back:\n");
	scrub();
	for (int s = 0; s < 2; s++) {
		db_scope sc = s ? DB_LIBRARY : DB_DEVICE;
		d = db_open(s ? LIB : DEV, sc);
		if (!d) { ck(0, "scope opens"); continue; }
		def = db_defaults(sc, &n);
		ck(n > 0, "the scope declares at least one default");
		for (i = 0; i < n; i++) {
			ck(db_has(d, def[i].key), def[i].key);
			db_get_str(d, def[i].key, buf, sizeof buf, "");
			ck(!strcmp(buf, def[i].value), "seeded value matches the table");
		}
		db_close(d);
	}
}

/* --dump must never put a credential on a terminal.
 *
 * It is the repair tool for a device with no sqlite3, so it is run over adb
 * and its output lands in a log, a paste or a screen share. It printed
 * ra.token from the day the account arrived; ss.password would have joined it,
 * and that one is the credential itself rather than something traded for it.
 *
 * Asserted by dumping to a file and searching it for the values, which is the
 * only form of this check that cannot be fooled by the redaction being applied
 * somewhere other than where the printing happens. */
static void a_dump_hides_credentials(void)
{
	static const char *secret = "hunter2-not-a-real-password";
	static const char *plain  = "America/New_York";
	char path[256], buf[8192];
	FILE *f;
	size_t got = 0;

	printf("what --dump puts on a terminal:\n");
	/* Its own database: the case before this one closes them deliberately, and
	 * a dump of nothing would pass every assertion below by being empty. */
	scrub();
	if (!db_init(DEV, LIB, NULL)) { ck(0, "could not open a database to dump"); return; }
	db_set_str(db_dev(), "ra.token", secret);
	db_set_str(db_dev(), "ss.password", secret);
	db_set_str(db_dev(), "ss.user", "someone");
	db_set_str(db_dev(), "timezone", plain);

	snprintf(path, sizeof path, "%s.dump", DEV);
	f = fopen(path, "w+");
	if (!f) { ck(0, "could not write a dump to read back"); return; }
	db_dump(db_dev(), f);
	fflush(f);
	rewind(f);
	got = fread(buf, 1, sizeof buf - 1, f);
	buf[got] = '\0';
	fclose(f);
	unlink(path);

	ck(strstr(buf, secret) == NULL, "no secret value appears anywhere in it");
	ck(strstr(buf, "ra.token") != NULL, "the token's KEY is still listed");
	ck(strstr(buf, "ss.password") != NULL, "and so is the password's");
	ck(strstr(buf, "[hidden,") != NULL, "with a length in place of the value");
	/* A user name is not a secret, and hiding it would make the dump useless
	 * for the question it is usually opened to answer: which account is this. */
	ck(strstr(buf, "someone") != NULL, "an account NAME is still shown");
	ck(strstr(buf, plain) != NULL, "and an ordinary setting is untouched");
}

int main(void)
{
	if (!db_available()) {
		/* Not a pass dressed as a skip: say which, and say it loudly. */
		printf("db-check: no libsqlite3 on this machine, so nothing was checked\n");
		return 77;
	}
	scrub();
	opens_and_seeds();
	a_default_never_beats_a_choice();
	round_trips();
	a_lost_database_self_heals();
	the_games_table();
	the_scopes_stay_apart();
	boot_env_says_what_the_shell_needs();
	prefix_scan();
	no_database_is_survivable();
	every_default_is_readable();
	a_dump_hides_credentials();
	scrub();

	if (fails) { printf("\n%d FAILED\n", fails); return 1; }
	printf("\nok: the settings store keeps its promises\n");
	return 0;
}
