/* SPDX-License-Identifier: MIT */
/* What a session record actually costs to commit.
 *
 * The game-stats work needs one durable write per game exit, and the argument
 * for how to store it rested on counting fsyncs rather than measuring them.
 * This measures them. Two candidate backends, on whichever filesystem the path
 * argument lands in:
 *
 *   append   open(O_APPEND), write 64 bytes, fsync, close. One fsync.
 *   sqlite   INSERT through a prepared statement, in DELETE and WAL journal
 *            modes. DELETE is the default and costs journal-create, page
 *            write, db write and journal-delete, each with its own barrier.
 *
 * Plus two controls, because a difference is only meaningful against them:
 *
 *   fsync    one fsync on an already-open fd, nothing else. The floor.
 *   nosync   the same write with no fsync at all. What durability costs.
 *
 * And the read path, which is the half the append design is supposed to lose:
 * folding every record versus asking SQL to group them.
 *
 * libsqlite3 is on the device already (3.12.2, /usr/lib), so this dlopens it
 * rather than linking, the same way the host reaches zlib. Nothing here needs
 * sqlite3.h - the ABI is stable and the eight entry points are declared below.
 *
 *   storeprobe <dir> [iterations]
 *   storeprobe <dir> exposure <sysfs stat path>
 *
 * The second mode answers a different question: after a write that does NOT
 * fsync, how long until the bytes are actually on the card? That window is
 * what a power cut can take. It polls the block device's written-sector
 * counter, so it measures the card rather than the page cache.
 *
 * It writes only inside <dir>/storeprobe.tmp and removes it on the way out.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

/* --- the eight sqlite entry points this needs, declared rather than included */
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
static int  (*sq_open)(const char *, sqlite3 **);
static int  (*sq_close)(sqlite3 *);
static int  (*sq_exec)(sqlite3 *, const char *, void *, void *, char **);
static int  (*sq_prepare)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
static int  (*sq_step)(sqlite3_stmt *);
static int  (*sq_reset)(sqlite3_stmt *);
static int  (*sq_finalize)(sqlite3_stmt *);
static int  (*sq_bind_int64)(sqlite3_stmt *, int, long long);
static const char *(*sq_errmsg)(sqlite3 *);
#define SQ_ROW  100
#define SQ_DONE 101

static int sqlite_load(void)
{
	void *h = dlopen("libsqlite3.so.0", RTLD_NOW);
	if (!h) h = dlopen("libsqlite3.so", RTLD_NOW);
	if (!h) h = dlopen("libsqlite3.dylib", RTLD_NOW);   /* so the host can smoke-test it */
	if (!h) return 0;
	sq_open       = dlsym(h, "sqlite3_open");
	sq_close      = dlsym(h, "sqlite3_close");
	sq_exec       = dlsym(h, "sqlite3_exec");
	sq_prepare    = dlsym(h, "sqlite3_prepare_v2");
	sq_step       = dlsym(h, "sqlite3_step");
	sq_reset      = dlsym(h, "sqlite3_reset");
	sq_finalize   = dlsym(h, "sqlite3_finalize");
	sq_bind_int64 = dlsym(h, "sqlite3_bind_int64");
	sq_errmsg     = dlsym(h, "sqlite3_errmsg");
	return sq_open && sq_close && sq_exec && sq_prepare && sq_step &&
	       sq_reset && sq_finalize && sq_bind_int64;
}

/* --- timing ------------------------------------------------------------- */
static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static int cmp_d(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

/* Median and p90 rather than a mean. One 300 ms stall in a hundred writes is
 * the thing a mean hides and the thing a player feels. */
static void report(const char *name, double *v, int n, const char *note)
{
	qsort(v, n, sizeof *v, cmp_d);
	double sum = 0;
	for (int i = 0; i < n; i++) sum += v[i];
	printf("  %-22s %8.3f %8.3f %8.3f %8.3f   %7.1f  %s\n",
	       name, v[0], v[n / 2], v[(int)(n * 0.9)], v[n - 1], sum, note ? note : "");
	fflush(stdout);
}

/* A session record: game id, start, seconds, exit reason. 64 bytes is the
 * shape the stats log would actually write, so the probe writes that. */
static int record(char *buf, int i)
{
	return snprintf(buf, 96, "%08x %ld %6d %-8s\n",
	                (unsigned)(i * 2654435761u), (long)1757000000 + i * 900,
	                60 + i % 3600, i % 7 ? "quit" : "crash");
}

/* Sectors written to the block device, from /sys/block/<dev>/stat field 7. */
static long long sectors_written(const char *statpath)
{
	FILE *f = fopen(statpath, "r");
	if (!f) return -1;
	long long v[8] = { 0 };
	int n = fscanf(f, "%lld %lld %lld %lld %lld %lld %lld %lld",
	               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
	fclose(f);
	return n >= 7 ? v[6] : -1;
}

/* Wait for the counter to move, or give up. Returns seconds, or -1. */
static double wait_for_flush(const char *statpath, long long base, double limit_s)
{
	double t0 = now_ms();
	for (;;) {
		double el = (now_ms() - t0) / 1000.0;
		long long now = sectors_written(statpath);
		if (now > base) return el;
		if (el > limit_s) return -1;
		usleep(200000);
	}
}

static void exposure(const char *dir, const char *statpath)
{
	char path[640];
	long long base;
	double t;

	printf("\n  how long until an unsynced write reaches the card\n");
	printf("  (polling %s, 0.2s resolution)\n\n", statpath);

	/* Settle first, so nothing older is still in flight and confusing this. */
	snprintf(path, sizeof path, "%s/exp.log", dir);
	FILE *f = fopen(path, "w"); fputs("seed\n", f); fclose(f);
	sync(); sleep(3);

	{	/* Append, no fsync - the candidate design. */
		base = sectors_written(statpath);
		int fd = open(path, O_WRONLY | O_APPEND);
		char rec[96]; int len = record(rec, 1);
		if (write(fd, rec, len) < 0) perror("write");
		close(fd);
		t = wait_for_flush(statpath, base, 45);
		if (t < 0) printf("  %-26s no write in 45s\n", "append, no fsync");
		else       printf("  %-26s %5.1f s\n", "append, no fsync", t);
	}

	{	/* Append with fsync - expected to be durable before write() returns. */
		sync(); sleep(3);
		base = sectors_written(statpath);
		int fd = open(path, O_WRONLY | O_APPEND);
		char rec[96]; int len = record(rec, 2);
		if (write(fd, rec, len) < 0) perror("write");
		fsync(fd);
		double after = (sectors_written(statpath) > base) ? 0.0 : -1.0;
		close(fd);
		if (after == 0.0) printf("  %-26s %5.1f s  (before fsync returned)\n",
		                         "append + fsync", 0.0);
		else printf("  %-26s %5.1f s\n", "append + fsync",
		            wait_for_flush(statpath, base, 45));
	}

	if (!sq_open && !sqlite_load()) { printf("  (no libsqlite3, sqlite rows skipped)\n"); return; }

	const char *modes[3]   = { "DELETE", "WAL",  "WAL" };
	const char *sync_of[3] = { "FULL",   "FULL", "NORMAL" };
	for (int m = 0; m < 3; m++) {
		sqlite3 *db = NULL; sqlite3_stmt *st = NULL; char *err = NULL, sql[128];
		snprintf(path, sizeof path, "%s/exp-%s-%s.db", dir, modes[m], sync_of[m]);
		unlink(path);
		if (sq_open(path, &db) != 0) continue;
		snprintf(sql, sizeof sql, "PRAGMA journal_mode=%s;", modes[m]);
		sq_exec(db, sql, NULL, NULL, &err);
		snprintf(sql, sizeof sql, "PRAGMA synchronous=%s;", sync_of[m]);
		sq_exec(db, sql, NULL, NULL, &err);
		sq_exec(db, "CREATE TABLE s(game INTEGER, started INTEGER, secs INTEGER, reason INTEGER);",
		        NULL, NULL, &err);
		sq_prepare(db, "INSERT INTO s VALUES(?,?,?,?);", -1, &st, NULL);

		/* Settle the table creation before timing the insert. */
		sync(); sleep(3);
		base = sectors_written(statpath);
		sq_bind_int64(st, 1, 305419896); sq_bind_int64(st, 2, 1757000000);
		sq_bind_int64(st, 3, 1234);      sq_bind_int64(st, 4, 1);
		sq_step(st); sq_reset(st);
		int immediate = sectors_written(statpath) > base;
		char label[40];
		snprintf(label, sizeof label, "sqlite %s/%s", modes[m], sync_of[m]);
		if (immediate) printf("  %-26s %5.1f s  (before the insert returned)\n", label, 0.0);
		else {
			t = wait_for_flush(statpath, base, 45);
			if (t < 0) printf("  %-26s no write in 45s  <- until checkpoint or close\n", label);
			else       printf("  %-26s %5.1f s\n", label, t);
		}
		sq_finalize(st);
		sq_close(db);
	}
}

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: storeprobe <dir> [iterations]\n"
	                                "       storeprobe <dir> exposure <sysfs stat path>\n"); return 2; }
	int n = argc > 2 ? atoi(argv[2]) : 100;
	if (n < 5) n = 5;

	char dir[512], path[640], sql[256];
	snprintf(dir, sizeof dir, "%s/storeprobe.tmp", argv[1]);
	if (mkdir(dir, 0755) < 0 && access(dir, W_OK) < 0) {
		fprintf(stderr, "storeprobe: cannot write in %s\n", argv[1]);
		return 1;
	}

	if (argc > 3 && !strcmp(argv[2], "exposure")) {
		if (!sqlite_load()) printf("  (no libsqlite3 found)\n");
		exposure(dir, argv[3]);
		char cmd[640];
		snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
		if (system(cmd) != 0) fprintf(stderr, "storeprobe: could not remove %s\n", dir);
		return 0;
	}

	double *v = malloc(n * sizeof *v);
	char rec[96];
	int len = record(rec, 0);

	printf("\n%s  (%d iterations, %d-byte records)\n", argv[1], n, len);
	printf("  %-22s %8s %8s %8s %8s   %7s\n",
	       "", "min", "median", "p90", "max", "total");

	/* --- control: the same write with no durability at all -------------- */
	snprintf(path, sizeof path, "%s/nosync.log", dir);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
	for (int i = 0; i < n; i++) {
		len = record(rec, i);
		double t0 = now_ms();
		if (write(fd, rec, len) < 0) { perror("write"); return 1; }
		v[i] = now_ms() - t0;
	}
	close(fd);
	report("write, no fsync", v, n, "control: what durability is bought against");

	/* --- control: the fsync floor, fd already open ---------------------- */
	snprintf(path, sizeof path, "%s/floor.log", dir);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
	for (int i = 0; i < n; i++) {
		len = record(rec, i);
		double t0 = now_ms();
		if (write(fd, rec, len) < 0) { perror("write"); return 1; }
		fsync(fd);
		v[i] = now_ms() - t0;
	}
	close(fd);
	report("write + fsync", v, n, "the floor any durable design pays");

	/* --- candidate: the append log, opened and closed per record -------- */
	snprintf(path, sizeof path, "%s/append.log", dir);
	unlink(path);
	for (int i = 0; i < n; i++) {
		len = record(rec, i);
		double t0 = now_ms();
		int f = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (f < 0) { perror("open"); return 1; }
		if (write(f, rec, len) < 0) { perror("write"); return 1; }
		fsync(f);
		close(f);
		v[i] = now_ms() - t0;
	}
	report("APPEND LOG", v, n, "open, write, fsync, close");

	/* --- how TortOS writes a config file today ---------------------------
	 * atomic_open/atomic_commit: a .new beside the target, fsync, rename.
	 * levels.cfg goes through this on EVERY volume and brightness nudge, and
	 * the rocker produces a stream of them while a game is running. This row
	 * is the baseline any change to config storage has to beat. */
	snprintf(path, sizeof path, "%s/levels.cfg", dir);
	{
		char tmp[700];
		snprintf(tmp, sizeof tmp, "%s.new", path);
		for (int i = 0; i < n; i++) {
			double t0 = now_ms();
			FILE *f = fopen(tmp, "w");
			if (!f) { perror("fopen"); return 1; }
			fprintf(f, "volume=%d\nbrightness=%d\n", i % 40, i % 10);
			fflush(f);
			fsync(fileno(f));
			fclose(f);
			if (rename(tmp, path) != 0) { perror("rename"); return 1; }
			v[i] = now_ms() - t0;
		}
		report("atomic cfg replace", v, n, "what a volume nudge costs today");
	}

	/* --- candidate: sqlite, both journal modes --------------------------- */
	if (!sqlite_load()) {
		printf("  (no libsqlite3 here, so the sqlite rows are skipped)\n");
	} else {
		/* Three modes, and the third is the one that makes the comparison
		 * honest. WAL with synchronous=NORMAL does not fsync on commit at
		 * all - it defers to a checkpoint - so a power cut loses the last
		 * transactions. That is a real option, but it is not the same
		 * promise the append log makes, and putting it beside DELETE
		 * without saying so would be comparing durability against speed
		 * and calling one of them faster. WAL/FULL is the like-for-like. */
		const char *modes[3]   = { "DELETE", "WAL", "WAL" };
		const char *sync_of[3] = { "FULL",   "FULL", "NORMAL" };
		for (int m = 0; m < 3; m++) {
			sqlite3 *db = NULL;
			sqlite3_stmt *st = NULL;
			char *err = NULL;
			snprintf(path, sizeof path, "%s/stats-%s-%s.db", dir, modes[m], sync_of[m]);
			unlink(path);
			if (sq_open(path, &db) != 0) { printf("  sqlite open failed\n"); break; }
			snprintf(sql, sizeof sql, "PRAGMA journal_mode=%s;", modes[m]);
			sq_exec(db, sql, NULL, NULL, &err);
			snprintf(sql, sizeof sql, "PRAGMA synchronous=%s;", sync_of[m]);
			sq_exec(db, sql, NULL, NULL, &err);
			sq_exec(db, "CREATE TABLE s(game INTEGER, started INTEGER, "
			            "secs INTEGER, reason INTEGER);", NULL, NULL, &err);
			if (sq_prepare(db, "INSERT INTO s VALUES(?,?,?,?);", -1, &st, NULL) != 0) {
				printf("  sqlite prepare failed: %s\n", sq_errmsg ? sq_errmsg(db) : "?");
				sq_close(db); break;
			}
			for (int i = 0; i < n; i++) {
				double t0 = now_ms();
				sq_bind_int64(st, 1, (i * 2654435761u) & 0xffffffff);
				sq_bind_int64(st, 2, 1757000000 + i * 900);
				sq_bind_int64(st, 3, 60 + i % 3600);
				sq_bind_int64(st, 4, i % 7 ? 1 : 2);
				if (sq_step(st) != SQ_DONE) { printf("  insert failed\n"); break; }
				sq_reset(st);
				v[i] = now_ms() - t0;
			}
			sq_finalize(st);
			sq_close(db);
			char label[40];
			snprintf(label, sizeof label, "SQLITE %s/%s", modes[m], sync_of[m]);
			snprintf(sql, sizeof sql, "one INSERT, synchronous=%s", sync_of[m]);
			report(label, v, n, sql);
		}
	}

	/* --- the read path, on a realistic year of sessions ------------------ */
	int many = 4000;
	printf("\n  reading back %d sessions:\n", many);

	snprintf(path, sizeof path, "%s/fold.log", dir);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	for (int i = 0; i < many; i++) { len = record(rec, i); if (write(fd, rec, len) < 0) break; }
	fsync(fd); close(fd);

	{	/* Fold: read it all, sum seconds per game into a small open hash. */
		double t0 = now_ms();
		FILE *f = fopen(path, "rb");
		static unsigned key[8192]; static long tot[8192];
		memset(key, 0, sizeof key); memset(tot, 0, sizeof tot);
		char line[128]; long games = 0;
		while (fgets(line, sizeof line, f)) {
			unsigned g; long st_, se;
			if (sscanf(line, "%x %ld %ld", &g, &st_, &se) != 3) continue;
			unsigned h = g & 8191;
			while (key[h] && key[h] != g) h = (h + 1) & 8191;
			if (!key[h]) { key[h] = g; games++; }
			tot[h] += se;
		}
		fclose(f);
		double ms = now_ms() - t0;
		printf("  %-22s %8.3f ms   %ld distinct games\n", "fold the log", ms, games);
	}

	if (sq_open) {
		sqlite3 *db = NULL; sqlite3_stmt *st = NULL; char *err = NULL;
		snprintf(path, sizeof path, "%s/read.db", dir);
		unlink(path);
		sq_open(path, &db);
		sq_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, &err);
		sq_exec(db, "PRAGMA synchronous=NORMAL;", NULL, NULL, &err);
		sq_exec(db, "CREATE TABLE s(game INTEGER, started INTEGER, secs INTEGER, reason INTEGER);",
		        NULL, NULL, &err);
		sq_exec(db, "BEGIN;", NULL, NULL, &err);
		sq_prepare(db, "INSERT INTO s VALUES(?,?,?,?);", -1, &st, NULL);
		for (int i = 0; i < many; i++) {
			sq_bind_int64(st, 1, (i * 2654435761u) & 0xffffffff);
			sq_bind_int64(st, 2, 1757000000 + i * 900);
			sq_bind_int64(st, 3, 60 + i % 3600);
			sq_bind_int64(st, 4, i % 7 ? 1 : 2);
			sq_step(st); sq_reset(st);
		}
		sq_finalize(st);
		sq_exec(db, "COMMIT;", NULL, NULL, &err);
		sq_close(db);

		/* Reopened cold, because the launcher would not hold it open. */
		double t0 = now_ms();
		sq_open(path, &db);
		sq_prepare(db, "SELECT game, SUM(secs) FROM s GROUP BY game;", -1, &st, NULL);
		long games = 0;
		while (sq_step(st) == SQ_ROW) games++;
		sq_finalize(st);
		sq_close(db);
		double ms = now_ms() - t0;
		printf("  %-22s %8.3f ms   %ld distinct games\n", "sqlite GROUP BY", ms, games);
	}

	/* Sizes matter for a card and for what has to be read at startup. */
	printf("\n  on disk:\n");
	const char *names[] = { "fold.log", "levels.cfg", "read.db", "stats-DELETE-FULL.db",
	                        "stats-WAL-FULL.db", "stats-WAL-NORMAL.db" };
	for (unsigned i = 0; i < sizeof names / sizeof *names; i++) {
		struct stat sb;
		snprintf(path, sizeof path, "%s/%s", dir, names[i]);
		if (stat(path, &sb) == 0)
			printf("  %-22s %8lld bytes\n", names[i], (long long)sb.st_size);
	}

	/* Leave nothing behind. */
	char cmd[640];
	snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
	if (system(cmd) != 0) fprintf(stderr, "storeprobe: could not remove %s\n", dir);
	free(v);
	return 0;
}
