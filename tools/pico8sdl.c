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
	return r;
}
