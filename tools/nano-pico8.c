/* SPDX-License-Identifier: MIT
 *
 * Preloaded into the owner's pico8_dyn on the RG Nano (plorpos-ggv.42.2),
 * which runs it on the glibc + SDL2 of mk/build-nano-pico8rt.sh:
 *
 *   PLORPOS_PICO8_EXE=<dir>/pico8_dyn SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dsp \
 *   pico8rt/ld-linux-armhf.so.3 --library-path pico8rt \
 *       --preload pico8rt/nano-pico8.so <dir>/pico8_dyn -home ... -run CART
 *
 * The Nano has no DRM and SDL2 no framebuffer driver, so SDL draws nowhere
 * and reads no keys (its dummy video; the evdev variant finds devices only
 * through udev, which FunKey-OS lacks), and this does the rest:
 *
 * - The screen. The display is said to be 240x240, whatever window or
 *   fullscreen config.txt asks for (PICO-8 went fullscreen on the dummy's
 *   1024x768 regardless). PICO-8 with blit_method 1 draws into the window
 *   surface, RGB888; each SDL_UpdateWindowSurface copies it to fb0's shown
 *   page as RGB565. The console is put in graphics mode with its keyboard
 *   off, as SDL 1.2 does, so it neither draws on the screen nor buffers the
 *   keys; nanoshelf's termfix_all puts it back after every game.
 * - The buttons, as fake08 has them on the Nano (picoarch's plat_funkey.c
 *   and fake08's libretro.cpp): B is O, A is X, START pauses. They are read
 *   from fkgpiod's keyboard, /dev/input/event0, whose letters (KEY_U,
 *   KEY_A, ...) become PICO-8's keys; the power key's tap (KEY_Q) opens
 *   the menu, below. Every other key is dropped, so FN combos and the letters
 *   PICO-8 would read as shortcuts do nothing.
 * - The power key's tap opens FunKey's menu, as in every other game: music,
 *   volume, brightness, exit, powerdown (plorpos-ggv.42.5). It is picoarch's
 *   own, run as `picoarch --menu` (PLORPOS_MENU names it) over the frame on
 *   the screen; PICO-8 waits meanwhile, its sound closed so the menu's music
 *   page can have the codec. Leaving it by Exit quits PICO-8.
 * - Where pico8.dat is. PICO-8 looks beside /proc/self/exe, which here is the
 *   glibc loader; PLORPOS_PICO8_EXE names the binary it should see instead.
 * - Muse. The Nano has one codec and no dmix, so the game's sound is closed
 *   while /tmp/plorpos/now says playing and opened again after, as picoarch
 *   does (its plorpos.c, plat_sound_set_quiet).
 * - FunKey's power key hold: powerdown signals the recorded pid with SIGUSR1
 *   and powers off a few seconds later; this quits PICO-8 first, so it
 *   writes its cart data.
 */
#define _GNU_SOURCE
#include <SDL.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NOW "/tmp/plorpos/now"   /* nanoshelf's; picoarch's plorpos.h too */

#define REAL(f) static __typeof__(f) *real_##f; \
	if (!real_##f) real_##f = (__typeof__(f) *)dlsym(RTLD_NEXT, #f)

/* ------------------------------------------------------------ pico8.dat */

ssize_t readlink(const char *path, char *buf, size_t n)
{
	const char *exe = getenv("PLORPOS_PICO8_EXE");
	REAL(readlink);

	if (exe && !strcmp(path, "/proc/self/exe")) {
		size_t l = strlen(exe);

		if (l > n) l = n;
		memcpy(buf, exe, l);
		return (ssize_t)l;
	}
	return real_readlink(path, buf, n);
}

/* ------------------------------------------------------------------ Muse */

static SDL_AudioSpec want;            /* what PICO-8 asked for */
static int asked, game_pause = 1, dev_open;

static int music_playing(void)
{
	char s[16] = "";
	FILE *f = fopen(NOW, "r");

	if (f) {
		if (!fgets(s, sizeof s, f)) s[0] = '\0';
		fclose(f);
	}
	return !strncmp(s, "playing\t", 8);
}

/* Opened with no obtained spec, so SDL converts to what PICO-8 asked for and
 * a reopen hands it the same format the first open did. SDL's OSS driver
 * lists /dev/dsp only if it could open it when audio started; while Muse
 * held the codec then, the list is empty and every open says "No such
 * audio device" - so a failed open starts SDL's audio over (SDL_AudioInit),
 * which lists again, and tries once more. */
static int audio_open(void)
{
	REAL(SDL_OpenAudio);
	REAL(SDL_PauseAudio);

	if (real_SDL_OpenAudio(&want, NULL) != 0) {
		int n = SDL_AudioInit(NULL) == 0 ? SDL_GetNumAudioDevices(0) : -1;

		if (real_SDL_OpenAudio(&want, NULL) != 0) {
			fprintf(stderr, "nano-pico8: relisted %d devices, open: %s\n", n, SDL_GetError());
			return -1;
		}
	}
	dev_open = 1;
	real_SDL_PauseAudio(game_pause);
	return 0;
}

int SDL_OpenAudio(SDL_AudioSpec *d, SDL_AudioSpec *o)
{
	want = *d;
	asked = 1;
	if (o) {
		*o = *d;
		o->silence = d->format == AUDIO_U8 ? 0x80 : 0;
		o->size = (Uint32)d->samples * d->channels * (SDL_AUDIO_BITSIZE(d->format) / 8);
	}
	/* Quiet from the start while music plays; PICO-8 never knows. */
	return music_playing() ? 0 : audio_open();
}

void SDL_PauseAudio(int on)
{
	REAL(SDL_PauseAudio);

	game_pause = on;
	if (dev_open) real_SDL_PauseAudio(on);
}

static struct timespec follow_next;    /* zeroed: follow Muse on the next frame */

static void audio_follow_muse(void)
{
	struct timespec next = follow_next;
	struct timespec t;
	REAL(SDL_CloseAudio);

	if (!asked) return;
	clock_gettime(CLOCK_MONOTONIC, &t);
	if (t.tv_sec < next.tv_sec || (t.tv_sec == next.tv_sec && t.tv_nsec < next.tv_nsec)) return;
	next = t;
	next.tv_nsec += 500000000;
	if (next.tv_nsec >= 1000000000) { next.tv_sec++; next.tv_nsec -= 1000000000; }
	follow_next = next;

	if (music_playing()) {
		if (dev_open) {
			real_SDL_CloseAudio();
			dev_open = 0;
			fprintf(stderr, "nano-pico8: music playing, game sound closed\n");
		}
	} else if (!dev_open) {
		static int failing;

		if (audio_open() == 0) {
			fprintf(stderr, "nano-pico8: game sound open again\n");
			failing = 0;
		} else if (!failing) {
			/* Once, not every 500 ms while it keeps failing. */
			fprintf(stderr, "nano-pico8: game sound will not open yet: %s\n", SDL_GetError());
			failing = 1;
		}
	}
}

/* ---------------------------------------------------------------- screen */

static int fb = -1;
static uint16_t *fbmem;
static unsigned stride;               /* in pixels */
static SDL_Window *win;

static void fb_open(void)
{
	struct fb_fix_screeninfo fix;
	struct fb_var_screeninfo var;
	void *m;

	fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	if (fb < 0 || ioctl(fb, FBIOGET_FSCREENINFO, &fix) != 0 ||
	    ioctl(fb, FBIOGET_VSCREENINFO, &var) != 0 || var.bits_per_pixel != 16) {
		fprintf(stderr, "nano-pico8: no 16 bpp fb0\n");
		return;
	}
	m = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
	if (m == MAP_FAILED) return;
	fbmem = m;
	stride = fix.line_length / 2;
}

static long long now_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}

/* PICO-8's pause menu presents as fast as it can - 113 fps, half the CPU -
 * where a real display's vsync would hold it to 60. A present less than
 * 12 ms after the last (only an unpaced loop does that; 60 fps is 16.7)
 * waits out the rest of a 60 Hz frame. */
static void pace(void)
{
	static long long last;
	long long t = now_us();

	if (last && t - last < 12000) {
		usleep((useconds_t)(16667 - (t - last)));
		t = now_us();
	}
	last = t;
}

/* The window surface to fb0's shown page - asked each time: one ioctl, and
 * whoever drew last may have panned. */
static void show(void)
{
	struct fb_var_screeninfo var;
	SDL_Surface *s = win ? SDL_GetWindowSurface(win) : NULL;

	if (fb < 0) fb_open();
	if (fbmem && s && s->format->BytesPerPixel == 4 &&
	    ioctl(fb, FBIOGET_VSCREENINFO, &var) == 0) {
		int w2 = s->w < (int)var.xres ? s->w : (int)var.xres;
		int h2 = s->h < (int)var.yres ? s->h : (int)var.yres;
		int x, y;

		for (y = 0; y < h2; y++) {
			const uint32_t *src = (const uint32_t *)((const uint8_t *)s->pixels + y * s->pitch);
			uint16_t *dst = fbmem + (var.yoffset + y) * stride + var.xoffset;

			for (x = 0; x < w2; x++) {
				uint32_t p = src[x];

				dst[x] = (uint16_t)(((p >> 8) & 0xf800) | ((p >> 5) & 0x07e0) | ((p >> 3) & 0x001f));
			}
		}
	}
}

int SDL_UpdateWindowSurface(SDL_Window *w)
{
	REAL(SDL_UpdateWindowSurface);

	win = w;
	audio_follow_muse();
	show();
	pace();
	return real_SDL_UpdateWindowSurface(w);
}

/* ------------------------------------------------------------- display */

static void say_240(SDL_DisplayMode *m) { m->w = 240; m->h = 240; }

int SDL_GetDesktopDisplayMode(int i, SDL_DisplayMode *m)
{
	REAL(SDL_GetDesktopDisplayMode);
	int r = real_SDL_GetDesktopDisplayMode(i, m);

	if (r == 0) say_240(m);
	return r;
}

int SDL_GetCurrentDisplayMode(int i, SDL_DisplayMode *m)
{
	REAL(SDL_GetCurrentDisplayMode);
	int r = real_SDL_GetCurrentDisplayMode(i, m);

	if (r == 0) say_240(m);
	return r;
}

int SDL_GetDisplayMode(int i, int k, SDL_DisplayMode *m)
{
	REAL(SDL_GetDisplayMode);
	int r = real_SDL_GetDisplayMode(i, k, m);

	if (r == 0) say_240(m);
	return r;
}

int SDL_GetDisplayBounds(int i, SDL_Rect *b)
{
	REAL(SDL_GetDisplayBounds);
	int r = real_SDL_GetDisplayBounds(i, b);

	if (r == 0) { b->x = b->y = 0; b->w = b->h = 240; }
	return r;
}

/* -------------------------------------------------------------- buttons */

#define MENU SDL_NUM_SCANCODES   /* not a key: the power tap */

static SDL_Scancode map(unsigned code)
{
	switch (code) {
	case KEY_U: return SDL_SCANCODE_UP;
	case KEY_D: return SDL_SCANCODE_DOWN;
	case KEY_L: return SDL_SCANCODE_LEFT;
	case KEY_R: return SDL_SCANCODE_RIGHT;
	case KEY_B: return SDL_SCANCODE_Z;        /* O */
	case KEY_A: return SDL_SCANCODE_X;        /* X */
	case KEY_S: return SDL_SCANCODE_RETURN;   /* pause */
	case KEY_Q: return (SDL_Scancode)MENU;
	default:    return SDL_SCANCODE_UNKNOWN;
	}
}

static Uint8 keys[SDL_NUM_SCANCODES];
static int kbd = -2;                  /* -2: not opened yet */
static volatile sig_atomic_t quit;

const Uint8 *SDL_GetKeyboardState(int *n)
{
	if (n) *n = SDL_NUM_SCANCODES;
	return keys;
}

/* The dummy window never gets focus, and PICO-8 reads no keys without it. */
SDL_Window *SDL_GetKeyboardFocus(void)
{
	REAL(SDL_GetKeyboardFocus);
	SDL_Window *f = real_SDL_GetKeyboardFocus();

	return f ? f : win;
}

static void console_set(int kd)
{
	int tty = open("/dev/tty0", O_RDWR | O_CLOEXEC);

	if (tty >= 0) {
		ioctl(tty, KDSETMODE, kd);
		if (kd == KD_GRAPHICS) ioctl(tty, KDSKBMODE, K_OFF);
		close(tty);
	}
}

static void console_off(void) { console_set(KD_GRAPHICS); }

static void on_usr1(int sig) { (void)sig; quit = 1; }

__attribute__((constructor)) static void init(void)
{
	signal(SIGUSR1, on_usr1);
	console_off();
}

extern char **environ;

/* FunKey's menu, with PICO-8 waiting: 1 when it says to leave the game.
 * Its environment is PICO-8's less the SDL 2 driver names, which SDL 1.2
 * reads too and would put the menu on its dummy driver. Built before the
 * fork: nothing that allocates runs between fork and exec. */
static int run_menu(void)
{
	static char *env[256];
	const char *bin = getenv("PLORPOS_MENU");
	struct input_event ev;
	int st = 0, n = 0, i;
	pid_t pid;
	REAL(SDL_CloseAudio);

	if (!bin) return 0;
	for (i = 0; environ[i] && n < 255; i++)
		if (strncmp(environ[i], "SDL_VIDEODRIVER=", 16) &&
		    strncmp(environ[i], "SDL_AUDIODRIVER=", 16))
			env[n++] = environ[i];
	env[n] = NULL;
	/* The kernel will not switch away from a console in graphics mode, and
	 * SDL 1.2 moves to a console of its own: the menu waited for it forever
	 * on a black screen. Text mode for the menu, graphics again after - and
	 * the frame again, which text mode blanked, for the menu to draw over. */
	console_set(KD_TEXT);
	show();
	if (dev_open) {
		real_SDL_CloseAudio();
		dev_open = 0;
	}
	pid = fork();
	if (pid == 0) {
		execle(bin, "picoarch", "--menu", (char *)NULL, env);
		_exit(127);
	}
	if (pid > 0) while (waitpid(pid, &st, 0) < 0 && errno == EINTR) ;
	console_off();                       /* SDL 1.2 put back what it found, but surely */
	while (kbd >= 0 && read(kbd, &ev, sizeof ev) > 0) ;   /* the menu's keys */
	memset(keys, 0, sizeof keys);
	memset(&follow_next, 0, sizeof follow_next);           /* sound back now if it may */
	fprintf(stderr, "nano-pico8: menu, exit %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
	return pid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 1;
}

/* One button from event0 as an SDL event, or 0 when none is waiting. The
 * kernel's input_event matches glibc armhf's: a 32-bit timeval. */
static int key_event(SDL_Event *e)
{
	struct input_event ev;

	if (kbd == -2) {
		kbd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (kbd < 0) fprintf(stderr, "nano-pico8: no /dev/input/event0\n");
	}
	while (kbd >= 0 && read(kbd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
		SDL_Scancode to;

		if (ev.type != EV_KEY || ev.value == 2) continue;   /* no autorepeat */
		to = map(ev.code);
		if (to == (SDL_Scancode)MENU) {
			if (ev.value && run_menu()) quit = 1;
			continue;
		}
		if (to == SDL_SCANCODE_UNKNOWN) continue;
		keys[to] = ev.value ? 1 : 0;
		memset(e, 0, sizeof *e);
		e->type = ev.value ? SDL_KEYDOWN : SDL_KEYUP;
		e->key.state = ev.value ? SDL_PRESSED : SDL_RELEASED;
		e->key.windowID = win ? SDL_GetWindowID(win) : 0;
		e->key.keysym.scancode = to;
		e->key.keysym.sym = SDL_GetKeyFromScancode(to);
		return 1;
	}
	return 0;
}

int SDL_PollEvent(SDL_Event *e)
{
	REAL(SDL_PollEvent);

	if (!e) return real_SDL_PollEvent(e);
	if (quit) {
		memset(e, 0, sizeof *e);
		e->type = SDL_QUIT;
		return 1;
	}
	return key_event(e) || real_SDL_PollEvent(e);
}
