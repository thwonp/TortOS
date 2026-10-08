/* SPDX-License-Identifier: MIT */
/* Muse: TortOS's audio player, the engine half.
 *
 * A daemon over a Unix socket - the Diatom relationship again, and for the
 * same reason: an engine that does one thing, with the policy in the launcher.
 * This half decodes, plays, seeks and says where it is. It knows files and
 * transport and nothing else: not what an album is, not what a book is, not
 * what should play next. Those are tortos.elf's, because that is where the
 * screen and the frame loop are.
 *
 * Separate from the launcher so that music survives the launcher restarting,
 * which is the one argument for a second process that did not go stale when
 * the launcher grew worker threads (BACKLOG, the Muse scoping).
 *
 * THE PROTOCOL, one line each way, tab-separated key=value after a verb:
 *
 *   in   PLAY    path=  [at=]  [speed=]   open and play, from `at` seconds
 *        PAUSE | RESUME | STOP
 *        SEEK    at=
 *        SPEED   x=                          0.5-2.0, pitch preserved
 *        SINK    device=                     an ALSA name; "default" is dmix
 *        STATUS
 *        COVER   path=  base=                write path's picture to base.jpg|png
 *        YEAR    path=                       the year path's tags give it
 *        ARTIST  dir=                        who a folder of tracks is by
 *   out  READY   proto=2
 *        STATE   state=playing|paused|stopped  path=
 *        META    title= artist= album= len= chapters=
 *        CHAPTER i= at= title=                 one per chapter, after META
 *        POS     at= len=                      about once a second while playing
 *        END     path=                         the file ran out by itself
 *        SINK    device=                       the one in use, after each SINK
 *                                              and on STATUS; "default" when a
 *                                              device would not open
 *        ERROR   why=
 *        COVER   base=  file=                  what was written; "" if nothing
 *        YEAR    path=  year=                  0 for none, -1 unreadable
 *        ARTIST  dir=  name=                   from the tags; "" if no clear
 *                                              answer (tags.h)
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "cover.h"
#include "dec.h"
#include "pcm.h"
#include "tags.h"

#define SOCK_PATH "/tmp/muse.sock"
#define PATH_MAX_ 1024
#define CHUNK     1024                   /* frames per write: ~21 ms */

typedef enum { ST_STOPPED, ST_PLAYING, ST_PAUSED } st;
typedef enum { C_NONE, C_PLAY, C_PAUSE, C_RESUME, C_STOP, C_SEEK, C_SPEED,
               C_SINK } cmd;

/* Everything the two threads share, under one lock. The player thread owns
 * the decoder and the output; the main thread owns the socket. Neither
 * touches the other's. */
static struct {
	pthread_mutex_t mu;
	pthread_cond_t  cv;

	st     state;
	char   path[PATH_MAX_];
	double heard, len, speed;
	/* What the file says about itself, copied out when it opens so the
	 * socket thread never reaches into the decoder the player thread is
	 * using. 256 chapters is more than any book this card will carry. */
	char   title[256], artist[256], album[256];
	int    nch;
	double ch_at[256];
	char   ch_title[256][96];
	int    ev_meta, ev_end, ev_state, ev_sink;   /* for the main thread to announce */
	char   ev_err[160];
	char   sink[128];                 /* g_dev, for the main thread to report */
} S = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER,
        .sink = "default" };

/* Commands waiting for the player thread, in order.
 *
 * A QUEUE, and not the single slot this started as. The slot let a second
 * command replace the first before the player had taken it, which was meant
 * for the case where it is right - two seeks, only the last matters - and was
 * wrong for every other pair: PLAY then SPEED sent together lost the PLAY, and
 * the book never started. Found by doing exactly that on 2026-09-18.
 *
 * So repeats of the SAME verb still merge, where the newest is the only one
 * that means anything - SEEK, SPEED, SINK - and everything else waits its turn. */
#define QCAP 16
typedef struct { cmd c; char path[PATH_MAX_]; char dev[128]; double at, speed; } qcmd;
static qcmd Q[QCAP];
static int  q_head, q_len;               /* under S.mu */

static void q_push(const qcmd *in)
{
	int last = (q_head + q_len - 1) % QCAP;

	if (q_len > 0 && Q[last].c == in->c &&
	    (in->c == C_SEEK || in->c == C_SPEED || in->c == C_SINK)) {
		Q[last] = *in;
		return;
	}
	if (q_len == QCAP) { q_head = (q_head + 1) % QCAP; q_len--; }  /* drop the oldest */
	Q[(q_head + q_len) % QCAP] = *in;
	q_len++;
}

static int   g_wake[2] = { -1, -1 };   /* player -> main: something to say */
static dec  *g_dec;                    /* player thread only */
static char  g_dev[128] = "default";   /* player thread only */
/* The codec, where Muse goes when it has nowhere else: "default" unless the
 * launcher names it in MUSE_CODEC. The Bricks' alsa-lib settles what
 * "default" means once per process, and a process that met a USB DAC there
 * kept meaning the DAC after it was pulled - every open busy, every track
 * skipped (plorpos-8wc.8). Their launcher names /etc/asound.conf's Playback. */
static const char *g_codec = "default";
static volatile sig_atomic_t g_quit;

static void wake(void)
{
	char b = 1;
	if (write(g_wake[1], &b, 1) < 0) { /* full is fine: a wake is pending */ }
}

static void say(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "muse: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* ---- the player thread -------------------------------------------------- */

static void fail(const char *why)
{
	snprintf(S.ev_err, sizeof S.ev_err, "%s", why);
	S.state = ST_STOPPED;
	S.ev_state = 1;
	wake();
}

/* Open S.path at `at`, replacing whatever is open. Called with the lock held;
 * opening is a few milliseconds and nothing else is waiting on it. */
static int reopen(double at)
{
	char err[160];

	dec_close(g_dec);
	g_dec = dec_open(S.path, at, S.speed, err, sizeof err);
	if (!g_dec) { fail(err); return 0; }
	S.len = dec_len(g_dec);
	S.heard = at;
	return 1;
}

static void snapshot(void)
{
	int i;

	snprintf(S.title, sizeof S.title, "%s", dec_tag(g_dec, "title"));
	snprintf(S.artist, sizeof S.artist, "%s", dec_tag(g_dec, "artist"));
	/* The album's artist when the track has none of its own, which is how
	 * every rip on the first card was tagged: TPE2 filled, TPE1 empty. On a
	 * compilation the two differ and the track's own wins, as it should. */
	if (!S.artist[0])
		snprintf(S.artist, sizeof S.artist, "%s", dec_tag(g_dec, "album_artist"));
	snprintf(S.album, sizeof S.album, "%s", dec_tag(g_dec, "album"));
	S.nch = dec_chapters(g_dec);
	if (S.nch > 256) S.nch = 256;
	for (i = 0; i < S.nch; i++) {
		S.ch_at[i] = dec_chapter_at(g_dec, i);
		snprintf(S.ch_title[i], sizeof S.ch_title[i], "%s",
		         dec_chapter_title(g_dec, i));
	}
}

static void handle(const qcmd *q)
{
	static const char *const names[] = { "none", "PLAY", "PAUSE", "RESUME", "STOP",
	                                     "SEEK", "SPEED", "SINK" };

	if (q->c != C_NONE)
		pcm_note("%s%s%s", names[q->c], q->c == C_SINK ? " " : "",
		         q->c == C_SINK ? q->dev : "");
	switch (q->c) {
	case C_NONE:
		break;
	case C_PLAY:
		snprintf(S.path, sizeof S.path, "%s", q->path);
		/* A PLAY with no speed means normal speed. It used to mean "whatever
		 * the last one was", and speed is a property of a book, not of the
		 * daemon: a 1.5x set for one audiobook played every song after it
		 * rushed. Found on the device 2026-09-18, the first time the launcher
		 * rather than a test drove it. */
		S.speed = q->speed > 0 ? q->speed : 1.0;
		pcm_drop();
		if (reopen(q->at)) {
			snapshot();
			S.state = ST_PLAYING;
			S.ev_meta = S.ev_state = 1;
			wake();
		}
		break;
	case C_PAUSE:
		if (S.state != ST_PLAYING) break;
		/* What has been HEARD, not what the decoder reached: the queue is
		 * thrown away, so resuming from the decoder's position would skip
		 * the 200 ms that never played - a word, in an audiobook. */
		pcm_drop();
		S.state = ST_PAUSED;
		S.ev_state = 1;
		wake();
		break;
	case C_RESUME:
		if (S.state != ST_PAUSED) break;
		if (reopen(S.heard)) { S.state = ST_PLAYING; S.ev_state = 1; wake(); }
		break;
	case C_SEEK:
		if (S.state == ST_STOPPED) break;
		pcm_drop();
		if (!reopen(q->at)) break;
		wake();
		break;
	case C_SPEED:
		if (q->speed < 0.5 || q->speed > 2.0) break;
		S.speed = q->speed;
		if (S.state == ST_STOPPED) break;
		pcm_drop();
		reopen(S.heard);
		break;
	case C_SINK: {
		/* Tried for up to a second before falling back, because a headset
		 * is handed over rather than shared. bluealsa 3.1 gives an A2DP PCM
		 * to one client at a time - measured 2026-09-25, a second open fails
		 * in hw_params - so Muse can only have it once Diatom has let go,
		 * and the launcher tells Diatom first but cannot see when it has.
		 * `default` is dmix and always opens, so it is tried once. */
		int tries = strcmp(q->dev, g_codec) ? 10 : 1, i;
		struct timespec t0, t1;

		/* Already there: reported, and nothing reopened. The launcher resends
		 * a headset that came back under the same name, and a reopen here
		 * would put a gap in a song that never left it. Diatom's port does
		 * the same for the same reason. */
		if (!strcmp(q->dev, g_dev) && pcm_is_open()) {
			S.ev_sink = 1;
			wake();
			break;
		}
		clock_gettime(CLOCK_MONOTONIC, &t0);
		snprintf(g_dev, sizeof g_dev, "%s", q->dev);
		for (i = 0; i < tries; i++) {
			if (pcm_open(g_dev)) break;
			/* Only busy is worth a wait - Diatom letting go. Anything else
			 * fails the same way ten times: a rate the device would not take
			 * spent the whole second before falling back, 2026-09-25. */
			if (!pcm_busy()) { i = tries; break; }
			if (i + 1 < tries) {
				/* Unlocked while it waits, so the socket thread can still
				 * take commands and answer STATUS. */
				pthread_mutex_unlock(&S.mu);
				usleep(100000);
				pthread_mutex_lock(&S.mu);
			}
		}
		clock_gettime(CLOCK_MONOTONIC, &t1);
		if (i == tries) {
			say("%s; falling back to %s", pcm_error(), g_codec);
			snprintf(g_dev, sizeof g_dev, "%s", g_codec);
			pcm_open(g_dev);
		} else {
			/* Logged every time: it happens only at a handover, and how long
			 * the other side took to let go is the number worth having. */
			say("sink %s after %ld ms, %d tr%s", g_dev,
			    (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000,
			    i + 1, i ? "ies" : "y");
		}
		snprintf(S.sink, sizeof S.sink, "%s", g_dev);
		S.ev_sink = 1;
		wake();
		if (S.state == ST_PLAYING) reopen(S.heard);
		break;
	}
	case C_STOP:
		pcm_drop();
		dec_close(g_dec);
		g_dec = NULL;
		S.state = ST_STOPPED;
		S.ev_state = 1;
		wake();
		break;
	}
}

#ifdef MUSE_HANDOVER
/* THE H700'S CODEC IS HANDED OVER, NOT SHARED (plorpos-7ny.10): BaseOS has no
 * dmix and its kernel no SysV IPC to build one, so Muse and Diatom take turns
 * on `default`. Diatom holds it only while a game runs unquieted; Muse opens it
 * to play - waiting up to a second for Diatom to let go, as for a headset at
 * SINK - and closes it after IDLE_CLOSE_MS of anything but playing, which
 * covers PAUSE, STOP and the end of a list alike. Called locked. */
#define IDLE_CLOSE_MS 500

static bool claim(void)
{
	int i;

	for (i = 0; i < 10; i++) {
		if (pcm_open(g_dev)) return true;
		if (!pcm_busy()) break;
		pthread_mutex_unlock(&S.mu);
		usleep(100000);
		pthread_mutex_lock(&S.mu);
	}
	return false;
}
#endif

static void *player(void *arg)
{
	static int16_t buf[CHUNK * DEC_CHANNELS];

	(void)arg;
#ifndef MUSE_HANDOVER
	if (!pcm_open(g_dev)) say("%s", pcm_error());
#endif
	pthread_mutex_lock(&S.mu);
	while (!g_quit) {
		int n;
		bool lost = false;

#ifdef MUSE_HANDOVER
		while (q_len == 0 && S.state != ST_PLAYING && !g_quit && pcm_is_open()) {
			struct timespec t;

			clock_gettime(CLOCK_REALTIME, &t);
			t.tv_nsec += IDLE_CLOSE_MS * 1000000L;
			if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
			if (pthread_cond_timedwait(&S.cv, &S.mu, &t) == ETIMEDOUT &&
			    q_len == 0 && S.state != ST_PLAYING)
				pcm_close();
		}
#endif
		while (q_len == 0 && S.state != ST_PLAYING && !g_quit)
			pthread_cond_wait(&S.cv, &S.mu);
		if (q_len > 0) {
			qcmd q = Q[q_head];

			q_head = (q_head + 1) % QCAP;
			q_len--;
			handle(&q);
			continue;
		}
		if (S.state != ST_PLAYING || !g_dec) continue;
#ifdef MUSE_HANDOVER
		if (!pcm_is_open()) {
			if (!claim()) {
				fail(pcm_error());
				dec_close(g_dec);
				g_dec = NULL;
				continue;
			}
			continue;
		}
#endif

		/* Decode and write WITHOUT the lock: the write blocks for as long as
		 * the device takes to drain, and a command arriving meanwhile must
		 * not wait behind it. */
		pthread_mutex_unlock(&S.mu);
		n = dec_read(g_dec, buf, CHUNK);
		if (n > 0 && !pcm_write(buf, n)) { n = -1; lost = true; }
		pthread_mutex_lock(&S.mu);

		/* A headset that went away mid-song: the default, and on from what was
		 * heard, the way Diatom's port falls back when its sink dies. Reported
		 * as an ERROR this was read as a track that would not open, so every
		 * write that failed skipped one: a headset switched off ran through the
		 * album, measured 2026-09-25. SINK says where it went, and the launcher
		 * sends the headset back when it reconnects. */
		if (lost && strcmp(g_dev, g_codec) && g_dec) {
			/* A USB DAC (the launcher names one plughw:CARD=<id>) is pulled
			 * out by hand, from headphones the player is wearing: arrive on
			 * the codec PAUSED, as a phone does. Playing on and leaving the
			 * pause to the launcher put half a second of the album out of the
			 * speaker first - its look comes every 500 ms (Brick Pro,
			 * 2026-10-07). A headset that drops keeps playing on. */
			bool dac = !strncmp(g_dev, "plughw:CARD=", 12);

			say("%s; the %s went, falling back to %s%s", pcm_error(),
			    dac ? "USB DAC" : "headset", g_codec, dac ? ", paused" : "");
			snprintf(g_dev, sizeof g_dev, "%s", g_codec);
			pcm_open(g_dev);
			snprintf(S.sink, sizeof S.sink, "%s", g_dev);
			S.ev_sink = 1;
			reopen(S.heard);
			if (dac) {
				S.state = ST_PAUSED;
				S.ev_state = 1;
			}
			wake();
			continue;
		}

		if (n > 0) {
			double h = dec_pos(g_dec) - (double)pcm_queued() * S.speed / DEC_RATE;

			S.heard = h > 0 ? h : 0;
			continue;
		}
		if (n == 0) {
			/* The end of the file. What is queued plays out on its own; the
			 * launcher hears END and decides what comes next. */
			dec_close(g_dec);
			g_dec = NULL;
			S.state = ST_STOPPED;
			S.ev_end = S.ev_state = 1;
			wake();
			continue;
		}
		fail(pcm_error()[0] ? pcm_error() : "the file stopped decoding");
		dec_close(g_dec);
		g_dec = NULL;
	}
	pthread_mutex_unlock(&S.mu);
	pcm_close();
	return NULL;
}

/* ---- the socket ---------------------------------------------------------- */

/* A value from `line`, or "" - `key=` up to the next tab. */
static void arg(const char *line, const char *key, char *out, size_t n)
{
	size_t kl = strlen(key);
	const char *p = line;

	out[0] = '\0';
	while ((p = strchr(p, '\t'))) {
		p++;
		if (!strncmp(p, key, kl) && p[kl] == '=') {
			size_t len = strcspn(p + kl + 1, "\t\r\n");

			if (len >= n) len = n - 1;
			memcpy(out, p + kl + 1, len);
			out[len] = '\0';
			return;
		}
	}
}

/* A value going OUT, with the two characters the protocol uses as structure
 * taken out of it. A tag is somebody else's text. */
static const char *clean(const char *s, char *buf, size_t n)
{
	size_t i;

	for (i = 0; s[i] && i + 1 < n; i++)
		buf[i] = (s[i] == '\t' || s[i] == '\n' || s[i] == '\r') ? ' ' : s[i];
	buf[i] = '\0';
	return buf;
}

static void send_line(int fd, const char *fmt, ...)
{
	char line[2048];
	va_list ap;
	int n;

	if (fd < 0) return;
	va_start(ap, fmt);
	n = vsnprintf(line, sizeof line - 1, fmt, ap);
	va_end(ap);
	if (n < 0) return;
	if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
	line[n++] = '\n';
	if (send(fd, line, (size_t)n, MSG_NOSIGNAL) < 0) { /* the client went */ }
}

static const char *st_name(st s)
{
	return s == ST_PLAYING ? "playing" : s == ST_PAUSED ? "paused" : "stopped";
}

static void announce(int fd, int all)
{
	char a[PATH_MAX_], b[256], c[256];

	pthread_mutex_lock(&S.mu);
	if (S.ev_meta || (all && S.state != ST_STOPPED)) {
		int i;

		send_line(fd, "META\ttitle=%s\tartist=%s\talbum=%s\tlen=%.1f\tchapters=%d",
		          clean(S.title, a, sizeof a), clean(S.artist, b, sizeof b),
		          clean(S.album, c, sizeof c), S.len, S.nch);
		for (i = 0; i < S.nch; i++)
			send_line(fd, "CHAPTER\ti=%d\tat=%.1f\ttitle=%s", i, S.ch_at[i],
			          clean(S.ch_title[i], a, sizeof a));
	}
	if (S.ev_err[0]) {
		send_line(fd, "ERROR\twhy=%s", clean(S.ev_err, a, sizeof a));
		say("error: %s", S.ev_err);
	}
	if (S.ev_end) send_line(fd, "END\tpath=%s", clean(S.path, a, sizeof a));
	if (S.ev_state || all)
		send_line(fd, "STATE\tstate=%s\tpath=%s", st_name(S.state),
		          clean(S.path, a, sizeof a));
	if (all || S.state == ST_PLAYING)
		send_line(fd, "POS\tat=%.1f\tlen=%.1f", S.heard, S.len);
	if (S.ev_sink || all) send_line(fd, "SINK\tdevice=%s", clean(S.sink, a, sizeof a));
	S.ev_meta = S.ev_end = S.ev_state = S.ev_sink = 0;
	S.ev_err[0] = '\0';
	pthread_mutex_unlock(&S.mu);
}

static void command(int fd, const char *line)
{
	static qcmd q;
	char v[64];

	memset(&q, 0, sizeof q);
	if (!strncmp(line, "COVER", 5)) {
		/* On this thread and not the player's. A picture is read from the
		 * file's header in milliseconds and has nothing to do with what is
		 * playing, and queued behind the player it would wait for a seek.
		 * The answer goes straight back, because this thread owns the
		 * socket. */
		char base[PATH_MAX_], file[PATH_MAX_ + 8], a[PATH_MAX_ + 8];

		arg(line, "path", q.path, sizeof q.path);
		arg(line, "base", base, sizeof base);
		if (!q.path[0] || !base[0]) return;
		if (cover_extract(q.path, base, file, sizeof file) < 0)
			say("no cover from %s", q.path);
		send_line(fd, "COVER\tbase=%s\tfile=%s", clean(base, a, sizeof a), file);
		return;
	}
	if (!strncmp(line, "YEAR", 4)) {             /* as COVER, and why */
		char a[PATH_MAX_ + 8];

		arg(line, "path", q.path, sizeof q.path);
		if (!q.path[0]) return;
		send_line(fd, "YEAR\tpath=%s\tyear=%d", clean(q.path, a, sizeof a), tag_year(q.path));
		return;
	}
	if (!strncmp(line, "ARTIST", 6)) {
		/* Here too, for COVER's reason: headers only, a few milliseconds a
		 * track, nothing to do with what is playing. */
		char dir[PATH_MAX_], name[256], a[PATH_MAX_ + 8], b[300];

		arg(line, "dir", dir, sizeof dir);
		if (!dir[0]) return;
		tags_folder_artist(dir, name, sizeof name);
		send_line(fd, "ARTIST\tdir=%s\tname=%s", clean(dir, a, sizeof a),
		          clean(name, b, sizeof b));
		return;
	}
	if (!strncmp(line, "PLAY", 4)) {
		arg(line, "path", q.path, sizeof q.path);
		arg(line, "at", v, sizeof v);    q.at = atof(v);
		arg(line, "speed", v, sizeof v); q.speed = atof(v);
		if (q.path[0]) q.c = C_PLAY;
	} else if (!strncmp(line, "PAUSE", 5))  q.c = C_PAUSE;
	else if (!strncmp(line, "RESUME", 6))   q.c = C_RESUME;
	else if (!strncmp(line, "STOP", 4))     q.c = C_STOP;
	else if (!strncmp(line, "SEEK", 4)) {
		arg(line, "at", v, sizeof v);
		q.at = atof(v);
		q.c = C_SEEK;
	} else if (!strncmp(line, "SPEED", 5)) {
		arg(line, "x", v, sizeof v);
		q.speed = atof(v);
		q.c = C_SPEED;
	} else if (!strncmp(line, "SINK", 4)) {
		arg(line, "device", q.dev, sizeof q.dev);
		if (q.dev[0]) q.c = C_SINK;
	}
	if (q.c != C_NONE) {
		pthread_mutex_lock(&S.mu);
		q_push(&q);
		pthread_cond_signal(&S.cv);
		pthread_mutex_unlock(&S.mu);
	}

	if (!strncmp(line, "STATUS", 6)) announce(fd, 1);
}

static void on_term(int sig) { (void)sig; g_quit = 1; }

/* Evidence for #42, and only that: nothing here changes what plays. */

/* A copy of the system log - bluetoothd's transport states and owners,
 * bluealsa's errors - which is otherwise a ring in memory that the next
 * reboot loses; stall #2's went that way. Once per stall, so a fork of this
 * process is fine; SIGCHLD is ignored, so nothing has to reap it. */
static void save_syslog(void)
{
	const char *dir = getenv("LOGS_PATH");
	char path[512], cmd[600];
	time_t now = time(NULL);
	struct tm tm;

	localtime_r(&now, &tm);
	snprintf(path, sizeof path, "%s/muse-stall-%04d%02d%02d-%02d%02d%02d.txt",
	         dir && *dir ? dir : "/tmp", tm.tm_year + 1900, tm.tm_mon + 1,
	         tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
	snprintf(cmd, sizeof cmd, "logread > '%s' 2>&1", path);
	if (fork() == 0) {
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	say("system log saved to %s", path);
}

static void stalled(const char *why)
{
	long writing = pcm_in_write_ms();

	if (writing)
		pcm_note("STALLED, inside a write for %ld ms: %s", writing, why);
	else
		pcm_note("STALLED, not in a write: %s", why);
	say("STALLED: %s; %s; the last output calls:", why,
	    writing ? "a write has not returned" : "no write in progress");
	fflush(stderr);
	pcm_trail_dump(2);
	save_syslog();
}

/* Once a second. Playing with a position that has not moved for five seconds
 * is a stall; so is a player that has held the lock for five, which would
 * otherwise freeze this thread in the lock along with it and make Muse look
 * dead to the launcher. Reported once each, and never acted on. Returns
 * whether the lock was free, so the caller does not wait on it either. */
static bool watch(void)
{
	static double last_heard = -1;
	static time_t since;
	static int    misses, reported;
	time_t now = time(NULL);
	char   why[256];
	bool   playing;
	double heard;

	if (pthread_mutex_trylock(&S.mu) != 0) {
		if (++misses == 5) stalled("the player has held its lock for 5 s");
		return false;
	}
	misses = 0;
	playing = S.state == ST_PLAYING;
	heard = S.heard;
	snprintf(why, sizeof why, "playing on %s, stuck at %.1f s for 5 s", S.sink, heard);
	pthread_mutex_unlock(&S.mu);

	if (!playing || heard != last_heard) {
		last_heard = heard;
		since = now;
		reported = 0;
	} else if (!reported && now - since >= 5) {
		reported = 1;
		stalled(why);
	}
	return true;
}

/* A fatal signal leaves one line and the record, then dies the same way. Only
 * write(): Muse B's predecessor vanished mid-song on 2026-09-25 with nothing
 * in the log at all. */
static void on_fatal(int sig)
{
	char line[80] = "muse: fatal signal ";
	size_t n = strlen(line);
	const char *tail = "; the last output calls:\n";

	if (sig >= 10) line[n++] = (char)('0' + sig / 10);
	line[n++] = (char)('0' + sig % 10);
	memcpy(line + n, tail, strlen(tail) + 1);
	if (write(2, line, strlen(line)) < 0) { /* nowhere left to say it */ }
	pcm_trail_dump(2);
	raise(sig);                         /* SA_RESETHAND: the default this time */
}

int main(void)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	pthread_t th;
	int lfd, cfd = -1;
	char buf[4096];
	size_t have = 0;
	const char *codec = getenv("MUSE_CODEC");

	if (codec && *codec) {
		g_codec = codec;
		snprintf(g_dev, sizeof g_dev, "%s", codec);
		snprintf(S.sink, sizeof S.sink, "%s", codec);
	}
	time_t last_pos = 0;

	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	signal(SIGCHLD, SIG_IGN);             /* save_syslog's child */
	openlog("muse", LOG_PID, LOG_USER);
	{
		static const int fatal[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
		struct sigaction sa;
		size_t i;

		memset(&sa, 0, sizeof sa);
		sa.sa_handler = on_fatal;
		sa.sa_flags = SA_RESETHAND;
		for (i = 0; i < sizeof fatal / sizeof fatal[0]; i++)
			sigaction(fatal[i], &sa, NULL);
	}
	snprintf(sa.sun_path, sizeof sa.sun_path, "%s", SOCK_PATH);

	/* One Muse. A second one started by a launcher that did not know the
	 * first was alive finds it answering and leaves. */
	lfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (connect(lfd, (struct sockaddr *)&sa, sizeof sa) == 0) {
		say("already running");
		return 0;
	}
	close(lfd);
	unlink(SOCK_PATH);
	lfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (bind(lfd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(lfd, 2) < 0) {
		say("cannot listen on %s: %s", SOCK_PATH, strerror(errno));
		return 1;
	}
	if (pipe(g_wake) < 0) return 1;
	fcntl(g_wake[0], F_SETFL, O_NONBLOCK);
	fcntl(g_wake[1], F_SETFL, O_NONBLOCK);

	S.speed = 1.0;
	pthread_create(&th, NULL, player, NULL);
	say("listening on %s", SOCK_PATH);

	while (!g_quit) {
		struct pollfd p[3] = {
			{ lfd, POLLIN, 0 }, { g_wake[0], POLLIN, 0 }, { cfd, POLLIN, 0 },
		};
		int r = poll(p, cfd >= 0 ? 3 : 2, 250);

		if (r < 0 && errno != EINTR) break;

		if (p[0].revents & POLLIN) {
			/* The launcher, again: it restarted, and the newest connection is
			 * the one that is alive. */
			int n = accept(lfd, NULL, NULL);

			if (n >= 0) {
				if (cfd >= 0) close(cfd);
				cfd = n;
				have = 0;
				send_line(cfd, "READY\tproto=2");
				announce(cfd, 1);
			}
		}
		if (p[1].revents & POLLIN) {
			while (read(g_wake[0], buf, sizeof buf) > 0) { }
			announce(cfd, 0);
		}
		if (cfd >= 0 && (p[2].revents & (POLLIN | POLLHUP | POLLERR))) {
			ssize_t got = recv(cfd, buf + have, sizeof buf - have - 1, 0);

			if (got <= 0) { close(cfd); cfd = -1; have = 0; continue; }
			have += (size_t)got;
			buf[have] = '\0';
			for (;;) {
				char *nl = memchr(buf, '\n', have);
				size_t used;

				if (!nl) break;
				*nl = '\0';
				command(cfd, buf);
				used = (size_t)(nl - buf) + 1;
				memmove(buf, nl + 1, have - used);
				have -= used;
			}
			if (have >= sizeof buf - 1) have = 0;   /* a line with no end */
		}
		if (time(NULL) != last_pos) {
			last_pos = time(NULL);
			/* Not the blocking lock it was: a player stuck while holding it
			 * would stop this thread too - see watch. */
			if (!watch()) continue;
			pthread_mutex_lock(&S.mu);
			r = S.state == ST_PLAYING;
			pthread_mutex_unlock(&S.mu);
			if (r) announce(cfd, 0);
		}
	}

	pthread_mutex_lock(&S.mu);
	g_quit = 1;
	pthread_cond_signal(&S.cv);
	pthread_mutex_unlock(&S.mu);
	pthread_join(th, NULL);
	if (cfd >= 0) close(cfd);
	close(lfd);
	unlink(SOCK_PATH);
	return 0;
}
