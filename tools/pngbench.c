/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* What one image actually costs the device to decode.
 *
 * Built for a question that took four wrong answers first: whether shrinking
 * box art made it faster to decode. Timings read out of the running launcher
 * could not answer it - the decode there competes with a 60fps render loop and
 * whatever else is running, the same file measured twice differed by 18%, and
 * comparing different covers across different sessions produced noise that
 * looked like a result. This decodes the files you name, with nothing else
 * happening, so two of them can be compared honestly.
 *
 * Build and run it on the device:
 *
 *   docker run --rm -v "$PWD":/work -w /work tortos-toolchain sh -c \
 *     'aarch64-linux-gnu-gcc -O2 -std=gnu11 -I/work/sysroot/usr/include/SDL2 \
 *      -D_GNU_SOURCE -L/work/sysroot/usr/lib -Wl,-rpath-link,/work/sysroot/usr/lib \
 *      -o /work/build/pngbench /work/tools/pngbench.c -lSDL2 -lSDL2_image'
 *   adb push build/pngbench /tmp/ && adb shell \
 *     'LD_LIBRARY_PATH=/usr/trimui/lib:/usr/lib /tmp/pngbench <files...>'
 *
 * `cold` is the first read, `warm` the fastest of the rest, so the gap between
 * them is the card rather than the CPU. `conv` is the cost of getting to
 * ARGB8888, which is what the launcher actually draws from, and `fmt` is what
 * the file decoded to - an RGBA cover pays that conversion and an RGB one
 * mostly does not. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <SDL.h>
#include <SDL_image.h>

int main(int argc, char **argv)
{
	int i, k, reps = 6;
	SDL_Init(0);
	IMG_Init(IMG_INIT_PNG);
	printf("%-42s %8s %9s %5s %5s %5s %5s\n",
	       "file", "bytes", "WxH", "cold", "warm", "conv", "fmt");
	for (i = 1; i < argc; i++) {
		struct stat st;
		Uint32 t0, t1, t2;
		int cold = -1, warm = 9999, conv = 9999, w = 0, h = 0;
		const char *fmt = "?";
		if (stat(argv[i], &st) != 0) { printf("  missing %s\n", argv[i]); continue; }
		for (k = 0; k < reps; k++) {
			SDL_Surface *s, *c;
			t0 = SDL_GetTicks();
			s = IMG_Load(argv[i]);
			t1 = SDL_GetTicks();
			if (!s) { printf("  unreadable %s\n", argv[i]); break; }
			w = s->w; h = s->h;
			fmt = SDL_GetPixelFormatName(s->format->format);
			c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ARGB8888, 0);
			t2 = SDL_GetTicks();
			if (k == 0) cold = (int)(t1 - t0);
			else if ((int)(t1 - t0) < warm) warm = (int)(t1 - t0);
			if ((int)(t2 - t1) < conv) conv = (int)(t2 - t1);
			if (c) SDL_FreeSurface(c);
			SDL_FreeSurface(s);
		}
		printf("%-42.42s %8lld %4dx%-4d %5d %5d %5d  %s\n",
		       strrchr(argv[i],'/') ? strrchr(argv[i],'/')+1 : argv[i],
		       (long long)st.st_size, w, h, cold, warm, conv,
		       fmt ? fmt + 16 : "?");
	}
	return 0;
}
