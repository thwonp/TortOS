/* SPDX-License-Identifier: MIT */
/* Shrinking fetched box art to the size it is actually drawn at.
 *
 * Its own file rather than part of artscrape.c, for two reasons. artscrape is
 * about naming and fetching and has no other business with pixels; and it is
 * compiled standalone by check-artscrape, which is an offline check that must
 * not need SDL to build. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <SDL.h>
#include <SDL_image.h>
#include "artshrink.h"

#define ARTSHRINK_PATH_MAX 2048


/* The longest edge a cover is ever drawn at, plus nothing.
 *
 * The shelf card is 332x461, and equal-area sizing can take a landscape cover
 * to 464 wide, so 512 covers the largest draw with a little spare. The launch
 * zoom passes that, but it is a 200ms accelerating fade and softness there is
 * not visible. */
#define ART_MAX_PX 512

/* Shrink a freshly fetched cover to the size it is actually drawn at.
 *
 * libretro ships art at its own resolution and TortOS never had an opinion
 * about it. Measured on this card 2026-09-12: NES covers arrive 512x731 and
 * Genesis 479x680, against a card that draws at most 332x461 - two to three
 * times the pixels that can ever reach the screen. That is not free, because
 * game_get_tex decodes on the render path: 7-27ms per card on this device
 * against a 16.7ms frame, every time one scrolls past.
 *
 * Never upscales. Art already inside the box is left exactly as it arrived.
 *
 * Area-average rather than a 2x2 bilinear. At these ratios bilinear samples
 * too few source pixels and softens exactly what a box cover is made of -
 * text, logos and hard edges. Averaging straight RGBA is only correct while
 * alpha is constant, which for box art it is: of 314 SNES covers sampled,
 * none carried a meaningful alpha channel.
 *
 * Written to a temporary and renamed, so a power cut during the encode leaves
 * the original rather than half a file. */
static void shrink_now(const char *path)
{
	SDL_Surface *raw, *src, *dst;
	char tmp[ARTSHRINK_PATH_MAX];
	Uint32 t0 = SDL_GetTicks();
	int tw, th, x, y;
	double k, sx, sy;

	if (!(raw = IMG_Load(path))) return;
	if (raw->w <= ART_MAX_PX && raw->h <= ART_MAX_PX) {
		SDL_FreeSurface(raw);
		return;
	}
	k  = (double)ART_MAX_PX / (raw->w > raw->h ? raw->w : raw->h);
	tw = (int)(raw->w * k + 0.5); if (tw < 1) tw = 1;
	th = (int)(raw->h * k + 0.5); if (th < 1) th = 1;

	src = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_ARGB8888, 0);
	SDL_FreeSurface(raw);
	if (!src) return;
	dst = SDL_CreateRGBSurfaceWithFormat(0, tw, th, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!dst) { SDL_FreeSurface(src); return; }

	sx = (double)src->w / tw;
	sy = (double)src->h / th;
	for (y = 0; y < th; y++) {
		Uint32 *drow = (Uint32 *)((Uint8 *)dst->pixels + y * dst->pitch);
		int y0 = (int)(y * sy), y1 = (int)((y + 1) * sy);

		if (y1 <= y0) y1 = y0 + 1;
		if (y1 > src->h) y1 = src->h;
		for (x = 0; x < tw; x++) {
			unsigned a = 0, rr = 0, gg = 0, bb = 0, n = 0;
			int x0 = (int)(x * sx), x1 = (int)((x + 1) * sx), xx, yy;

			if (x1 <= x0) x1 = x0 + 1;
			if (x1 > src->w) x1 = src->w;
			for (yy = y0; yy < y1; yy++) {
				const Uint32 *srow = (const Uint32 *)
					((Uint8 *)src->pixels + yy * src->pitch);
				for (xx = x0; xx < x1; xx++) {
					Uint32 px = srow[xx];
					a  += (px >> 24) & 0xFF;
					rr += (px >> 16) & 0xFF;
					gg += (px >>  8) & 0xFF;
					bb +=  px        & 0xFF;
					n++;
				}
			}
			drow[x] = n ? ((a / n) << 24 | (rr / n) << 16 |
			               (gg / n) << 8 | (bb / n)) : 0;
		}
	}

	/* THE OUTPUT IS RGBA AND CANNOT BE MADE RGB HERE. Every cover libretro
	 * ships as RGB gains an alpha plane of constant 255 on the way through,
	 * and that is worth about 20% of every later decode: measured on the
	 * device 2026-09-12, the same cover is 14ms as RGBA and 11ms as RGB, plus
	 * a millisecond of conversion the RGB one does not pay.
	 *
	 * It is IMG_SavePNG, not this function. It converts whatever it is handed
	 * to RGBA32 before writing, with no flag to stop it; handing it an RGB24
	 * surface and the original RGB24 image produced byte-identical RGBA files.
	 * Getting RGB out means writing the PNG here instead - chunk framing, CRCs
	 * and adaptive row filtering, against a zlib the sysroot does not carry
	 * yet. Deliberately not done: a hand-rolled encoder that is subtly wrong
	 * corrupts art silently, and decoding has since moved off the render
	 * thread (src/texload.c), so 3ms of decode is no longer what anyone feels. */
	if (snprintf(tmp, sizeof tmp, "%s.tmp", path) < (int)sizeof tmp &&
	    IMG_SavePNG(dst, tmp) == 0) {
		fprintf(stderr, "art: %dx%d -> %dx%d %ums %s\n",
		        src->w, src->h, tw, th, SDL_GetTicks() - t0,
		        strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
		if (rename(tmp, path) != 0) unlink(tmp);
	} else {
		unlink(tmp);
	}
	SDL_FreeSurface(dst);
	SDL_FreeSurface(src);
}


/* ---------------------------------------------------------- the thread --- */

/* Queue and return; one thread does the resizing, one cover at a time.
 *
 * Off the caller either way. The caller is the Box Art screen's frame loop,
 * which is where the power button is read, so nothing there may wait. A resize
 * that blocked would also stall the next download, because there is ONE async
 * slot - the link would sit idle through every resize and a full run would cost
 * downloads PLUS resizes rather than the larger of the two.
 *
 * THIS USED TO FORK, AND THE FORK COULD HANG FOREVER. It was a double fork, so
 * the grandchild did the work and init reaped it, and that was correct when it
 * was written: the launcher had no threads. src/texload.c added two decode
 * workers an hour later, which made it a fork from a multithreaded process. A
 * child gets only the thread that forked, and any lock another thread held at
 * that instant stays held with nothing left to release it.
 *
 * glibc covers malloc across a fork. It does not cover SDL. SDL2 2.30.8 guards
 * its pixel-format list with a spinlock, formats_lock in SDL_pixels.c, taken by
 * surface creation, SDL_ConvertSurfaceFormat and SDL_FreeSurface - every decode
 * the workers do, and every step below. If a worker held it at the fork, the
 * child's first surface call would spin in SDL_AtomicLock, which has no timeout
 * (its source says so, in a FIXME), and the orphan would busy-yield a core for
 * as long as the device stayed on. Never observed across 1,599 resizes, because
 * scrapes run while the workers are mostly idle. Rare, not impossible.
 *
 * A thread has none of that. The spinlock that can deadlock a forked child is
 * exactly what makes those same calls safe to make concurrently - the decode
 * workers already do IMG_Load and surface conversion alongside the main thread.
 *
 * What a thread gives up is outliving the launcher. A forked child survived a
 * killall of its parent; a thread does not. Covers queued but not yet resized
 * when the launcher exits stay full size, which is the one failure this file
 * already accepts, and shrink_now's temporary-and-rename still means an exit
 * mid-write leaves the original rather than half a PNG.
 *
 * Started on the first cover, not at launch: most sessions never fetch art,
 * and those should not carry an idle thread. */
struct job {
	struct job *next;
	char        path[];
};

static SDL_Thread *g_thread;
static SDL_mutex  *g_lock;
static SDL_cond   *g_wake;
static bool        g_stop;
static struct job *g_head, *g_tail;

static int worker(void *unused)
{
	(void)unused;
	SDL_LockMutex(g_lock);
	for (;;) {
		struct job *j;

		while (!g_stop && !g_head) SDL_CondWait(g_wake, g_lock);
		if (g_stop) break;
		j = g_head;
		g_head = j->next;
		if (!g_head) g_tail = NULL;
		SDL_UnlockMutex(g_lock);

		shrink_now(j->path);
		free(j);

		SDL_LockMutex(g_lock);
	}
	SDL_UnlockMutex(g_lock);
	return 0;
}

static bool started(void)
{
	if (g_thread) return true;
	if (!(g_lock = SDL_CreateMutex())) return false;
	if (!(g_wake = SDL_CreateCond())) {
		SDL_DestroyMutex(g_lock);
		g_lock = NULL;
		return false;
	}
	g_stop = false;
	if (!(g_thread = SDL_CreateThread(worker, "tortos-shrink", NULL))) {
		SDL_DestroyCond(g_wake);   g_wake = NULL;
		SDL_DestroyMutex(g_lock);  g_lock = NULL;
		return false;
	}
	return true;
}

void art_shrink(const char *path)
{
	size_t      n;
	struct job *j;

	if (!path) return;
	/* No thread to be had: do it here rather than not at all. It stalls the
	 * frame, but a launcher that cannot start a thread cannot decode card art
	 * off the frame either, and has larger problems than one slow cover. */
	if (!started()) { shrink_now(path); return; }

	n = strlen(path) + 1;
	if (!(j = malloc(sizeof *j + n))) return;   /* best effort: stays full size */
	j->next = NULL;
	memcpy(j->path, path, n);

	SDL_LockMutex(g_lock);
	if (g_tail) g_tail->next = j;
	else        g_head = j;
	g_tail = j;
	SDL_CondSignal(g_wake);
	SDL_UnlockMutex(g_lock);
}

void art_shrink_stop(void)
{
	struct job *j;

	if (!g_thread) return;
	SDL_LockMutex(g_lock);
	g_stop = true;
	SDL_CondSignal(g_wake);
	SDL_UnlockMutex(g_lock);
	/* Waits for the cover in hand, at most one resize, and no more. */
	SDL_WaitThread(g_thread, NULL);
	g_thread = NULL;

	while ((j = g_head)) {
		g_head = j->next;
		free(j);
	}
	g_tail = NULL;
	SDL_DestroyCond(g_wake);   g_wake = NULL;
	SDL_DestroyMutex(g_lock);  g_lock = NULL;
}
