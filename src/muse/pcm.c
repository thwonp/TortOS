/* SPDX-License-Identifier: MIT */
/* See pcm.h. */
#include "pcm.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

/* libasound's own values, which are ABI and have not moved in two decades. */
#define SND_PCM_STREAM_PLAYBACK       0
#define SND_PCM_FORMAT_S16_LE         2
#define SND_PCM_ACCESS_RW_INTERLEAVED 3

typedef struct snd_pcm snd_pcm_t;

static int         (*a_open)(snd_pcm_t **, const char *, int, int);
static int         (*a_set_params)(snd_pcm_t *, int, int, unsigned, unsigned,
                                   int, unsigned);
static long        (*a_writei)(snd_pcm_t *, const void *, unsigned long);
static int         (*a_recover)(snd_pcm_t *, int, int);
static int         (*a_drop)(snd_pcm_t *);
static int         (*a_prepare)(snd_pcm_t *);
static int         (*a_delay)(snd_pcm_t *, long *);
static int         (*a_close)(snd_pcm_t *);
static const char *(*a_strerror)(int);

static snd_pcm_t *g_pcm;
static char       g_err[128];
static int        g_last;        /* the last open's error, 0 when it worked */

/* The last things done to the output, kept for when it goes wrong - #42.
 * Twice on 2026-09-25 Muse stalled inside bluealsa's plugin, and the second
 * time the Muse before it vanished with no line at all, so there was nothing
 * to say which call it had been making. Each entry is formatted when it is
 * made, wall clock included to line up with the syslog, so that dumping them
 * is only write() and is safe from a signal handler. */
#define TRAIL 32
static char     g_trail[TRAIL][128];
static unsigned g_trail_n;
static volatile long g_write_since;   /* when the write in progress began, or 0 */
static long     g_written;           /* frames written since the last open or prepare */

void pcm_note(const char *fmt, ...)
{
	char *slot = g_trail[g_trail_n % TRAIL];
	struct timespec t;
	struct tm tm;
	va_list ap;
	int n;

	clock_gettime(CLOCK_REALTIME, &t);
	localtime_r(&t.tv_sec, &tm);
	n = snprintf(slot, sizeof g_trail[0], "muse:   %02d:%02d:%02d.%03ld ",
	             tm.tm_hour, tm.tm_min, tm.tm_sec, t.tv_nsec / 1000000);
	va_start(ap, fmt);
	vsnprintf(slot + n, sizeof g_trail[0] - n - 1, fmt, ap);
	va_end(ap);
	n = (int)strlen(slot);
	/* Into the syslog as well, where it lands between bluetoothd's transport
	 * lines in the order things happened. Not by time: the syslog's own stamps
	 * come in batches - two lines three seconds apart read the same to the
	 * millisecond, measured 2026-09-25 - hence the wall clock in the text. */
	syslog(LOG_INFO, "%s", slot + 8);
	slot[n] = '\n';
	slot[n + 1] = '\0';
	g_trail_n++;
}

void pcm_trail_dump(int fd)
{
	unsigned i = g_trail_n > TRAIL ? g_trail_n - TRAIL : 0;

	for (; i < g_trail_n; i++) {
		const char *s = g_trail[i % TRAIL];
		if (write(fd, s, strlen(s)) < 0) return;
	}
}

static long now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

long pcm_in_write_ms(void)
{
	long since = g_write_since;
	return since ? now_ms() - since : 0;
}

static bool bind(void)
{
	static void *h;

	if (h) return true;
	h = dlopen("libasound.so.2", RTLD_NOW);
	if (!h) { snprintf(g_err, sizeof g_err, "no libasound: %s", dlerror()); return false; }
	a_open       = dlsym(h, "snd_pcm_open");
	a_set_params = dlsym(h, "snd_pcm_set_params");
	a_writei     = dlsym(h, "snd_pcm_writei");
	a_recover    = dlsym(h, "snd_pcm_recover");
	a_drop       = dlsym(h, "snd_pcm_drop");
	a_prepare    = dlsym(h, "snd_pcm_prepare");
	a_delay      = dlsym(h, "snd_pcm_delay");
	a_close      = dlsym(h, "snd_pcm_close");
	a_strerror   = dlsym(h, "snd_strerror");
	if (!a_open || !a_set_params || !a_writei || !a_recover || !a_drop ||
	    !a_prepare || !a_delay || !a_close || !a_strerror) {
		snprintf(g_err, sizeof g_err, "libasound is missing a function");
		dlclose(h);
		h = NULL;
		return false;
	}
	return true;
}

bool pcm_open(const char *device)
{
	int r;

	if (!bind()) return false;
	pcm_close();
	r = a_open(&g_pcm, device, SND_PCM_STREAM_PLAYBACK, 0);
	g_last = r < 0 ? r : 0;
	if (r < 0) {
		snprintf(g_err, sizeof g_err, "open %s: %s", device, a_strerror(r));
		pcm_note("%s", g_err);
		g_pcm = NULL;
		return false;
	}
	/* soft_resample 0: the decoder already delivers 48 kHz, and asking ALSA
	 * to resample again would be the awrate path this exists to avoid.
	 * 200 ms of latency: long enough that a busy frame in a running game does
	 * not starve it, short enough that pause and seek feel immediate. */
	r = a_set_params(g_pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
	                 2, 48000, 0, 200000);
	/* Except a headset that is not running at 48 kHz, where ALSA has to
	 * resample or nothing plays. A headset picks the rate when IT starts the
	 * connection: the OpenFit came back at 44.1 kHz on reconnecting by itself
	 * and every open failed on it, measured 2026-09-25. The wired path's
	 * reason against awrate does not carry over - the SBC encode that follows
	 * costs more than the resample does.
	 *
	 * Through `plug:`, because a bare `type bluealsa` PCM has no rate
	 * converter to allow: soft_resample 1 on the PCM itself failed the same
	 * way, measured the same day. Only on a rate mismatch, which is -EINVAL
	 * here; busy stays busy, for the caller to retry. */
	if (r == -EINVAL && strcmp(device, "default")) {
		char plug[160];

		pcm_close();
		snprintf(plug, sizeof plug, "plug:%s", device);
		r = a_open(&g_pcm, plug, SND_PCM_STREAM_PLAYBACK, 0);
		if (r >= 0)
			r = a_set_params(g_pcm, SND_PCM_FORMAT_S16_LE,
			                 SND_PCM_ACCESS_RW_INTERLEAVED, 2, 48000, 1, 200000);
		else
			g_pcm = NULL;
		if (r >= 0)
			fprintf(stderr, "muse: %s is not at 48 kHz; ALSA resamples it\n", device);
	}
	if (r < 0) {
		snprintf(g_err, sizeof g_err, "set params on %s: %s", device, a_strerror(r));
		g_last = r;
		pcm_note("%s", g_err);
		pcm_close();
		return false;
	}
	pcm_note("open %s: ok", device);
	g_written = 0;
	return true;
}

void pcm_close(void)
{
	if (g_pcm) {
		pcm_note("close");
		a_close(g_pcm);
	}
	g_pcm = NULL;
}

bool pcm_write(const int16_t *frames, int n)
{
	while (g_pcm && n > 0) {
		long t0 = now_ms(), took;
		long w;

		g_write_since = t0;
		w = a_writei(g_pcm, frames, (unsigned long)n);
		g_write_since = 0;

		/* A write is ~21 ms of audio into a 200 ms buffer, so a quarter of a
		 * second inside one is the output not taking it - noted, because a
		 * stall starts as exactly that. */
		if ((took = now_ms() - t0) > 250)
			pcm_note("write of %d frames took %ld ms (%s)", n, took,
			          w < 0 ? a_strerror((int)w) : "ok");
		if (w < 0) {
			/* An underrun is ordinary - a pause, a slow card read - and
			 * recover re-prepares the stream. Anything else is not. */
			int r = a_recover(g_pcm, (int)w, 1);

			pcm_note("write: %s, recover %s", a_strerror((int)w), r < 0 ? a_strerror(r) : "ok");
			if (r < 0) {
				snprintf(g_err, sizeof g_err, "write: %s", a_strerror((int)w));
				return false;
			}
			continue;
		}
		frames += w * 2;
		n -= (int)w;
		g_written += w;
	}
	return g_pcm != NULL;
}

void pcm_drop(void)
{
	if (!g_pcm) return;
	/* Nothing to drop on a device that has not been written to since it was
	 * opened or last prepared - and on a bluealsa headset a drop there is
	 * what stalled Muse (#42). The drop makes bluealsa release the A2DP
	 * stream it acquired a millisecond earlier for the open, and the writes
	 * that follow wait for a stream it never takes again. All three stalls on
	 * 2026-09-25/26 were SINK then PLAY, the drop right behind the open; the
	 * third was caught in order in the syslog: Acquire, ACTIVE, the open,
	 * PLAY, drop, Release, IDLE, then a write that never returned. */
	if (!g_written) {
		pcm_note("drop skipped: nothing written since the open or prepare");
		return;
	}
	pcm_note("drop+prepare");
	a_drop(g_pcm);
	a_prepare(g_pcm);
	g_written = 0;
}

long pcm_queued(void)
{
	long d = 0;

	if (!g_pcm || a_delay(g_pcm, &d) < 0 || d < 0) return 0;
	return d;
}

const char *pcm_error(void) { return g_err; }

bool pcm_is_open(void) { return g_pcm != NULL; }

bool pcm_busy(void) { return g_last == -EBUSY; }
