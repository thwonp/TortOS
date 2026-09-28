/* Play time: does it record what happened, and does it stay off the launch?
 *
 * The second half is the one worth a check. "Launch does no I/O" is a claim
 * about a path nobody watches, and the way it stops being true is one
 * reasonable-looking write at a time - so it is asserted here rather than
 * remembered. The rest is arithmetic that a power cut is allowed to interrupt
 * at any point, which is exactly the sort of thing a test can enumerate and a
 * person cannot.
 *
 * Time is passed in, the way idle_check takes it, so a checkpoint interval and
 * a clock wrap can be driven directly instead of waited for.
 *
 * Links src/stats.c and src/db.c and NOT SDL.
 */
#include "../src/db.h"
#include "../src/stats.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fails;
static void ck(int cond, const char *what)
{
	if (!cond) { printf("  FAIL %s\n", what); fails++; }
}

#define DEV "/tmp/tortos-stats-dev.db"
#define LIB "/tmp/tortos-stats-lib.db"

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

static bool count_cb(const char *k, const char *v, void *ctx)
{
	(void)k; (void)v; (*(int *)ctx)++; return true;
}
static int rows(void)
{
	int n = 0;
	db_each_prefix(db_lib(), "sess.", count_cb, &n);
	return n;
}

static bool grab(const char *k, const char *v, void *ctx)
{
	(void)k;
	snprintf(ctx, 64, "%s", v);
	return false;                       /* the first is enough */
}
static const char *only_value(void)
{
	static char v[64];
	v[0] = '\0';
	db_each_prefix(db_lib(), "sess.", grab, v);
	return v;
}

/* THE property. A launch that touches storage is the failure this design
 * exists to prevent, and it would be invisible in every other test here. */
static void launch_writes_nothing(void)
{
	printf("a launch writes nothing at all:\n");
	scrub();
	ck(rows() == 0, "the store starts empty");
	stats_begin("NES", "Contra (USA).zip", 1000);
	ck(rows() == 0, "stats_begin wrote no row");
	stats_tick(1100);
	ck(rows() == 0, "and neither did the first tick");
	stats_tick(1000 + STATS_MARK_MS - 1);
	ck(rows() == 0, "nor a tick just before the marker is due");
}

static void a_short_session_leaves_no_marker(void)
{
	printf("backing straight out of a game:\n");
	scrub();
	stats_begin("NES", "Contra (USA).zip", 1000);
	stats_tick(2000);
	ck(rows() == 0, "nothing written during a two-second visit");
	stats_end(NULL, 3000);
	ck(rows() == 1, "but the session itself is still recorded");
	ck(!strcmp(only_value(), "2\tquit"), "two seconds, quit");
}

static void a_long_session_marks_and_checkpoints(void)
{
	printf("a session long enough to be worth recovering:\n");
	scrub();
	stats_begin("GBA", "Sigma Star Saga.zip", 0);
	stats_tick(STATS_MARK_MS);
	ck(rows() == 1, "the marker lands once the session is worth it");
	ck(!strcmp(only_value(), "5\topen"), "and says it is still open");

	stats_tick(STATS_MARK_MS + 1000);
	ck(!strcmp(only_value(), "5\topen"), "a tick before the interval changes nothing");

	stats_tick(STATS_MARK_MS + STATS_CKPT_MS);
	ck(!strcmp(only_value(), "65\topen"), "the checkpoint moves it forward");
	ck(rows() == 1, "and updates the row rather than adding one");

	stats_end("quit", 120000);
	ck(rows() == 1, "the end still updates the same row");
	ck(!strcmp(only_value(), "120\tquit"), "with the final length and reason");
}

static void a_power_cut_is_recovered_at_the_checkpoint(void)
{
	printf("a session the device never saw end:\n");
	scrub();
	stats_begin("SNES", "Super Metroid.zip", 0);
	stats_tick(STATS_MARK_MS);
	stats_tick(STATS_MARK_MS + STATS_CKPT_MS);      /* 65s on the card */
	/* and then the battery goes. No stats_end, ever. */

	db_shutdown();
	db_init(DEV, LIB, NULL);                        /* next boot */
	ck(!strcmp(only_value(), "65\topen"), "it is still marked open");
	stats_recover();
	ck(!strcmp(only_value(), "65\tlost"),
	   "recovery closes it at the last checkpoint, and says it was lost");
	ck(rows() == 1, "without inventing a second row");

	stats_recover();
	ck(!strcmp(only_value(), "65\tlost"), "and running recovery twice is a no-op");
}

static void the_clock_wrapping_is_not_a_49_day_session(void)
{
	printf("the millisecond clock wrapping:\n");
	scrub();
	/* SDL_GetTicks is 32-bit and wraps after ~49 days of uptime. Signed
	 * arithmetic here would turn a 10-second session into a colossal one. */
	stats_begin("GB", "Link's Awakening.zip", 0xFFFFF000u);
	stats_end("quit", 0xFFFFF000u + 10000u);        /* wraps past zero */
	ck(!strcmp(only_value(), "10\tquit"), "ten seconds, not forty-nine days");
}

static void sleep_is_not_play(void)
{
	printf("time asleep:\n");
	scrub();
	stats_begin("GB", "Tetris.zip", 0);
	stats_asleep(45000);                            /* a light sleep */
	stats_end("quit", 60000);
	ck(!strcmp(only_value(), "15\tquit"), "a minute with 45s asleep is 15s played");
	scrub();
	stats_begin("GB", "Tetris.zip", 100000);
	stats_end("quit", 110000);
	ck(!strcmp(only_value(), "10\tquit"), "the next session does not inherit it");
}

static void two_systems_can_hold_the_same_filename(void)
{
	printf("the same file on two shelves is two games:\n");
	scrub();
	stats_begin("GB", "Tetris.zip", 0);   stats_end("quit", 60000);
	stats_begin("GBC", "Tetris.zip", 0);  stats_end("quit", 30000);
	ck(stats_summarize(STATS_ALL, false, time(NULL)) == 2,
	   "two distinct games");
}

static void summarize_folds_and_ranks(void)
{
	const char *tag, *file;
	long secs;
	int launches;

	printf("summarizing:\n");
	scrub();
	/* Contra twice, Metroid once and longer. The two Contra runs start in
	 * the same wall-clock second on purpose - back to back with no sleep is
	 * exactly how the key collision was found. */
	stats_begin("NES", "Contra.zip", 0);       stats_end("quit", 60000);
	stats_begin("NES", "Contra.zip", 1);       stats_end("quit", 120001);
	stats_begin("SNES", "Metroid.zip", 0);     stats_end("quit", 600000);

	ck(stats_summarize(STATS_ALL, false, time(NULL)) == 2, "two games");
	ck(stats_total_seconds() == 60 + 120 + 600, "total is every session");
	ck(stats_total_launches() == 3, "three launches");

	ck(stats_at(0, &tag, &file, &secs, &launches), "the first row reads");
	ck(!strcmp(file, "Metroid.zip"), "most played first");
	ck(secs == 600 && launches == 1, "with its own total and count");

	ck(stats_at(1, &tag, &file, &secs, &launches), "the second row reads");
	ck(!strcmp(file, "Contra.zip") && secs == 180 && launches == 2,
	   "and the two Contra sessions folded into one row");

	ck(!stats_at(2, NULL, NULL, NULL, NULL), "and there is no third");
}

/* A session at a chosen moment in the past.
 *
 * stats_begin reads the clock itself, and correctly so - a launch does no I/O
 * and has nothing to ask - which leaves a window test no way to place a
 * session anywhere but now. So it writes the row it wants to see. This is the
 * only code outside stats.c that knows the key format, and if the two ever
 * disagree, the tests below stop folding anything and say so loudly. */
static void seed(long start, const char *tag, const char *file,
                 long secs, const char *state)
{
	static unsigned uniq;
	char k[STATS_TAG_MAX + STATS_FILE_MAX + 56], v[64];

	snprintf(k, sizeof k, "sess.%ld.%u.%s\t%s", start, ++uniq, tag, file);
	snprintf(v, sizeof v, "%ld\t%s", secs, state);
	db_set_str(db_lib(), k, v);
}

/* The boundaries are asked of stats_window_start rather than computed here,
 * because a check that recomputes them with the same arithmetic proves only
 * that the arithmetic was copied correctly. What is asserted is the PROPERTY:
 * a second before a boundary is outside the window and a second after is
 * inside it. That holds in any timezone, which a hardcoded date would not. */
static void windows_cut_on_a_boundary(void)
{
	long now = (long)time(NULL);
	int w;

	printf("what each window includes:\n");
	ck(stats_window_start(STATS_ALL, now) == 0, "all time starts at zero");
	ck(stats_window_start(STATS_TODAY, now) >= stats_window_start(STATS_WEEK, now),
	   "today starts no earlier than this week");
	ck(stats_window_start(STATS_WEEK, now) >= stats_window_start(STATS_MONTH, now),
	   "this week starts no earlier than this month");
	ck(stats_window_start(STATS_MONTH, now) >= stats_window_start(STATS_YEAR, now),
	   "this month starts no earlier than this year");
	ck(stats_window_start(STATS_YEAR, now) <= now, "this year has begun");

	for (w = STATS_YEAR; w <= STATS_TODAY; w++) {
		long edge = stats_window_start(w, now);

		scrub();
		seed(edge - 1, "NES", "Before.zip", 100, "quit");
		seed(edge,     "NES", "On.zip",     200, "quit");
		seed(now,      "NES", "After.zip",  300, "quit");
		ck(stats_summarize(w, false, now) == 2,
		   stats_window_name(w));
		ck(stats_total_seconds() == 500,
		   "the session before the boundary is not counted");
		ck(stats_summarize(STATS_ALL, false, now) == 3,
		   "and all time still has all three");
	}
}

static void folding_by_system(void)
{
	const char *tag, *file;
	long secs, now = (long)time(NULL);
	int launches;

	printf("by system rather than by game:\n");
	scrub();
	seed(now, "NES", "Contra.zip",  600, "quit");
	seed(now, "NES", "Metroid.zip", 300, "quit");
	seed(now, "GB",  "Tetris.zip",  100, "quit");

	ck(stats_summarize(STATS_ALL, true, now) == 2, "three games, two systems");
	ck(stats_at(0, &tag, &file, &secs, &launches), "the first row reads");
	ck(!strcmp(tag, "NES"), "the busiest system first");
	ck(secs == 900 && launches == 2, "both its games folded into it");
	ck(!*file, "and a system row names no file");
	ck(stats_summarize(STATS_ALL, false, now) == 3, "by game it is three again");
}

static void the_extras(void)
{
	long now = (long)time(NULL), longest = 0, last = 0;
	int lost = 0;

	printf("longest, last played and lost:\n");
	scrub();
	seed(now - 200000, "NES", "Contra.zip", 900, "quit");
	seed(now - 100000, "NES", "Contra.zip", 300, "lost");
	seed(now -  50000, "NES", "Contra.zip", 120, "quit");

	ck(stats_summarize(STATS_ALL, false, now) == 1, "one game");
	ck(stats_extra(0, &longest, &last, &lost), "its extras read");
	ck(longest == 900, "the longest single session, not the total");
	ck(last == now - 50000, "last played is the most recent start");
	ck(lost == 1 && stats_total_lost() == 1, "one session never reached exit");

	/* A row still marked open is a session this boot has not recovered. It
	 * counts as lost for the same reason stats_recover will call it that. */
	scrub();
	seed(now, "NES", "Contra.zip", 60, "open");
	ck(stats_summarize(STATS_ALL, false, now) == 1, "an open row still folds");
	ck(stats_total_lost() == 1, "and counts as lost");
}

static void how_long_ago(void)
{
	long now = (long)time(NULL);
	char b[16];

	printf("how long ago it reads:\n");
	stats_ago(0, now, b, sizeof b);       ck(!strcmp(b, "never"), "never played");
	stats_ago(now, now, b, sizeof b);     ck(!strcmp(b, "today"), "today");
	stats_ago(now - 86400 * 3, now, b, sizeof b);
	ck(!strcmp(b, "3d ago"), "a few days");
	stats_ago(now - 86400 * 21, now, b, sizeof b);
	ck(!strcmp(b, "3w ago"), "a few weeks");
}

static void formatting(void)
{
	char b[16];
	printf("how a duration reads:\n");
	stats_format(0, b, sizeof b);     ck(!strcmp(b, "never"), "nothing played");
	stats_format(45, b, sizeof b);    ck(!strcmp(b, "45s"), "under a minute");
	stats_format(432, b, sizeof b);   ck(!strcmp(b, "7m 12s"), "minutes and seconds");
	stats_format(45296, b, sizeof b); ck(!strcmp(b, "12h 34m"), "hours and minutes");
}

int main(void)
{
	if (!db_available()) {
		printf("stats-check: no libsqlite3 here, so nothing was checked\n");
		return 77;
	}
	launch_writes_nothing();
	a_short_session_leaves_no_marker();
	a_long_session_marks_and_checkpoints();
	a_power_cut_is_recovered_at_the_checkpoint();
	the_clock_wrapping_is_not_a_49_day_session();
	sleep_is_not_play();
	two_systems_can_hold_the_same_filename();
	summarize_folds_and_ranks();
	windows_cut_on_a_boundary();
	folding_by_system();
	the_extras();
	formatting();
	how_long_ago();
	db_shutdown();
	scrub();
	db_shutdown();

	if (fails) { printf("\n%d FAILED\n", fails); return 1; }
	printf("\nok: play time is recorded, and a launch still writes nothing\n");
	return 0;
}
