/* SPDX-License-Identifier: MIT */
#ifndef TORTOS_MUSEC_H
#define TORTOS_MUSEC_H

#include <stdbool.h>
#include <stddef.h>

#include "musequeue.h"

/* The launcher's side of Muse: the connection to the daemon, and the play
 * queue.
 *
 * The daemon (src/muse/) knows one file at a time. What comes next is policy,
 * and policy is the launcher's - so this holds the queue, and when the daemon
 * says END it sends the next PLAY itself.
 *
 * Polled, never waited on: musec_poll reads whatever has arrived and returns.
 * It is called from the shelf's main loop, from Muse's own screen, and from
 * the 10 Hz tick the launcher gets while a game runs - which is what lets an
 * album carry on through a game, the way EROS's Muse did. On the speaker and
 * the jack the mixing needs nothing from anybody: both processes open
 * `default`, which is dmix. A Bluetooth headset is different - see musec_sink. */

typedef enum { MU_OFF, MU_STOPPED, MU_PLAYING, MU_PAUSED } mu_state;

typedef struct {
	mu_state state;
	char     title[128], artist[128], album[128];
	double   at, len;
	double   speed;                 /* 1.0 but on a book set otherwise */
	/* Where in the queue, and how long it is. The PLAY order's place, so
	 * shuffled it counts the order being heard rather than album order. */
	int      index, count;
} mu_now;

/* Where the daemon lives, and the folder paths are relative to: the card,
 * since tracks come from Music and books from Audiobooks. Starting it is lazy:
 * the first call that wants it spawns it if it is not answering. */
void musec_init(const char *muse_bin, const char *root);

void musec_poll(void);

/* Queue `n` tracks - paths relative to the root - and play from `start`,
 * `at` seconds into it. The launcher's copies; the caller's array may go away.
 *
 * A `book` queue plays in order whatever the play mode is: a book shuffled or
 * on repeat is not a way anyone listens to one. The mode is left as it was
 * set, for the music after it. */
void musec_play(const char *const *paths, int n, int start, double at, bool book,
                double speed, const char *artist, const char *album);

/* Whether the queue is a book. */
bool musec_is_book(void);

/* A book's speed, 0.5 to 2.0 with the pitch kept - the daemon's SPEED. Set on
 * the playing queue at once and carried by every PLAY after it: the daemon
 * treats a PLAY with no speed as 1x, so a speed the launcher did not repeat
 * would be lost at the next file. */
void   musec_set_speed(double x);
double musec_speed(void);

/* The chapters of the file playing, as the daemon read them from it (an
 * M4B's; a folder of MP3s has none, its files are its parts). A book with more
 * than one moves by chapter on L1 and R1 - see musec_next. */
int         musec_chapters(void);
double      musec_chapter_at(int i);
const char *musec_chapter_title(int i);
/* The chapter the playing position is in, or -1 with none. */
int         musec_chapter_now(void);

/* True once each time a queue plays out to its end - the last track ended by
 * itself, not stopped or replaced. A book that does has been finished. */
bool musec_take_ran_out(void);

void musec_toggle(void);       /* pause or resume */
/* The next and previous track - or, in a book whose file has chapters, the
 * next chapter and the start of this one (or the one before, within three
 * seconds of its start), falling back to the files at either end. */
void musec_next(void);
void musec_prev(void);
void musec_seek_by(double delta);
void musec_stop(void);

const mu_now *musec_now(void);

/* The queued track, relative to the music root, or "" - so a list can mark
 * the row that is playing without comparing titles, which tags can change. */
const char *musec_path(void);

/* Playing right now, for Auto Off: music is somebody using the device with
 * nobody touching it, the same as the charger. */
bool musec_playing(void);

/* Playing, or asked to and not yet answered. What decides who holds a
 * Bluetooth headset, which has to be true from the moment a PLAY is sent:
 * musec_playing only turns true when the daemon says so, and in between the
 * route would hand the headset back. */
bool musec_heard(void);

/* The output. bluealsa here gives a headset to one process at a time, so it
 * is handed between Diatom and Muse rather than shared: whoever may be heard
 * holds it (ADR-0032 already quiets a game while music plays). `device` is an
 * ALSA name, or "" for the default. Sent only when it changes; the daemon
 * tries a headset for up to a second, while the other side lets go, and falls
 * back to the default if it never does. */
void musec_sink(const char *device);
/* Make the next musec_sink send even if the device has not changed: for a
 * headset that dropped and came back under the same name, which Muse may
 * have fallen back from while it was gone. The daemon ignores a SINK for the
 * device it is already on, so this costs nothing when it never left. */
void musec_sink_again(void);
/* Whether the last musec_sink has been answered, or given up on - so the
 * launcher can hand the headset to Diatom only once Muse has let go. */
bool musec_sink_settled(void);
/* What the daemon last said it is on, or "" before it has said, or while a
 * SINK is waiting for its answer. */
const char *musec_sink_now(void);
/* Called just before a PLAY or RESUME is sent, so the launcher can move the
 * headset to Muse first. */
void musec_on_before_heard(void (*fn)(void));

/* The queue's track `i` in PLAY order - shuffled, the i-th to be heard -
 * relative to the music root, or NULL past either end. */
const char *musec_track(int i);

/* What plays when this track finishes, or NULL when that is nothing or not yet
 * decided - see muq_upcoming. For "Next:", which has to follow the mode. */
const char *musec_upcoming(void);

/* The play mode, src/musequeue.h. Applied to the queue playing now without
 * changing its track, and kept for every queue after it; saving it is the
 * caller's. */
void     musec_set_mode(muq_mode m);
muq_mode musec_mode(void);

/* Ask for the picture `track` (relative to the music root) carries, written to
 * `base` plus the extension its format has. The daemon answers in a few
 * milliseconds and the answer is collected with musec_cover_take.
 *
 * False when it could not be asked - the daemon is not up yet, and asking has
 * just started it - and the caller asks again later. An answer can also be
 * lost with the daemon, so a caller that asked and heard nothing should ask
 * again after a while rather than wait for good. */
bool musec_cover_ask(const char *track, const char *base);

/* One answer: the `base` asked about, and the file written there - "" when the
 * track carries no picture or it could not be written. False when none is
 * waiting. */
bool musec_cover_take(char *base, size_t bn, char *file, size_t fn);

/* The year `track` (relative to the music root) is tagged with, asked the same
 * way and lost the same way; one at a time - an answer not taken is replaced
 * by the next. Taken as the track asked about and the year: 0 none in the
 * tags, -1 the file would not open (plorpos-xav). */
bool musec_year_ask(const char *track);
bool musec_year_take(char *track, size_t n, int *year);

/* Ask who the tracks in `dir` (relative to the music root) are by, from their
 * tags: the album artist, or the artist every track agrees on, or "" (see
 * src/muse/tags.h). False when it could not be asked, as musec_cover_ask. The
 * answer is collected with musec_artist_take, `dir` as it was asked. */
bool musec_artist_ask(const char *dir);
bool musec_artist_take(char *dir, size_t dn, char *name, size_t nn);

#endif
