/* SPDX-License-Identifier: MIT */
/* How long each game has been played.
 *
 * WALL CLOCK, from RUN to EXIT, and there is nothing to subtract: the device
 * has no suspend and is not getting one (see the Auto Off row in sys_menu.c).
 * Auto Off powers it OFF, so a session left running is bounded by that
 * interval rather than by how long someone was away.
 *
 * THE LAUNCH PATH DOES NO I/O. stats_begin notes a number in memory and
 * nothing else - no file opened, nothing written, nothing synced - because
 * launch speed is the point of this firmware and a millisecond bought here
 * would be the first of several. Everything else happens from the game tick,
 * which already runs at 10 Hz and is cheap by contract, or at exit.
 *
 * NOTHING IS FSYNCED WHILE A GAME RUNS. Measured on the card: an fsync is
 * 1.56 ms median but 16.4 ms at the tail, which is a dropped frame. The writes
 * during play are unsynced, so they cost 0.007 ms and reach the card when the
 * kernel gets to them - about 33 s later, measured. That window is the right
 * trade here and self-correcting: by the time a session is long enough to be
 * worth recording, its marker has been on the card for a long while.
 *
 * SESSIONS THAT NEVER REACH EXIT. A power cut or a flat battery means no EXIT,
 * so a marker is written a few seconds in and updated every minute or so. On
 * the next boot an unclosed row is closed at its last checkpoint - short by up
 * to that interval rather than lost entirely.
 *
 * Rows live in the LIBRARY database, not the device one: play time belongs to
 * the card the games are on, the same argument favorites and earned
 * achievements are stored under. Keyed
 * "sess.<start>.<clock>.<tag>\t<file>", where start is the unix time the
 * session began and clock is the millisecond clock at RUN. The second number
 * is there only to keep two launches of one game inside the same second from
 * sharing a key, and it has to be something already in hand: asking the
 * database whether a key is free would put a read on the launch path.
 */
#ifndef TORTOS_STATS_H
#define TORTOS_STATS_H

#include <stdbool.h>
#include <stddef.h>

/* A game is a system tag plus the ROM's launch path, the same key favorites
 * use, so a file of the same name on two systems counts as two games. */
#define STATS_TAG_MAX   16
#define STATS_FILE_MAX 544
#define STATS_MAX      512   /* distinct games held in memory when summarizing */

/* Marker after this long, so a launch someone backs straight out of writes
 * nothing at all. Then a checkpoint at this interval, which is also the most
 * a crash can lose. */
#define STATS_MARK_MS  (5 * 1000u)
#define STATS_CKPT_MS  (60 * 1000u)

/* RUN. In-memory only - see the header note. */
void stats_begin(const char *tag, const char *file, unsigned now_ms);

/* Time the device spent asleep mid-session, which is not play: NextUI stops
 * its play clock across sleep (gametimectl stop_all/resume). Subtracted from
 * the session, never from the key - g_t0 names the row. */
void stats_asleep(unsigned ms);

/* From the game tick. Writes at most one unsynced row, and usually nothing. */
void stats_tick(unsigned now_ms);

/* EXIT. The one write that is synced, on the path where the player is already
 * waiting for the shelf rather than for a game. `reason` is Diatom's own EXIT
 * reason, or NULL for a clean quit. */
void stats_end(const char *reason, unsigned now_ms);

/* Close any session left open by a power cut, at its last checkpoint. Call
 * once at startup, before anything reads a total. */
void stats_recover(void);

/* --- reading, for the menu ---------------------------------------------- */

/* Which slice of history a summary covers.
 *
 * CALENDAR boundaries, not rolling ones: "this week" is the week you are in,
 * not the last seven days. Someone asking what they played this week means
 * the week, and a rolling window answers a question nobody asked - it also
 * changes its own answer every hour, which makes a number impossible to
 * check twice.
 *
 * Applied to the session's START, which is the key's first segment, so a
 * session that ran across midnight belongs to the day it began on. */
typedef enum {
	STATS_ALL, STATS_YEAR, STATS_MONTH, STATS_WEEK, STATS_TODAY,
	STATS_WINDOWS
} stats_window;

/* "All Time", "This Year", "This Month", "This Week", "Today". */
const char *stats_window_name(stats_window w);

/* The unix time a window opens at, or 0 for STATS_ALL. Exposed for the check,
 * which has to be able to place a session on either side of a boundary. */
long stats_window_start(stats_window w, long now);

/* Fold the sessions inside `w` into one row per game - or one per SYSTEM when
 * `by_system`, which is the "what do I actually play" question rather than
 * "which game" - most-played first. `now` is passed in for the same reason
 * now_ms is: a window boundary has to be drivable from a test.
 *
 * Returns the number of rows. Cheap enough to call on every view change: it
 * is one pass over the session rows, and there are hundreds, not millions. */
int  stats_summarize(stats_window w, bool by_system, long now);

/* tag is the system, and file is the ROM - empty when the rows were folded
 * by system, because a system is not one file. */
bool stats_at(int i, const char **tag, const char **file,
              long *seconds, int *launches);

/* The rest of what a row knows: the longest single session in it, the unix
 * time it was last started, and how many of its sessions ended `lost` rather
 * than `quit`. */
bool stats_extra(int i, long *longest, long *last, int *lost);

/* One game's totals, for a caller that has a game rather than a row: the
 * shelf sorting by play time asks this per entry. Reads whatever the last
 * stats_summarize folded, so call that first - and note the folded rows are
 * shared with the play-time screen, which re-folds them under its own window
 * whenever it is opened. Both re-fold on entry, so neither reads the other's
 * answer today; it is one set of rows, not two, and worth knowing.
 *
 * Returns false for a game with no sessions, leaving the outputs untouched. */
bool stats_lookup(const char *tag, const char *file,
                  long *seconds, long *last);

long stats_total_seconds(void);
int  stats_total_launches(void);

/* Sessions that never reached EXIT - a flat battery, a power cut, or a crash.
 * A play statistic only by accident: it is the one number either project has
 * that says a game DIED rather than was quit. */
int  stats_total_lost(void);

/* "12h 34m", "7m 12s", "never". `out` takes at least 16 bytes. */
void stats_format(long seconds, char *out, size_t n);

/* "3h ago", "2d ago", "today", "never". `out` takes at least 16 bytes. */
void stats_ago(long then, long now, char *out, size_t n);

#endif
