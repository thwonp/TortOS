/* SPDX-License-Identifier: MIT
 *
 * Preloaded into the owner's pico8_64 on the TrimUI Brick (plorpos-gkd.50.13).
 * PICO-8 asks SDL_Init for every subsystem, sensors included, and the Brick's
 * firmware SDL2 was built without them: "SDL not built with sensor support",
 * and PICO-8 stops with "Unable to initialize SDL". The firmware SDL stays -
 * it is the one with the display backend - and this takes the one flag it
 * cannot honour off the request. PICO-8 reads no sensor.
 *
 * It also quiets PICO-8 while Muse plays, as the GKD does through PipeWire
 * (plorpos-reo.11): the launcher's child_quiet creates QUIET_FLAG, and the
 * audio callback below writes silence over PICO-8's samples while it exists.
 * The flag is on tmpfs, so checking it once a buffer costs no card I/O.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define QUIET_FLAG "/tmp/plorpos-pico8-quiet"   /* platform_brick.c's too */

#define SDL_INIT_SENSOR 0x00008000u

int SDL_Init(uint32_t flags)
{
	static int (*real)(uint32_t);

	if (!real) real = (int (*)(uint32_t))dlsym(RTLD_NEXT, "SDL_Init");
	return real ? real(flags & ~SDL_INIT_SENSOR) : -1;
}

int SDL_InitSubSystem(uint32_t flags)
{
	static int (*real)(uint32_t);

	if (!real) real = (int (*)(uint32_t))dlsym(RTLD_NEXT, "SDL_InitSubSystem");
	return real ? real(flags & ~SDL_INIT_SENSOR) : -1;
}

/* SDL2's SDL_AudioSpec, which this file does without the headers for. */
typedef struct {
	int      freq;
	uint16_t format;
	uint8_t  channels, silence;
	uint16_t samples, padding;
	uint32_t size;
	void   (*callback)(void *userdata, uint8_t *stream, int len);
	void    *userdata;
} audio_spec;

#define AUDIO_U8 0x0008   /* the one format whose silence is not zero */

static void (*game_cb)(void *, uint8_t *, int);
static uint8_t silence;

static void quiet_cb(void *userdata, uint8_t *stream, int len)
{
	game_cb(userdata, stream, len);
	if (access(QUIET_FLAG, F_OK) == 0) memset(stream, silence, (size_t)len);
}

#ifdef PICO8_FOLLOW
/* The H700's own: PICO-8's sound follows where the launcher says it should go
 * (plorpos-7ny.35). pico8_64 opens its device once, at start; this reopens it
 * whenever the target changes - a headset that connects or goes, a cable, Muse
 * starting (then nothing is held at all: PICO-8 is quiet anyway, and the
 * headset takes one opener, which Muse needs) - and when SDL lost the device
 * under it. PICO-8 keeps its callback and its format; SDL converts.
 *
 * The launcher writes AUDIO_TARGET ("-": nothing; "": the default device;
 * else a PCM name, a tab and the headset's link id) and reads AUDIO_NOW, what
 * is open here - which is how it knows the headset is free before it freezes
 * PICO-8 for its menu. Both on tmpfs. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define AUDIO_TARGET "/tmp/plorpos-pico8-audio"       /* platform_h700.c's too */
#define AUDIO_NOW    "/tmp/plorpos-pico8-audio-now"   /* and this */
#define SDL_AUDIO_STOPPED 0                           /* SDL_GetAudioStatus */

static int  (*real_open)(audio_spec *, audio_spec *);
static void (*real_close)(void);
static void (*real_pause)(int);
static int  (*real_status)(void);
static audio_spec game_fmt;      /* what PICO-8 believes it has, quiet_cb in front */
static int  is_open, want_paused = 1, game_closed, watching;
static char cur[192];            /* the target that is open, or "" */
static char first_dev[160];      /* AUDIODEV at PICO-8's own open */
static int  first;               /* cur not known yet: adopt a matching target */
/* Around every open and close here and PICO-8's own lock, pause and close:
 * recursive, as SDL's audio lock is. */
static pthread_mutex_t mx = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

static void now_write(const char *v)
{
	FILE *f = fopen(AUDIO_NOW ".tmp", "w");

	if (!f) return;
	fputs(v, f);
	fclose(f);
	rename(AUDIO_NOW ".tmp", AUDIO_NOW);
}

static void reopen(const char *t)
{
	char dev[160];
	audio_spec spec = game_fmt;
	size_t n = strcspn(t, "\t\n");

	if (n >= sizeof dev) n = sizeof dev - 1;
	memcpy(dev, t, n);
	dev[n] = '\0';
	if (is_open) { real_close(); is_open = 0; }
	if (dev[0]) setenv("AUDIODEV", dev, 1);
	else unsetenv("AUDIODEV");
	if (real_open(&spec, NULL) == 0) {
		is_open = 1;
		real_pause(want_paused);
		snprintf(cur, sizeof cur, "%s", t);
		now_write(t);
	} else {
		cur[0] = '\0';          /* try again, see `wait` */
	}
}

static void *watch(void *u)
{
	char t[192];
	int wait = 0;

	(void)u;
	for (;;) {
		FILE *f;

		usleep(25000);
		if (!(f = fopen(AUDIO_TARGET, "r"))) continue;
		if (!fgets(t, sizeof t, f)) t[0] = '\0';
		fclose(f);
		if (!t[0]) continue;
		pthread_mutex_lock(&mx);
		if (game_closed) { pthread_mutex_unlock(&mx); break; }
		if (first) {
			/* The launcher's first word, normally the device PICO-8 was
			 * started on: take it as what is open, without a reopen. */
			size_t n = strcspn(t, "\t\n");
			first = 0;
			if (t[0] != '-' && n == strlen(first_dev) && !strncmp(t, first_dev, n)) {
				snprintf(cur, sizeof cur, "%s", t);
				now_write(t);
			}
		}
		if (t[0] == '-') {
			if (is_open) { real_close(); is_open = 0; }
			if (strcmp(cur, t)) { snprintf(cur, sizeof cur, "%s", t); now_write(t); }
		} else if (strcmp(t, cur) || !is_open ||
		           (real_status && real_status() == SDL_AUDIO_STOPPED)) {
			/* A device that will not open (busy, a headset still coming up)
			 * is tried every half second, not every 25 ms. */
			if (wait > 0) wait--;
			else { reopen(t); if (!cur[0]) wait = 20; }
		}
		pthread_mutex_unlock(&mx);
	}
	return NULL;
}

void SDL_PauseAudio(int on)
{
	if (!real_pause) real_pause = (void (*)(int))dlsym(RTLD_NEXT, "SDL_PauseAudio");
	pthread_mutex_lock(&mx);
	want_paused = on;
	if (is_open || !watching) real_pause(on);
	pthread_mutex_unlock(&mx);
}

void SDL_CloseAudio(void)
{
	if (!real_close) real_close = (void (*)(void))dlsym(RTLD_NEXT, "SDL_CloseAudio");
	pthread_mutex_lock(&mx);
	game_closed = 1;
	if (is_open || !watching) real_close();
	is_open = 0;
	pthread_mutex_unlock(&mx);
}

void SDL_LockAudio(void)
{
	static void (*real)(void);

	if (!real) real = (void (*)(void))dlsym(RTLD_NEXT, "SDL_LockAudio");
	pthread_mutex_lock(&mx);
	real();
}

void SDL_UnlockAudio(void)
{
	static void (*real)(void);

	if (!real) real = (void (*)(void))dlsym(RTLD_NEXT, "SDL_UnlockAudio");
	real();
	pthread_mutex_unlock(&mx);
}
#endif

/* PICO-8 plays through SDL_OpenAudio's callback: put quiet_cb in front of
 * it. SDL keeps its own copy of the spec, so the caller's is given back as
 * it was. */
int SDL_OpenAudio(audio_spec *desired, audio_spec *obtained)
{
	static int (*real)(audio_spec *, audio_spec *);
	int r;

	if (!real) real = (int (*)(audio_spec *, audio_spec *))dlsym(RTLD_NEXT, "SDL_OpenAudio");
	if (!real) return -1;
	if (!desired || !desired->callback) return real(desired, obtained);
	game_cb = desired->callback;
	silence = desired->format == AUDIO_U8 ? 0x80 : 0;
	desired->callback = quiet_cb;
	r = real(desired, obtained);
	desired->callback = game_cb;
	if (r == 0 && obtained)
		silence = obtained->format == AUDIO_U8 ? 0x80 : 0;
#ifdef PICO8_FOLLOW
	if (r == 0) {
		const char *d = getenv("AUDIODEV");
		pthread_t th;

		pthread_mutex_lock(&mx);
		game_fmt = obtained ? *obtained : *desired;
		game_fmt.callback = quiet_cb;
		game_fmt.userdata = desired->userdata;
		real_open   = real;
		if (!real_close)  real_close  = (void (*)(void))dlsym(RTLD_NEXT, "SDL_CloseAudio");
		if (!real_pause)  real_pause  = (void (*)(int))dlsym(RTLD_NEXT, "SDL_PauseAudio");
		if (!real_status) real_status = (int (*)(void))dlsym(RTLD_NEXT, "SDL_GetAudioStatus");
		snprintf(first_dev, sizeof first_dev, "%s", d ? d : "");
		is_open = first = 1;
		cur[0] = '\0';
		if (!watching && real_close && real_pause &&
		    pthread_create(&th, NULL, watch, NULL) == 0) {
			pthread_detach(th);
			watching = 1;
		}
		pthread_mutex_unlock(&mx);
	}
#endif
	return r;
}
