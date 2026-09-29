/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The GKD 350H Ultra on vendor ROCKNIX, under sway: the device file for
 * PLATFORM=gkd, as platform_brick.c is for the Brick.
 *
 * Video, input and paths are real (plorpos-gkd.3.3), levels and battery too
 * (gkd.3.4). The rest are stubs answering "not here" until their tasks:
 * power and sleep later. diatom's port/gkd.c is the reference for all of it - same
 * window setup, same buttons - so the launcher and a game agree on the
 * device. */
#include "db.h"
#include "platform.h"
#include "platform_dev.h"

#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

/* Where everything is, unless TORTOS_* says otherwise - see paths_init(). The
 * SD card, as ROCKNIX mounts it; userdata per device, as on the Brick. */
const char *P_ROOT = "/storage/games-external/TortOS";
const char *P_CARD = "/storage/games-external";
const char *P_ROMS = "/storage/games-external/Roms";
const char *P_USERDATA = "/storage/games-external/.userdata/gkd";
const char *P_SHARED = "/storage/games-external/.userdata/shared";
const char *P_WEB = "/storage/games-external/TortOS/res/web";
const char *P_FONT = "/storage/games-external/TortOS/menu.ttf";

/* What a game's process is told about the device - see build_child_env(). */
const char *const plat_child_env[] = {
	"PLATFORM=gkd350h", "DEVICE=gkd350h",
	"SDCARD_PATH=/storage/games-external",
	"BIOS_PATH=/storage/games-external/Bios",
	"CHEATS_PATH=/storage/games-external/Cheats",
	"SAVES_PATH=/storage/games-external/Saves",
	NULL
};
const char plat_child_libpath[] = "";

bool plat_is_brick_pro(void) { return false; }
/* Home turns the volume keys into brightness keys, as it does in a game. */
const char *plat_bright_keys(void) { return "Home+Vol"; }

/* ---- video ------------------------------------------------------------
 *
 * A fullscreen Wayland window under sway, which rotates the portrait panel
 * (transform 270) to 1600x1440. The launcher still draws 1024x768 and SDL
 * scales it up (1.5625, letterboxed 120 px top and bottom) - interim, until
 * the layout is native (gkd.11). */
static SDL_Window *win;
static SDL_Renderer *ren;

SDL_Renderer *plat_renderer(void) { return ren; }

bool plat_video_init(void)
{
	int w = 0, h = 0;

	/* Without it the Mali driver hands sway AFBC-compressed buffers and the
	 * window composites as solid black (diatom, 2026-09-29). */
	setenv("MALI_WAYLAND_AFBC", "0", 0);
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
	SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
	if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "video init: %s\n", SDL_GetError());
		return false;
	}
	SDL_ShowCursor(SDL_DISABLE);
	/* At the output's own size: asking for 0x0 leaves SDL's Wayland backend
	 * a 1x1 window that fullscreen never grows (diatom, 2026-09-29). */
	{
		SDL_DisplayMode dm = { 0 };
		if (SDL_GetDesktopDisplayMode(0, &dm) != 0) { dm.w = 0; dm.h = 0; }
		win = SDL_CreateWindow("TortOS", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		                       dm.w, dm.h, SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN);
	}
	if (!win) {
		fprintf(stderr, "window: %s\n", SDL_GetError());
		return false;
	}
	ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!ren) {
		fprintf(stderr, "renderer: %s\n", SDL_GetError());
		return false;
	}
	SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
	/* sway sizes the window in its first configure, which arrives after
	 * SDL_CreateWindow returns. Bounded, so a compositor that never answers
	 * fails visibly rather than hanging. */
	for (int i = 0; i < 100; i++) {
		SDL_PumpEvents();
		SDL_GetRendererOutputSize(ren, &w, &h);
		if (w > 1 && h > 1) break;
		SDL_Delay(10);
	}
	SDL_RenderSetLogicalSize(ren, TORTOS_SCREEN_W, TORTOS_SCREEN_H);
	/* The same two lines as the Brick's, for the same reasons: vsync is a
	 * request, and the cube needs render targets. */
	SDL_RendererInfo info;
	if (SDL_GetRendererInfo(ren, &info) == 0) {
		fprintf(stderr, "renderer: %s, driver: %s, output %dx%d, vsync %s\n",
		        info.name, SDL_GetCurrentVideoDriver(), w, h,
		        (info.flags & SDL_RENDERER_PRESENTVSYNC) ? "granted"
		                                                 : "DECLINED (requested)");
		fprintf(stderr, "renderer: render-to-texture %s\n",
		        (info.flags & SDL_RENDERER_TARGETTEXTURE) ? "available"
		                                                  : "MISSING");
	}
	return true;
}

void plat_video_quit(void)
{
	if (ren) { SDL_DestroyRenderer(ren); ren = NULL; }
	if (win) { SDL_DestroyWindow(win); win = NULL; }
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

/* ---- input ------------------------------------------------------------
 *
 * Straight from evdev, found by name: the pad (gkd_atom_joypad) and the
 * volume keys (gpio-keys). No SDL joystick, and no EVIOCGRAB - a game's
 * diatom reads the same nodes the same way. The map is diatom's padmap: the
 * face buttons are NOT crossed as on the Brick (BTN_EAST is the A printed on
 * the shell), MODE is Menu, the stick is a second d-pad, and Home is only a
 * modifier - held, it turns the volume keys into brightness keys. L2/R2 and
 * the stick click mean nothing on the shelf. */
static int fd_pad = -1, fd_keys = -1;
static bool home_down;
/* The d-pad and the stick each keep their own state, so letting go of one
 * does not release a direction the other still holds. Left, right, up, down -
 * IN_LEFT..IN_DOWN's order. */
static bool pad_dir[4], stick_dir[4];

static const struct { int code; in_button b; } padmap[] = {
	{ BTN_EAST,   IN_ACCEPT }, { BTN_SOUTH,  IN_BACK },
	{ BTN_NORTH,  IN_X      }, { BTN_WEST,   IN_Y    },
	{ BTN_TL,     IN_L1     }, { BTN_TR,     IN_R1   },
	{ BTN_SELECT, IN_SELECT }, { BTN_START,  IN_START },
	{ BTN_MODE,   IN_MENU   },
};

/* Range -900..899, right = +X, up = -Y. Half travel to press, a third to let
 * go, as on the Brick: the gap stops a stick resting near the line from
 * chattering. */
#define STICK_PRESS   450
#define STICK_RELEASE 300

/* Found by name, not by number: the numbering is probe order. */
static int evdev_open(const char *want)
{
	char path[32], name[64];

	for (int i = 0; i < 32; i++) {
		snprintf(path, sizeof path, "/dev/input/event%d", i);
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) continue;
		if (ioctl(fd, EVIOCGNAME(sizeof name), name) >= 0 && !strcmp(name, want))
			return fd;
		close(fd);
	}
	fprintf(stderr, "input: %s not found\n", want);
	return -1;
}

bool plat_input_init(void)
{
	if (fd_pad < 0)  fd_pad  = evdev_open("gkd_atom_joypad");
	if (fd_keys < 0) fd_keys = evdev_open("gpio-keys");
	return true;
}

static void drain(int fd)
{
	struct input_event ev;
	while (fd >= 0 && read(fd, &ev, sizeof ev) == (ssize_t)sizeof ev) { }
}

void plat_input_flush(void)
{
	SDL_Event e;
	SDL_PumpEvents();
	while (SDL_PollEvent(&e)) { }
	drain(fd_pad);
	drain(fd_keys);
	/* Their releases were in what was just thrown away. */
	memset(pad_dir, 0, sizeof pad_dir);
	memset(stick_dir, 0, sizeof stick_dir);
	home_down = false;
}

void plat_input_quit(void)
{
	if (fd_pad >= 0)  { close(fd_pad);  fd_pad = -1; }
	if (fd_keys >= 0) { close(fd_keys); fd_keys = -1; }
}

static void stick_axis(bool *neg_pos, int v)
{
	neg_pos[0] = neg_pos[0] ? v <= -STICK_RELEASE : v <= -STICK_PRESS;
	neg_pos[1] = neg_pos[1] ? v >=  STICK_RELEASE : v >=  STICK_PRESS;
}

static void pad_read(in_state *st)
{
	struct input_event ev;

	while (fd_pad >= 0 && read(fd_pad, &ev, sizeof ev) == (ssize_t)sizeof ev) {
		if (ev.type == EV_ABS) {
			if (ev.code == ABS_X) stick_axis(&stick_dir[0], ev.value);
			if (ev.code == ABS_Y) stick_axis(&stick_dir[2], ev.value);
			continue;
		}
		if (ev.type != EV_KEY || ev.value == 2) continue;
		bool down = ev.value == 1;
		switch (ev.code) {
		case BTN_DPAD_LEFT:  pad_dir[0] = down; continue;
		case BTN_DPAD_RIGHT: pad_dir[1] = down; continue;
		case BTN_DPAD_UP:    pad_dir[2] = down; continue;
		case BTN_DPAD_DOWN:  pad_dir[3] = down; continue;
		case BTN_TRIGGER_HAPPY1: home_down = down; continue;
		}
		for (size_t k = 0; k < sizeof padmap / sizeof padmap[0]; k++)
			if (padmap[k].code == ev.code) set_btn(st, padmap[k].b, down);
	}
	for (int i = 0; i < 4; i++)
		set_btn(st, (in_button)(IN_LEFT + i), pad_dir[i] || stick_dir[i]);
}

/* A press goes to volume or brightness by whether Home is held at the time;
 * a release lets go of both, so letting go of Home first strands nothing. */
static void keys_read(in_state *st)
{
	struct input_event ev;

	while (fd_keys >= 0 && read(fd_keys, &ev, sizeof ev) == (ssize_t)sizeof ev) {
		if (ev.type != EV_KEY || ev.value == 2) continue;
		in_button vol, bright;
		if      (ev.code == KEY_VOLUMEUP)   { vol = IN_VOLUP; bright = IN_BRIGHTUP; }
		else if (ev.code == KEY_VOLUMEDOWN) { vol = IN_VOLDN; bright = IN_BRIGHTDN; }
		else continue;
		if (ev.value == 1) set_btn(st, home_down ? bright : vol, true);
		else { set_btn(st, vol, false); set_btn(st, bright, false); }
	}
}

void plat_input_poll(in_state *st)
{
	memset(st->pressed, 0, sizeof st->pressed);
	if (g_terminating) st->quit_requested = true;
	SDL_Event e;
	while (SDL_PollEvent(&e))
		if (e.type == SDL_QUIT) st->quit_requested = true;
	pad_read(st);
	keys_read(st);
}

void plat_leds_off(void) { }

/* ---- levels ---- */

/* diatom's ladder (port/gkd.c), so a level means the same panel on both
 * sides of a handover: this backlight is non-linear already, raw 40 is the
 * first step that reads as distinct, and above it every 8 units are visible -
 * so twelve rungs evenly spaced from 40 to 255. */
static const unsigned char bright_ladder[] = {
	40, 60, 79, 99, 118, 138, 157, 177, 196, 216, 235, 255
};
#define BRIGHT_MAX ((int)(sizeof bright_ladder / sizeof bright_ladder[0]) - 1)
_Static_assert(BRIGHT_MAX == PLAT_BRIGHT_MAX, "ladder and platform.h disagree");
#define VOL_MAX PLAT_VOL_MAX
#define BACKLIGHT "/sys/class/backlight/backlight/brightness"

/* disp_fd is the backlight's brightness file, kept open. There is no mixer
 * device - volume is PipeWire's - so mixer_fd is 0 once the worker below is
 * running: "present", so the nudges run. */
int mixer_fd = -1, disp_fd = -1;
int cur_vol = -1, cur_bright = -1;
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* As on the Brick: the player's levels in the device db, and boot.env
 * following them so a launch script can light the panel before we run. */
void levels_save(void)
{
	db_set_int(db_dev(), "volume", cur_vol);
	db_set_int(db_dev(), "brightness", cur_bright);
	db_write_boot_env();
}

/* wpctl is a process, tens of ms - far too slow for the frame loop, and a
 * nudge lands in the frame loop - so sets go to a worker. Latest wins: a held
 * rocker is one write of where it ended, not a queue of stale ones. Same as
 * diatom's vol_worker. */
static pthread_mutex_t vol_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  vol_cv = PTHREAD_COND_INITIALIZER;
static int             vol_want = -1;

static void *vol_worker(void *arg)
{
	extern char **environ;
	(void)arg;
	for (;;) {
		char pct[16];
		char *argv[] = { "wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", pct, NULL };
		pid_t pid;
		int st, want;

		pthread_mutex_lock(&vol_mu);
		while (vol_want < 0) pthread_cond_wait(&vol_cv, &vol_mu);
		want = vol_want;
		vol_want = -1;
		pthread_mutex_unlock(&vol_mu);

		snprintf(pct, sizeof pct, "%d%%", want * 100 / VOL_MAX);
		if (posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ) == 0)
			waitpid(pid, &st, 0);
	}
	return NULL;
}

void apply_volume(int v)
{
	cur_vol = v;
	if (mixer_fd < 0) return;
	pthread_mutex_lock(&vol_mu);
	vol_want = v;
	pthread_cond_signal(&vol_cv);
	pthread_mutex_unlock(&vol_mu);
}

static void backlight_write(int raw)
{
	char s[8];
	int n = snprintf(s, sizeof s, "%d\n", raw);

	if (disp_fd >= 0 && pwrite(disp_fd, s, n, 0) != n)
		fprintf(stderr, "settings: backlight write %d failed\n", raw);
}

void apply_brightness(int b)
{
	cur_bright = b;
	backlight_write(bright_ladder[b]);
}

/* Dark, not rung 0 (raw 40 is a dim screen); cur_bright is kept so
 * apply_brightness(cur_bright) brings the player's level back. */
void backlight_off(void) { backlight_write(0); }

void jack_forget(void) { }
void mute_forget(void) { }

/* What PipeWire is at now, in rungs - once, at start, where the one wpctl run
 * is not in a frame. -1 if it cannot say. */
static int wpctl_level(void)
{
	FILE *f = popen("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", "r");
	float v;
	int lv = -1;

	if (!f) return -1;
	if (fscanf(f, "Volume: %f", &v) == 1)
		lv = clampi((int)(v * VOL_MAX + 0.5f), 0, VOL_MAX);
	pclose(f);
	return lv;
}

void plat_settings_init(void)
{
	pthread_t t;
	int v, b;

	disp_fd = open(BACKLIGHT, O_RDWR | O_CLOEXEC);
	if (disp_fd < 0) fprintf(stderr, "settings: no %s\n", BACKLIGHT);
	if (pthread_create(&t, NULL, vol_worker, NULL) == 0) {
		pthread_detach(t);
		mixer_fd = 0;
	} else
		fprintf(stderr, "settings: no volume worker\n");

	/* The player's last choice wins; failing that, whatever the device is at
	 * already, so the first OSD tells the truth. */
	v = db_get_int(db_dev(), "volume", -1);
	b = db_get_int(db_dev(), "brightness", -1);
	if (v < 0) v = wpctl_level();
	if (b < 0 && disp_fd >= 0) {
		char s[16] = { 0 };
		int raw, i;
		if (pread(disp_fd, s, sizeof s - 1, 0) > 0 && (raw = atoi(s)) >= 0) {
			b = 0;
			for (i = 1; i <= BRIGHT_MAX; i++)
				if (abs(bright_ladder[i] - raw) < abs(bright_ladder[b] - raw)) b = i;
		}
	}
	cur_vol    = v >= 0 ? clampi(v, 0, VOL_MAX)    : -1;
	cur_bright = b >= 0 ? clampi(b, 0, BRIGHT_MAX) : BRIGHT_MAX / 2;
	if (cur_vol >= 0) apply_volume(cur_vol);
	apply_brightness(cur_bright);
}

void plat_audio_jack_poll(void) { }
bool plat_headphones_present(void) { return false; }
bool plat_mute_poll(bool own_volume) { (void)own_volume; return false; }
bool plat_muted(void) { return false; }
void plat_mute_switch_lock(bool lock) { (void)lock; }
bool plat_hold_switch(void) { return false; }

bool plat_sleep_supported(void) { return false; }
bool plat_sleep(void) { return false; }
/* "Awake at once", not false: false means no suspend to escalate into, and
 * the caller answers that by powering off - which is what idle did here,
 * minutes into the first test. Until the GKD sleeps for real, idle is a
 * no-op that restarts the idle clock. */
bool plat_light_sleep(unsigned waited_ms) { (void)waited_ms; return true; }
bool plat_usb_host(void) { return false; }
int plat_suspend_timeout_secs(void) { return 30; }   /* never 0: platform.h */
pwr_action plat_power_tap_or_hold(bool down) { (void)down; return PWR_NONE; }

bool plat_battery(int *pct, bool *charging)
{
	return battery_read("/sys/class/power_supply/battery", pct, charging);
}
