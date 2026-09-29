/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* See stats.h. No SDL and no platform header, so the check can drive it
 * without a window - `now_ms` comes from the caller for the same reason
 * idle_check takes one.
 *
 * A row is "sess.<start>.<tag>\t<file>" -> "<seconds>\t<state>", where state
 * is "open" while the game is running and the EXIT reason afterwards. The tab
 * is the separator for the reason favorites use one: a ROM filename may
 * legally contain almost anything except a tab or a newline.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "db.h"
#include "stats.h"

/* --- the session in progress -------------------------------------------- */
static char     g_tag[STATS_TAG_MAX];
static char     g_file[STATS_FILE_MAX];
static long     g_start;          /* unix time, the key's middle segment */
static unsigned g_t0;             /* now_ms at RUN, for the elapsed seconds */
static unsigned g_written;        /* now_ms of the last row written, 0 = none */
static unsigned g_slept;          /* ms of this session spent asleep */
static bool     g_running;

/* The millisecond clock is in the key, not for ordering but for UNIQUENESS.
 * Two launches of the same game inside one wall-clock second would otherwise
 * share a key and the second would overwrite the first - which is what the
 * check found, by launching Contra twice in a row with no sleep between.
 *
 * It has to be something already in hand, because stats_begin does no I/O:
 * asking the database whether a key is taken would put a read on the launch
 * path, which is the one thing this design refuses. g_t0 is monotonic and
 * costs nothing. */
static void session_key(char *out, size_t n, long start, unsigned t0,
                        const char *tag, const char *file)
{
	snprintf(out, n, "sess.%ld.%u.%.*s\t%.*s", start, t0,
	         STATS_TAG_MAX - 1, tag, STATS_FILE_MAX - 1, file);
}

/* Elapsed in whole seconds. unsigned arithmetic, so a wrap of the millisecond
 * clock subtracts correctly rather than producing a session of 49 days. */
static long elapsed_s(unsigned now_ms)
{
	return (long)((now_ms - g_t0 - g_slept) / 1000u);
}

static void write_row(unsigned now_ms, const char *state)
{
	char key[STATS_TAG_MAX + STATS_FILE_MAX + 56], val[64];

	session_key(key, sizeof key, g_start, g_t0, g_tag, g_file);
	snprintf(val, sizeof val, "%ld\t%s", elapsed_s(now_ms), state);
	db_set_str(db_lib(), key, val);
	g_written = now_ms;
}

void stats_begin(const char *tag, const char *file, unsigned now_ms)
{
	g_running = false;
	if (!tag || !file || !*tag || !*file) return;
	if (strlen(tag) >= STATS_TAG_MAX || strlen(file) >= STATS_FILE_MAX) return;

	/* Everything here is memory. Nothing is opened, written or synced - the
	 * whole point of the design is that a launch costs nothing. */
	snprintf(g_tag, sizeof g_tag, "%s", tag);
	snprintf(g_file, sizeof g_file, "%s", file);
	g_start   = (long)time(NULL);
	g_t0      = now_ms;
	g_written = 0;
	g_slept   = 0;
	g_running = true;
}

void stats_asleep(unsigned ms)
{
	if (g_running) g_slept += ms;
}

void stats_tick(unsigned now_ms)
{
	if (!g_running) return;

	/* Nothing at all until the session is worth recording, so backing
	 * straight out of a game leaves no row behind. */
	if (!g_written) {
		if (now_ms - g_t0 >= STATS_MARK_MS) write_row(now_ms, "open");
		return;
	}
	if (now_ms - g_written >= STATS_CKPT_MS) write_row(now_ms, "open");
}

void stats_end(const char *reason, unsigned now_ms)
{
	if (!g_running) return;
	g_running = false;

	/* A session too short to have written a marker still gets its row here,
	 * because by now we know it ended and how long it was. The marker exists
	 * for sessions that never reach this point, not for this one. */
	write_row(now_ms, reason && *reason ? reason : "quit");
}

/* --- recovery ------------------------------------------------------------ */

struct open_row { char key[STATS_TAG_MAX + STATS_FILE_MAX + 40];
                  char val[64]; struct open_row *next; };

static bool collect_open(const char *key, const char *value, void *ctx)
{
	struct open_row **head = ctx, *r;
	const char *tab = strchr(value, '\t');

	if (!tab || strcmp(tab + 1, "open") != 0) return true;
	if (!(r = malloc(sizeof *r))) return false;
	snprintf(r->key, sizeof r->key, "%s", key);
	snprintf(r->val, sizeof r->val, "%.*s\tlost", (int)(tab - value), value);
	r->next = *head;
	*head = r;
	return true;
}

void stats_recover(void)
{
	struct open_row *open = NULL, *r;

	/* Collected before anything is written. Rewriting rows while enumerating
	 * the same prefix is asking a question and changing the answer at once -
	 * the same care fav_save needs for its deletions. */
	db_each_prefix(db_lib(), "sess.", collect_open, &open);
	while ((r = open)) {
		open = r->next;
		/* The seconds stay as they were: the last checkpoint is the most that
		 * can honestly be claimed for a session nobody saw end. Marked so it
		 * is not mistaken for a clean quit later. */
		db_set_str(db_lib(), r->key, r->val);
		free(r);
	}
}

/* --- summarizing --------------------------------------------------------- */

typedef struct {
	char tag[STATS_TAG_MAX];
	char file[STATS_FILE_MAX];
	long seconds;
	int  launches;
	long longest;      /* the longest single session folded into this row */
	long last;         /* unix time that session started */
	int  lost;         /* of those launches, how many never reached EXIT */
} stats_row;

static stats_row g_rows[STATS_MAX];
static int  g_nrows;
static long g_total;
static int  g_launches;
static int  g_lost;

/* What one summarizing pass is filtering and grouping by. Passed through
 * db_each_prefix's ctx rather than kept in a global, so the fold has no state
 * of its own to get out of step with the call that started it. */
typedef struct {
	long cutoff;       /* sessions that started before this are not counted */
	bool by_system;
} fold_ctx;

static int find_row(const char *tag, const char *file)
{
	int i;
	for (i = 0; i < g_nrows; i++)
		if (!strcmp(g_rows[i].tag, tag) && !strcmp(g_rows[i].file, file))
			return i;
	return -1;
}

const char *stats_window_name(stats_window w)
{
	switch (w) {
	case STATS_YEAR:  return "This Year";
	case STATS_MONTH: return "This Month";
	case STATS_WEEK:  return "This Week";
	case STATS_TODAY: return "Today";
	default:          return "All Time";
	}
}

/* Midnight local, then back to the start of the week, month or year.
 *
 * mktime is what makes this correct rather than arithmetic on seconds: a day
 * is not always 86400 seconds where daylight saving exists, and a month is
 * never a fixed number of them. Filling in a struct tm and asking mktime what
 * that instant was is the only way to get "the first of this month" right
 * without owning a calendar. */
long stats_window_start(stats_window w, long now)
{
	time_t t = (time_t)now;
	struct tm tm;

	if (w == STATS_ALL) return 0;
	tm = *localtime(&t);
	tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
	tm.tm_isdst = -1;              /* let mktime decide; -1, never 0 */
	switch (w) {
	case STATS_YEAR:  tm.tm_mon = 0;  /* fall through */
	case STATS_MONTH: tm.tm_mday = 1; break;
	/* Weeks start Sunday here because tm_wday does. Nothing in the UI names
	 * the first day, so this is a convention, not a claim. */
	case STATS_WEEK:  tm.tm_mday -= tm.tm_wday; break;
	default: break;
	}
	return (long)mktime(&tm);
}

static bool fold(const char *key, const char *value, void *ctx)
{
	/* sess.<start>.<t0>.<tag>\t<file>. The first segment was skipped along
	 * with the second until the windows needed it: it is when the session
	 * began, which is what "this week" is asked against. */
	const char *rest = key + strlen("sess.");
	const char *dot = strchr(rest, '.');
	const char *tab, *sep = strchr(value, '\t');
	const fold_ctx *f = ctx;
	char tag[STATS_TAG_MAX];
	long secs, start;
	int i;

	if (!dot || !sep) return true;
	start = strtol(rest, NULL, 10);
	if (!(dot = strchr(dot + 1, '.'))) return true;
	if (!(tab = strchr(dot + 1, '\t'))) return true;
	if ((size_t)(tab - dot - 1) >= STATS_TAG_MAX) return true;
	secs = strtol(value, NULL, 10);
	if (secs < 0) return true;
	if (start < f->cutoff) return true;      /* outside the window */

	snprintf(tag, sizeof tag, "%.*s", (int)(tab - dot - 1), dot + 1);
	/* By system, every session on a machine lands in one row, and the file
	 * is empty because the row is not about a file. */
	i = find_row(tag, f->by_system ? "" : tab + 1);
	if (i < 0) {
		if (g_nrows >= STATS_MAX) return false;
		i = g_nrows++;
		snprintf(g_rows[i].tag, STATS_TAG_MAX, "%s", tag);
		snprintf(g_rows[i].file, STATS_FILE_MAX, "%s",
		         f->by_system ? "" : tab + 1);
		g_rows[i].seconds = 0;
		g_rows[i].launches = 0;
		g_rows[i].longest = 0;
		g_rows[i].last = 0;
		g_rows[i].lost = 0;
	}
	g_rows[i].seconds += secs;
	g_rows[i].launches++;
	if (secs > g_rows[i].longest) g_rows[i].longest = secs;
	if (start > g_rows[i].last)   g_rows[i].last = start;
	/* "open" counts as lost too. A row still marked open during a summary is
	 * a session this boot has not recovered, which is the same event. */
	if (strcmp(sep + 1, "quit") != 0) { g_rows[i].lost++; g_lost++; }
	g_total += secs;
	g_launches++;
	return true;
}

static int by_seconds(const void *a, const void *b)
{
	const stats_row *x = a, *y = b;
	if (x->seconds != y->seconds) return x->seconds < y->seconds ? 1 : -1;
	return strcmp(x->file, y->file);      /* stable enough to read twice */
}

int stats_summarize(stats_window w, bool by_system, long now)
{
	fold_ctx f;

	f.cutoff = stats_window_start(w, now);
	f.by_system = by_system;
	g_nrows = 0;
	g_total = 0;
	g_launches = 0;
	g_lost = 0;
	db_each_prefix(db_lib(), "sess.", fold, &f);
	qsort(g_rows, g_nrows, sizeof g_rows[0], by_seconds);
	return g_nrows;
}

bool stats_at(int i, const char **tag, const char **file,
              long *seconds, int *launches)
{
	if (i < 0 || i >= g_nrows) return false;
	if (tag)      *tag      = g_rows[i].tag;
	if (file)     *file     = g_rows[i].file;
	if (seconds)  *seconds  = g_rows[i].seconds;
	if (launches) *launches = g_rows[i].launches;
	return true;
}

bool stats_lookup(const char *tag, const char *file,
                  long *seconds, long *last)
{
	int i;

	if (!tag || !file) return false;
	i = find_row(tag, file);
	if (i < 0) return false;
	if (seconds) *seconds = g_rows[i].seconds;
	if (last)    *last    = g_rows[i].last;
	return true;
}

bool stats_extra(int i, long *longest, long *last, int *lost)
{
	if (i < 0 || i >= g_nrows) return false;
	if (longest) *longest = g_rows[i].longest;
	if (last)    *last    = g_rows[i].last;
	if (lost)    *lost    = g_rows[i].lost;
	return true;
}

long stats_total_seconds(void) { return g_total; }
int  stats_total_launches(void) { return g_launches; }
int  stats_total_lost(void) { return g_lost; }

void stats_format(long seconds, char *out, size_t n)
{
	if (!out || !n) return;
	if (seconds <= 0)      snprintf(out, n, "never");
	else if (seconds < 60) snprintf(out, n, "%lds", seconds);
	else if (seconds < 3600)
		snprintf(out, n, "%ldm %lds", seconds / 60, seconds % 60);
	else
		snprintf(out, n, "%ldh %ldm", seconds / 3600, (seconds % 3600) / 60);
}

/* Days apart on the CALENDAR, not elapsed hours divided by 24: something
 * played at 11pm was played yesterday when read at 1am, and "2h ago" would be
 * a true sentence that answers the wrong question. */
void stats_ago(long then, long now, char *out, size_t n)
{
	time_t a = (time_t)then, b = (time_t)now;
	struct tm ta, tb;
	long days;

	if (!out || !n) return;
	if (then <= 0) { snprintf(out, n, "never"); return; }
	ta = *localtime(&a);
	tb = *localtime(&b);
	ta.tm_hour = ta.tm_min = ta.tm_sec = 0; ta.tm_isdst = -1;
	tb.tm_hour = tb.tm_min = tb.tm_sec = 0; tb.tm_isdst = -1;
	days = ((long)mktime(&tb) - (long)mktime(&ta) + 43200) / 86400;

	if (days <= 0)     snprintf(out, n, "today");
	else if (days == 1) snprintf(out, n, "yesterday");
	else if (days < 7)  snprintf(out, n, "%ldd ago", days);
	else if (days < 60) snprintf(out, n, "%ldw ago", days / 7);
	else                snprintf(out, n, "%ldmo ago", days / 30);
}
