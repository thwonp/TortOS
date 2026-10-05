/* SPDX-License-Identifier: MIT
 *
 * Preloaded into the owner's pico8_64 on the TrimUI Brick (plorpos-gkd.50.13).
 * PICO-8 asks SDL_Init for every subsystem, sensors included, and the Brick's
 * firmware SDL2 was built without them: "SDL not built with sensor support",
 * and PICO-8 stops with "Unable to initialize SDL". The firmware SDL stays -
 * it is the one with the display backend - and this takes the one flag it
 * cannot honour off the request. PICO-8 reads no sensor.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>

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
