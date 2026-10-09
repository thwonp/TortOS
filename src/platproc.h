/* SPDX-License-Identifier: MIT */
#ifndef TORTOS_PLATPROC_H
#define TORTOS_PLATPROC_H
#include <stdbool.h>

/* The two platform calls that need no SDL, apart from platform.h so that code
 * using only these - src/musec.c - also builds for the RG Nano's SDL 1.2
 * frontend (src/nano/), where platform.h's SDL2 cannot be included. */

/* Milliseconds from some fixed point, for intervals. */
unsigned plat_now_ms(void);

/* Start a child and forget it: it outlives this process and leaves no zombie
 * behind. This is how a resident emulator that has died gets started again. */
bool plat_spawn_detached(char *const argv[], const char *const envkv[],
                         const char *workdir);
#endif
