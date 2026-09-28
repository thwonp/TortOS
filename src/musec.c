/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* See musec.h. The protocol is src/muse/muse.c's. */
#include "musec.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "muselib.h"
#include "musequeue.h"
#include "platform.h"

#define MUSE_SOCK "/tmp/muse.sock"

static char    g_bin[256], g_root[256];
static int     g_fd = -1;
static char    g_in[4096];
static size_t  g_have;
static unsigned g_spawned_ms;
static int     g_spawned;

/* Covers the daemon has answered about and nobody has taken yet. A handful:
 * they are asked for one screen at a time, and one dropped here is asked for
 * again by a caller whose answer never came - see musec_cover_ask. */
#define COVER_RING 8
static struct { char base[LIB_PATH * 2], file[LIB_PATH * 2 + 8]; } g_cov[COVER_RING];
static int     g_cov_head, g_cov_n;

static char  **g_q;                 /* the queue, relative paths, ours */
static int     g_qn;
static muq     g_order;             /* which of them plays when: the mode */
static muq_mode g_mode = MUQ_IN_ORDER;
static int     g_pending;           /* a PLAY waiting for the connection */
static bool    g_advanced;          /* the next track is asked for; see event() */
static int     g_skips;             /* consecutive tracks that would not open */
static char    g_artist[128], g_album[128];
static mu_now  g_now;
static bool    g_book;              /* the queue is a book: in order, always */
static double  g_start_at;          /* where the first PLAY of a queue starts */
static bool    g_ran_out;           /* see musec_take_ran_out */

/* The output, which Muse holds for its whole life and has to be HANDED a
 * headset: see musec_sink in musec.h. */
static char     g_sink_sent[128];   /* what it was last asked for, "" unknown */
static char     g_sink_said[128];   /* what it last reported, "" not yet */
static unsigned g_sink_ms;          /* when the ask went */
static bool     g_sink_waiting;     /* asked, and no answer yet */
static bool     g_asked;            /* PLAY or RESUME sent, not yet answered */
static void   (*g_before_heard)(void);

void musec_init(const char *muse_bin, const char *root)
{
	snprintf(g_bin, sizeof g_bin, "%s", muse_bin);
	snprintf(g_root, sizeof g_root, "%s", root);
}

/* `why` is logged when a live connection ends: it is when "Muse vanished"
 * happened, which on 2026-09-25 could only be inferred afterwards - #42. */
static void drop(const char *why, int err)
{
	if (g_fd >= 0) {
		fprintf(stderr, "muse: connection lost: %s%s%s (state %s, sink %s)\n", why,
		        err ? ", " : "", err ? strerror(err) : "",
		        g_now.state == MU_PLAYING ? "playing" : g_now.state == MU_PAUSED ? "paused" : "stopped",
		        g_sink_said[0] ? g_sink_said : "?");
		close(g_fd);
	}
	g_fd = -1;
	g_have = 0;
	if (g_now.state != MU_OFF) g_now.state = MU_OFF;
	/* A new connection may be a new daemon, so its output is not known
	 * until it says; its READY announcement does. */
	g_sink_sent[0] = g_sink_said[0] = '\0';
	g_sink_waiting = g_asked = false;
}

static void sendf(const char *fmt, ...)
{
	char line[1400];
	va_list ap;
	int n;

	if (g_fd < 0) return;
	va_start(ap, fmt);
	n = vsnprintf(line, sizeof line - 1, fmt, ap);
	va_end(ap);
	/* Not sent at all rather than sent cut short: a PLAY cut short plays a
	 * file nobody asked for, and a COVER cut short writes a picture under a
	 * name nobody will look for. Nothing the launcher builds is this long -
	 * two paths of LIB_PATH each - so this is a guard, not a case. */
	if (n < 0 || n > (int)sizeof line - 2) {
		fprintf(stderr, "muse: a line too long to send\n");
		return;
	}
	line[n++] = '\n';
	if (send(g_fd, line, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT) < 0 &&
	    errno != EAGAIN)
		drop("send failed", errno);      /* it went away; the next poll starts it */
}

/* Connected, or on the way. The daemon is started at most once every two
 * seconds, so a Muse that cannot start does not become a fork a frame. */
static int connected(void)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int fd;

	if (g_fd >= 0) return 1;
	snprintf(sa.sun_path, sizeof sa.sun_path, "%s", MUSE_SOCK);
	/* Non-blocking by fcntl rather than SOCK_NONBLOCK, which is Linux's: the
	 * host build compiles this too, and a Unix-socket connect either answers
	 * at once or fails at once either way. */
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd >= 0) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
	if (fd >= 0 && connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) {
		g_fd = fd;
		g_have = 0;
		if (g_now.state == MU_OFF) g_now.state = MU_STOPPED;
		return 1;
	}
	if (fd >= 0) close(fd);
	if (!g_bin[0]) return 0;
	if (!g_spawned || plat_now_ms() - g_spawned_ms > 2000) {
		char *argv[] = { g_bin, NULL };

		plat_spawn_detached(argv, NULL, NULL);
		g_spawned = 1;
		g_spawned_ms = plat_now_ms();
		fprintf(stderr, "muse: started %s\n", g_bin);
	}
	return 0;
}

static void play_current(void)
{
	int t = muq_current(&g_order);
	const char *q;

	if (t < 0 || t >= g_qn) return;
	q = g_q[t];
	/* Held until the daemon has said which output it is on - its READY
	 * announcement ends with SINK - or the route below has nothing to go by
	 * and the first track of a fresh daemon starts on the speaker. */
	if (!connected() || !g_sink_said[0]) { g_pending = 1; return; }
	g_pending = 0;
	/* Routed BEFORE the PLAY goes, so the track starts where it is heard
	 * rather than on the speaker for however long the route takes. The
	 * daemon runs its commands in order, so a SINK sent now lands first. */
	g_asked = true;
	if (g_before_heard) g_before_heard();
	/* Only the queue's first PLAY starts part way: every track after it is
	 * one the queue moved on to, from its beginning. */
	if (g_start_at > 0) sendf("PLAY\tpath=%s/%s\tat=%.1f", g_root, q, g_start_at);
	else                sendf("PLAY\tpath=%s/%s", g_root, q);
	/* Until META says otherwise, the name the file has. */
	ml_track_name(strrchr(q, '/') ? strrchr(q, '/') + 1 : q,
	              g_now.title, sizeof g_now.title);
	snprintf(g_now.artist, sizeof g_now.artist, "%s", g_artist);
	snprintf(g_now.album, sizeof g_now.album, "%s", g_album);
	g_now.at = g_start_at;
	g_start_at = 0;
	g_now.len = 0;
	g_now.index = g_order.pos;
	g_now.count = g_qn;
}

/* The next track the queue's mode names, or the end. `failed` is a track that
 * would not play rather than one that finished - see muq_failed. */
static void advance(bool failed)
{
	if ((failed ? muq_failed(&g_order) : muq_ended(&g_order)) >= 0) {
		play_current();
		g_advanced = true;
	} else {
		g_now.state = MU_STOPPED;
		if (!failed) g_ran_out = true;
	}
}

/* `key=` in a tab-separated line, or "". */
static void field(const char *line, const char *key, char *out, size_t n)
{
	size_t kl = strlen(key);
	const char *p = line;

	out[0] = '\0';
	while ((p = strchr(p, '\t'))) {
		p++;
		if (!strncmp(p, key, kl) && p[kl] == '=') {
			size_t len = strcspn(p + kl + 1, "\t");

			if (len >= n) len = n - 1;
			memcpy(out, p + kl + 1, len);
			out[len] = '\0';
			return;
		}
	}
}

static void event(const char *line)
{
	char v[256];

	if (!strncmp(line, "READY", 5)) {
		if (g_pending) play_current();
	} else if (!strncmp(line, "STATE", 5)) {
		field(line, "state", v, sizeof v);
		/* The track that ended says "stopped" AFTER its END, and by then the
		 * next one has already been asked for. Taken at its word, the queue
		 * reads as stopped for the moment between two tracks - and a game
		 * follows musec_playing to decide whether it is heard, so its sound
		 * came up for a tenth of a second at every track change. */
		if (g_advanced && !strcmp(v, "stopped")) { g_advanced = false; return; }
		g_advanced = false;
		g_asked = false;
		g_now.state = !strcmp(v, "playing") ? MU_PLAYING
		            : !strcmp(v, "paused")  ? MU_PAUSED : MU_STOPPED;
		if (g_now.state == MU_PLAYING) g_skips = 0;
	} else if (!strncmp(line, "META", 4)) {
		/* The file's own tags win where it has them; the folder names stand
		 * in where it does not, which on this card is the artist almost
		 * every time - the rips carry an album tag and no artist one. */
		field(line, "title", v, sizeof v);
		if (v[0]) snprintf(g_now.title, sizeof g_now.title, "%s", v);
		field(line, "artist", v, sizeof v);
		if (v[0]) snprintf(g_now.artist, sizeof g_now.artist, "%s", v);
		field(line, "album", v, sizeof v);
		if (v[0]) snprintf(g_now.album, sizeof g_now.album, "%s", v);
		field(line, "len", v, sizeof v);
		g_now.len = atof(v);
	} else if (!strncmp(line, "POS", 3)) {
		field(line, "at", v, sizeof v);  g_now.at = atof(v);
		field(line, "len", v, sizeof v); if (atof(v) > 0) g_now.len = atof(v);
	} else if (!strncmp(line, "END", 3)) {
		advance(false);
	} else if (!strncmp(line, "SINK", 4)) {
		field(line, "device", g_sink_said, sizeof g_sink_said);
		g_sink_waiting = false;
		/* Taken as the starting point only when nothing has been asked of
		 * this daemon yet. After that, what was ASKED stands: re-asking
		 * because Muse fell back would retry a dead headset every tick,
		 * which is the loop aout_apply's comment in main.c describes. */
		if (!g_sink_sent[0])
			snprintf(g_sink_sent, sizeof g_sink_sent, "%s", g_sink_said);
		if (g_pending) play_current();
	} else if (!strncmp(line, "COVER", 5)) {
		int k = (g_cov_head + g_cov_n) % COVER_RING;

		if (g_cov_n == COVER_RING) {           /* the oldest makes room */
			g_cov_head = (g_cov_head + 1) % COVER_RING;
			g_cov_n--;
			k = (g_cov_head + g_cov_n) % COVER_RING;
		}
		field(line, "base", g_cov[k].base, sizeof g_cov[k].base);
		field(line, "file", g_cov[k].file, sizeof g_cov[k].file);
		g_cov_n++;
	} else if (!strncmp(line, "ERROR", 5)) {
		field(line, "why", v, sizeof v);
		fprintf(stderr, "muse: %s\n", v);
		/* A track that will not open is skipped rather than ending the
		 * album - but not forever: a whole album of files that fail is a
		 * folder problem, and walking it at a frame a track says nothing. */
		if (++g_skips < 8) advance(true); else g_now.state = MU_STOPPED;
	}
}

/* A headset's own play, pause, next and previous, which btplayer reads off its
 * AVRCP keyboard and writes as `<action> <stamp>` (tools/btplayer.c). Here and
 * not in any one screen, because musec_poll runs wherever Muse matters - the
 * shelf, the menus, a game's tick, Muse's own screens. Only ever Muse, even in
 * a game: pausing the music is what brings the game's sound back (ADR-0032),
 * and a headset's button never reaches into the game itself - Eric's call,
 * 2026-09-26. A new stamp is a new press; the one found at start is only
 * noted. Read every tenth of a second, before anything that could return
 * early, so a press made while Muse was not running is used up, not replayed
 * later. */
static void headset_keys(void)
{
	static unsigned looked;
	static long     seen;
	static bool     primed;
	unsigned now = plat_now_ms();
	char     act[16];
	long     stamp;
	FILE    *f;

	if (primed && now - looked < 100) return;
	looked = now;
	f = fopen("/tmp/tortos_btkey", "r");
	if (f && fscanf(f, "%15s %ld", act, &stamp) == 2 && stamp != seen) {
		bool fresh = primed;

		seen = stamp;
		if (fresh) {
			fprintf(stderr, "muse: headset %s\n", act);
			if (!strcmp(act, "toggle"))     musec_toggle();
			else if (!strcmp(act, "pause")) { if (g_now.state == MU_PLAYING) sendf("PAUSE"); }
			else if (!strcmp(act, "next"))  musec_next();
			else if (!strcmp(act, "prev"))  musec_prev();
		}
	}
	if (f) fclose(f);
	primed = true;
}

void musec_poll(void)
{
	ssize_t got;

	headset_keys();

	/* Reconnect whenever there is a queue to look after, not only when a
	 * play is waiting: a daemon that restarts mid-album would otherwise
	 * finish the track it was on and stop, because its END went to a socket
	 * nobody was reading. */
	if (g_fd < 0) {
		if ((!g_pending && g_qn == 0) || !connected()) return;
	}
	for (;;) {
		got = recv(g_fd, g_in + g_have, sizeof g_in - g_have - 1, MSG_DONTWAIT);
		if (got == 0) { drop("closed by Muse", 0); return; }
		if (got < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK) drop("receive failed", errno);
			break;
		}
		g_have += (size_t)got;
		g_in[g_have] = '\0';
		for (;;) {
			char *nl = memchr(g_in, '\n', g_have);
			size_t used;

			if (!nl) break;
			*nl = '\0';
			event(g_in);
			if (g_fd < 0) return;              /* the event dropped us */
			used = (size_t)(nl - g_in) + 1;
			memmove(g_in, nl + 1, g_have - used);
			g_have -= used;
		}
		if (g_have >= sizeof g_in - 1) g_have = 0;
	}
}

void musec_play(const char *const *paths, int n, int start, double at, bool book,
                const char *artist, const char *album)
{
	int i, k = 0;

	for (i = 0; i < g_qn; i++) free(g_q[i]);
	free(g_q);
	g_q = calloc((size_t)(n > 0 ? n : 1), sizeof *g_q);
	g_qn = 0;
	if (!g_q) return;
	for (i = 0; i < n; i++) {
		/* A tab or a newline is structure on the wire; a name holding one
		 * would arrive as a different path. Nobody names a song that way,
		 * and the one that did is left out rather than misplayed. */
		if (strpbrk(paths[i], "\t\n")) { if (i < start) start--; continue; }
		g_q[k] = strdup(paths[i]);
		if (g_q[k]) k++;
	}
	g_qn = k;
	muq_free(&g_order);
	g_book = book;
	muq_init(&g_order, k, start >= 0 && start < k ? start : 0,
	         book ? MUQ_IN_ORDER : g_mode, plat_now_ms() * 2654435761u + 1);
	g_skips = 0;
	g_start_at = at > 0 ? at : 0;
	g_ran_out = false;
	snprintf(g_artist, sizeof g_artist, "%s", artist ? artist : "");
	snprintf(g_album, sizeof g_album, "%s", album ? album : "");
	play_current();
}

void musec_toggle(void)
{
	if (g_now.state == MU_PLAYING) sendf("PAUSE");
	else if (g_now.state == MU_PAUSED) {
		g_asked = true;
		if (g_before_heard) g_before_heard();   /* see play_current */
		sendf("RESUME");
	}
	else if (g_qn > 0) play_current();
}

void musec_on_before_heard(void (*fn)(void)) { g_before_heard = fn; }

bool musec_heard(void) { return g_now.state == MU_PLAYING || g_asked; }

void musec_sink(const char *device)
{
	const char *dev = device && device[0] ? device : "default";

	if (g_fd < 0 || !g_sink_said[0]) return;   /* not connected, or not heard from */
	if (!strcmp(dev, g_sink_sent)) return;
	snprintf(g_sink_sent, sizeof g_sink_sent, "%s", dev);
	g_sink_ms = plat_now_ms();
	g_sink_waiting = true;
	sendf("SINK\tdevice=%s", dev);
	fprintf(stderr, "muse: sink -> %s\n", dev);
}

void musec_sink_again(void)
{
	/* Forget what was asked, so the next musec_sink sends even an unchanged
	 * device. Only while connected and heard from: an empty ask otherwise
	 * means "not known yet", which the SINK answer fills in. */
	if (g_fd >= 0 && g_sink_said[0]) g_sink_sent[0] = '\0';
}

bool musec_sink_settled(void)
{
	/* The daemon answers a SINK after its retries, which are a second at
	 * most; a little over that, and it is taken as done either way. */
	if (g_fd < 0 || !g_sink_waiting) return true;
	return plat_now_ms() - g_sink_ms > 1200;
}

const char *musec_sink_now(void)
{
	/* Not while a SINK is unanswered: until then it is the device before -
	 * the same stale-report trouble as Diatom's, see aout_send in main.c. */
	return g_fd >= 0 && g_sink_said[0] && !g_sink_waiting ? g_sink_said : "";
}

void musec_next(void)
{
	if (muq_next(&g_order) >= 0) play_current();
}

void musec_prev(void)
{
	/* The way every player does it: three seconds in, "back" means the start
	 * of this track; before that it means the one before - and where there is
	 * none before, the start of this one again. */
	if (g_now.at > 3.0 || muq_prev(&g_order) < 0) sendf("SEEK\tat=0");
	else play_current();
}

void musec_seek_by(double delta)
{
	double t = g_now.at + delta;

	if (g_now.state != MU_PLAYING && g_now.state != MU_PAUSED) return;
	if (t < 0) t = 0;
	if (g_now.len > 0 && t > g_now.len - 1) t = g_now.len - 1;
	g_now.at = t;                  /* shown at once, confirmed by the next POS */
	sendf("SEEK\tat=%.1f", t);
}

void musec_stop(void)
{
	sendf("STOP");
}

bool musec_cover_ask(const char *track, const char *base)
{
	if (strpbrk(track, "\t\n") || strpbrk(base, "\t\n")) return false;
	if (!connected()) return false;
	sendf("COVER\tpath=%s/%s\tbase=%s", g_root, track, base);
	return g_fd >= 0;
}

bool musec_cover_take(char *base, size_t bn, char *file, size_t fn)
{
	if (g_cov_n == 0) return false;
	snprintf(base, bn, "%s", g_cov[g_cov_head].base);
	snprintf(file, fn, "%s", g_cov[g_cov_head].file);
	g_cov_head = (g_cov_head + 1) % COVER_RING;
	g_cov_n--;
	return true;
}

const char *musec_track(int i)
{
	return i >= 0 && i < g_qn ? g_q[g_order.order[i]] : NULL;
}

const char *musec_upcoming(void)
{
	int t = muq_upcoming(&g_order);

	return t >= 0 && t < g_qn ? g_q[t] : NULL;
}

void musec_set_mode(muq_mode m)
{
	if ((int)m < 0 || m >= MUQ_MODES) m = MUQ_IN_ORDER;
	g_mode = m;
	if (g_book) return;                  /* see musec_play */
	muq_set_mode(&g_order, m);
	g_now.index = g_order.pos;
}

bool musec_is_book(void) { return g_book; }

bool musec_take_ran_out(void)
{
	bool r = g_ran_out;

	g_ran_out = false;
	return r;
}

muq_mode musec_mode(void) { return g_mode; }

const mu_now *musec_now(void) { return &g_now; }

const char *musec_path(void)
{
	int t = muq_current(&g_order);

	if (g_now.state == MU_OFF || g_now.state == MU_STOPPED) return "";
	return t >= 0 && t < g_qn ? g_q[t] : "";
}

bool musec_playing(void) { return g_now.state == MU_PLAYING; }
