/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The GKD 350H Ultra on vendor ROCKNIX, under sway: the device file for
 * PLATFORM=gkd, as platform_brick.c is for the Brick.
 *
 * Video, input and paths are real (plorpos-gkd.3.3), levels and battery too
 * (gkd.3.4), power and sleep too (gkd.7). The rest are stubs answering "not
 * here". diatom's port/gkd.c is the reference for all of it - same
 * window setup, same buttons - so the launcher and a game agree on the
 * device. */
#include "db.h"
#include "platform.h"
#include "platform_dev.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
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
bool plat_has_stick(void) { return true; }
/* Home turns the volume keys into brightness keys, as it does in a game. */
const char *plat_bright_keys(void) { return "Home+Vol"; }

/* ---- video ------------------------------------------------------------
 *
 * A fullscreen Wayland window under sway, which rotates the portrait panel
 * (transform 270) to 1600x1440: 1.5625 panel pixels to the layout unit, and
 * 921 units tall. */
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
	plat_geometry_init(ren);
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

/* Two fullscreen windows under sway, and sway does not reliably show the one
 * that drew last: a game started under the shelf played unseen (2026-09-29).
 * So only one is ever mapped - Diatom hides its own at every handover, and
 * this hides the shelf's while a game runs. Pumping flushes the unmap to sway
 * before Diatom's first frame, as Diatom does. */
void screen_yield(bool to_game)
{
	if (!win) return;
	if (to_game) { SDL_HideWindow(win); SDL_PumpEvents(); }
	else SDL_ShowWindow(win);
}

/* ---- input ------------------------------------------------------------
 *
 * Straight from evdev, found by name: the pad (gkd_atom_joypad) and the
 * volume keys (gpio-keys). No SDL joystick, and no EVIOCGRAB - a game's
 * diatom reads the same nodes the same way. The map is diatom's padmap: the
 * face buttons are NOT crossed as on the Brick (BTN_EAST is the A printed on
 * the shell), MODE is Menu, the stick is a second d-pad, and Home is only a
 * modifier - held, it turns the volume keys into brightness keys. L2/R2, the
 * stick click, Home and the stick's own directions are reported too, but only
 * the Hotkeys screen's press-to-bind listens (plorpos-gkd.43.3). */
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
	{ BTN_TL2,    IN_L2     }, { BTN_TR2,    IN_R2   },
	{ BTN_THUMBL, IN_L3     },
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
	/* Read though logind is reading it too: launch.sh's inhibitor only stops
	 * logind acting on it. */
	if (fd_power < 0) fd_power = evdev_open("rk805 pwrkey");
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
	drain(fd_power);
	/* Their releases were in what was just thrown away. */
	memset(pad_dir, 0, sizeof pad_dir);
	memset(stick_dir, 0, sizeof stick_dir);
	home_down = false;
}

void plat_input_quit(void)
{
	if (fd_pad >= 0)  { close(fd_pad);  fd_pad = -1; }
	if (fd_keys >= 0) { close(fd_keys); fd_keys = -1; }
	if (fd_power >= 0) { close(fd_power); fd_power = -1; }
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
		case BTN_TRIGGER_HAPPY1: home_down = down; set_btn(st, IN_HOME, down); continue;
		}
		for (size_t k = 0; k < sizeof padmap / sizeof padmap[0]; k++)
			if (padmap[k].code == ev.code) set_btn(st, padmap[k].b, down);
	}
	for (int i = 0; i < 4; i++) {
		set_btn(st, (in_button)(IN_LEFT + i), pad_dir[i] || stick_dir[i]);
		set_btn(st, (in_button)(IN_SLEFT + i), stick_dir[i]);
	}
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

static void power_read(in_state *st)
{
	struct input_event ev;

	while (fd_power >= 0 && read(fd_power, &ev, sizeof ev) == (ssize_t)sizeof ev)
		if (ev.type == EV_KEY && ev.code == KEY_POWER && ev.value != 2)
			set_btn(st, IN_POWER, ev.value == 1);
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
	power_read(st);
	plat_sink_follow(false);
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

/* The headphone jack, which the kernel switches and sometimes gets wrong.
 *
 * ROCKNIX's es9018k2m driver owns three pins on gpio4: hp-det (the jack), and
 * hp-con / spk-con (the two amps). On an edge it waits 100 ms, reads hp-det
 * once and re-arms for the opposite edge only - so when the plug's rings are
 * still crossing the detect contact after that read, the last edge is lost
 * and the amps stay set for headphones that are gone: a silent speaker, with
 * the stream still running, until the next edge. Seen 2026-09-29 (plorpos-
 * gkd.9.1), rare, and nothing in userspace makes the driver look again.
 *
 * So this looks instead: every 250 ms, and once the pin has read the same
 * twice, amps that disagree with it are set to match. The data register is
 * write-masked per pin (high half), so this touches only those two pins and
 * cannot race the driver's own writes. Pin numbers are this board's device
 * tree; jack_names_ok() checks it is still that tree, or none of this runs.
 *
 * Audio Output = Speaker overrides the jack here too (user, 2026-09-29): the
 * amps are set for the speaker with headphones in. The kernel still switches
 * to them on a plug, so that is a blip of at most half a second. */
#define JACK_BANK     0x2ae40000u   /* gpio4 */
#define JACK_DR_L     (0x00 / 4)    /* data out, pins 0-15, write-masked */
#define JACK_EXT      (0x70 / 4)    /* pin levels */
#define JACK_HP_CON   2             /* gpio-130 */
#define JACK_SPK_CON  4             /* gpio-132 */
#define JACK_DET      6             /* gpio-134 */

static volatile uint32_t *jack_regs;
static bool               jack_in;
static bool               route_speaker_only;   /* Audio Output = Speaker */

static bool jack_names_ok(void)
{
	static const char *want[] = {
		"GPIOs 128-159, parent: platform/2ae40000.gpio",
		"gpio-130 (", "avrman-hp-con",
		"gpio-132 (", "avrman-spk-con",
		"gpio-134 (", "avrman-hp-det",
	};
	char line[256];
	int  seen = 0;
	FILE *f = fopen("/sys/kernel/debug/gpio", "r");

	if (!f) return false;
	while (fgets(line, sizeof line, f)) {
		if (strstr(line, want[0])) seen |= 1;
		for (int i = 1; i < 7; i += 2)
			if (strstr(line, want[i]) && strstr(line, want[i + 1])) seen |= 2 << (i / 2);
	}
	fclose(f);
	return seen == 15;
}

static void *jack_worker(void *arg)
{
	const uint32_t mask = 1u << (16 + JACK_HP_CON) | 1u << (16 + JACK_SPK_CON);
	int  last  = -1;
	bool fixed = false;
	(void)arg;

	for (;;) {
		int      det = jack_regs[JACK_EXT] >> JACK_DET & 1;
		uint32_t out = jack_regs[JACK_DR_L];
		int      hp  = out >> JACK_HP_CON & 1, spk = out >> JACK_SPK_CON & 1;
		int      phones = det && !__atomic_load_n(&route_speaker_only, __ATOMIC_RELAXED);
		bool     fix = det == last && (hp != phones || spk == phones);

		__atomic_store_n(&jack_in, det == 1, __ATOMIC_RELAXED);
		if (fix) {
			jack_regs[JACK_DR_L] = mask | (uint32_t)phones << JACK_HP_CON
			                            | (uint32_t)!phones << JACK_SPK_CON;
			/* Once per run of fixes, so a driver that fights back cannot
			 * fill the card's log. */
			if (!fixed) fprintf(stderr, "jack: amps set for %s\n",
			                    phones ? "headphones" : "the speaker");
		}
		fixed = fix;
		last  = det;
		usleep(250 * 1000);
	}
	return NULL;
}

static void jack_init(void)
{
	pthread_t t;
	void *p;
	int fd;

	if (!jack_names_ok()) {
		fprintf(stderr, "jack: not the expected gpio layout, no jack watch\n");
		return;
	}
	fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
	if (fd < 0) return;
	p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, JACK_BANK);
	close(fd);
	if (p == MAP_FAILED) return;
	jack_regs = p;
	if (pthread_create(&t, NULL, jack_worker, NULL) == 0) pthread_detach(t);
}

/* Where the sound goes (plorpos-gkd.9.3). Every player opens ALSA's "default",
 * which is PipeWire's default sink, and PipeWire moves running streams when
 * that changes - so routing here is choosing the default sink, and Diatom and
 * Muse are never told a device. ROCKNIX's module-switch-on-connect already
 * makes a new headset the default; this adds the launcher's rules on top:
 * a cable wins (the kernel puts it on the built-in sink), Speaker never uses
 * Bluetooth, and a headset going away while it was playing pauses Muse, as a
 * phone does.
 *
 * Driven by `pactl subscribe` - one child for the launcher's life, woken only
 * when a sink comes, goes or the default changes - plus the jack watchdog's
 * pin and the player's setting, both looked at every 250 ms for nothing. */
static bool route_bt;               /* PipeWire has a Bluetooth sink */
static int  sink_level = -1;        /* the default sink's volume, in rungs */
static int  wpctl_level(void);

void plat_audio_speaker_only(bool on) { __atomic_store_n(&route_speaker_only, on, __ATOMIC_RELAXED); }
bool plat_bt_audio(void) { return __atomic_load_n(&route_bt, __ATOMIC_RELAXED); }

/* The same note btplayer leaves on the Brick for a headset's own buttons, and
 * musec reads it wherever Muse matters; tmpfs, so nothing reaches the card. */
static void btkey_write(const char *act)
{
	struct timespec t;
	FILE *f = fopen("/tmp/tortos_btkey.new", "w");

	if (!f) return;
	clock_gettime(CLOCK_MONOTONIC, &t);
	fprintf(f, "%s %ld\n", act, (long)t.tv_sec * 1000 + t.tv_nsec / 1000000);
	if (fclose(f) == 0) rename("/tmp/tortos_btkey.new", "/tmp/tortos_btkey");
}

static void route_eval(bool *on_bt)
{
	extern char **environ;
	char line[256], bt[128] = "", builtin[128] = "", def[128] = "";
	const char *want;
	bool speaker = __atomic_load_n(&route_speaker_only, __ATOMIC_RELAXED);
	FILE *f = popen("pactl list short sinks 2>/dev/null", "r");

	if (!f) return;
	while (fgets(line, sizeof line, f)) {
		char name[128];
		if (sscanf(line, "%*s %127s", name) != 1) continue;
		if (!strncmp(name, "bluez_output.", 13) && !bt[0])
			snprintf(bt, sizeof bt, "%s", name);
		else if (!strncmp(name, "alsa_output.", 12) && !builtin[0])
			snprintf(builtin, sizeof builtin, "%s", name);
	}
	pclose(f);
	if ((f = popen("pactl get-default-sink 2>/dev/null", "r"))) {
		if (fgets(def, sizeof def, f)) def[strcspn(def, "\n")] = '\0';
		pclose(f);
	}
	/* Two headsets: keep the one already chosen. */
	if (bt[0] && !strncmp(def, "bluez_output.", 13)) snprintf(bt, sizeof bt, "%s", def);
	__atomic_store_n(&route_bt, bt[0] != '\0', __ATOMIC_RELAXED);

	if (*on_bt && !bt[0]) {
		fprintf(stderr, "audio: headset gone, pausing Muse\n");
		btkey_write("pause");
	}
	want = speaker || plat_headphones_present() || !bt[0] ? builtin : bt;
	*on_bt = want == bt;
	if (want[0] && strcmp(want, def)) {
		char *argv[] = { "pactl", "set-default-sink", (char *)want, NULL };
		pid_t pid;
		int st;

		fprintf(stderr, "audio: output %s\n", want == bt ? "bluetooth" : "built-in");
		if (posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ) == 0)
			waitpid(pid, &st, 0);
	}
}

static void *route_worker(void *arg)
{
	bool on_bt = false;
	(void)arg;

	for (;;) {
		char *argv[] = { "pactl", "subscribe", NULL };
		int fd[2];
		pid_t pid;
		bool speaker = !route_speaker_only, jack = !plat_headphones_present();

		if (pipe(fd) < 0) { sleep(2); continue; }
		/* Not posix_spawn: pactl ignores SIGPIPE itself, so a launcher that
		 * died would leave it writing into a pipe nobody reads, one more per
		 * restart. The kernel ends it with the launcher instead - with this
		 * thread, strictly, which lives as long. */
		pid_t parent = getpid();
		pid = fork();
		if (pid == 0) {
			prctl(PR_SET_PDEATHSIG, SIGTERM);
			if (getppid() != parent) _exit(1);      /* died before the prctl */
			dup2(fd[1], STDOUT_FILENO);
			close(fd[0]);
			close(fd[1]);
			execv("/usr/bin/pactl", argv);
			_exit(127);
		}
		close(fd[1]);

		while (pid > 0) {
			struct pollfd p = { fd[0], POLLIN, 0 };
			char buf[1024];
			bool dirty = false;

			if (poll(&p, 1, 250) > 0) {
				ssize_t n = read(fd[0], buf, sizeof buf - 1);
				if (n <= 0) break;          /* pipewire-pulse went away */
				buf[n] = '\0';
				dirty = strstr(buf, "'new' on sink #") || strstr(buf, "'remove' on sink #")
				     || strstr(buf, "on server");
			}
			if (speaker != route_speaker_only || jack != plat_headphones_present()) {
				speaker = route_speaker_only;
				jack    = plat_headphones_present();
				dirty   = true;
			}
			if (dirty) route_eval(&on_bt);
			/* After route_eval, so a new default is the one read. Every
			 * volume set - ours, Diatom's, a headset's - is a sink change. */
			if (dirty || (p.revents && strstr(buf, "'change' on sink #")))
				__atomic_store_n(&sink_level, wpctl_level(), __ATOMIC_RELAXED);
		}
		close(fd[0]);
		if (pid > 0) { kill(pid, SIGTERM); waitpid(pid, NULL, 0); }
		sleep(1);
	}
	return NULL;
}

/* ---- headset buttons ---------------------------------------------------
 *
 * btplayer's job on the Brick (tools/btplayer.c), without its D-Bus half:
 * BlueZ gives each AVRCP headset a keyboard named `<headset> (AVRCP)` that
 * comes and goes with the connection, so the list is read again every two
 * seconds and a node that has gone is closed. The WF-C510 sends PLAYCD or
 * PAUSECD for a press, by its own idea of the state, NEXTSONG for two and
 * PREVIOUSSONG for three; a hold stays on the earbud. No volume keys: the GKD
 * owns the level. */
#define HEADSETS 4

/* btplayer's key_action, for the same reasons. */
static const char *avrcp_action(unsigned code)
{
	switch (code) {
	case KEY_PLAYCD: case KEY_PLAYPAUSE: case KEY_PLAY: return "toggle";
	case KEY_PAUSECD: case KEY_STOPCD: case KEY_PAUSE:  return "pause";
	case KEY_NEXTSONG: case KEY_FASTFORWARD:           return "next";
	case KEY_PREVIOUSSONG: case KEY_REWIND:            return "prev";
	}
	return NULL;
}

static void *avrcp_worker(void *arg)
{
	struct pollfd p[HEADSETS];
	char node[HEADSETS][24];
	int n = 0;
	(void)arg;

	for (;;) {
		FILE *f = fopen("/proc/bus/input/devices", "r");
		char line[256], *ev;
		bool avrcp = false;

		while (f && fgets(line, sizeof line, f)) {
			char path[24];
			int i;

			if (!strncmp(line, "N: Name=", 8)) {
				avrcp = strstr(line, " (AVRCP)\"") != NULL;
				continue;
			}
			if (!avrcp || strncmp(line, "H: Handlers=", 12) || !(ev = strstr(line, "event")))
				continue;
			avrcp = false;
			snprintf(path, sizeof path, "/dev/input/%.*s", (int)strspn(ev, "event0123456789"), ev);
			for (i = 0; i < n && strcmp(node[i], path); i++) ;
			if (i < n || n == HEADSETS) continue;
			if ((p[n].fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC)) < 0) continue;
			p[n].events = POLLIN;
			snprintf(node[n++], sizeof node[0], "%s", path);
			fprintf(stderr, "audio: headset keys on %s\n", path);
		}
		if (f) fclose(f);

		poll(p, n, 2000);
		for (int i = 0; i < n; i++) {
			struct input_event e;
			ssize_t got;

			if (!p[i].revents) continue;
			while ((got = read(p[i].fd, &e, sizeof e)) == (ssize_t)sizeof e) {
				const char *act = e.type == EV_KEY && e.value == 1 ? avrcp_action(e.code) : NULL;
				if (act) btkey_write(act);
			}
			if (got < 0 && errno != EAGAIN) {   /* the headset went */
				close(p[i].fd);
				p[i] = p[--n];
				memcpy(node[i], node[n], sizeof node[0]);
				i--;
			}
		}
	}
	return NULL;
}

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

/* The output's own volume, when something other than a level set here or by
 * Diatom moved it: a headset's buttons, which PipeWire takes as the sink's
 * (bluez5.hw-volume), or a change of output, since each sink keeps its own.
 * Adopted once the sink has disagreed with the level for half a second - a
 * set of ours lands in tens of ms, and a headset's echo of it rounds to the
 * same rung, so only a change from outside lasts. On the shelf it is a nudge
 * (slider, saved; the sink snaps onto the ladder's 5 % rungs); in a game it
 * goes to Diatom, which owns the level while it runs. */
void plat_sink_follow(bool game)
{
	static unsigned since;
	static int seen = -1;
	int count, resident = plat_resident_volume(&count);
	int have = game && resident >= 0 ? resident : cur_vol;
	int want = __atomic_load_n(&sink_level, __ATOMIC_RELAXED);
	unsigned now = plat_now_ms();

	if (want < 0 || have < 0 || want == have) { seen = -1; return; }
	if (want != seen) { seen = want; since = now; return; }
	if (now - since < 500) return;
	seen = -1;
	fprintf(stderr, "audio: output is at level %d, was %d\n", want, have);
	if (game) plat_resident_line("SETLEVEL\tkind=volume\tindex=%d\tcount=%d", want, VOL_MAX + 1);
	else      plat_volume_nudge(want - have);
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
	/* Before the watchdog's first look, or it sets the amps for headphones
	 * until main.c's first aout_apply says Speaker. */
	{
		char ao[32];
		db_get_str(db_dev(), "audioout", ao, sizeof ao, "auto");
		plat_audio_speaker_only(!strcmp(ao, "speaker"));
	}
	jack_init();
	if (pthread_create(&t, NULL, route_worker, NULL) == 0) pthread_detach(t);
	if (pthread_create(&t, NULL, avrcp_worker, NULL) == 0) pthread_detach(t);

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
	plat_mute_switch_lock(db_get_int(db_dev(), "muteswitch", 0) == 1);
}

void plat_audio_jack_poll(void) { }
bool plat_headphones_present(void) { return __atomic_load_n(&jack_in, __ATOMIC_RELAXED); }
bool plat_mute_poll(bool own_volume) { (void)own_volume; return false; }
bool plat_muted(void) { return false; }
/* No switch: Muse Settings' Sleep Button Lock stands in for the Brick's
 * switch held down in button lock (gkd.34), under the same db key. */
static bool button_lock;
void plat_mute_switch_lock(bool lock) { button_lock = lock; }
bool plat_hold_switch(void) { return button_lock; }

/* Real suspend through systemd, so ROCKNIX's system-sleep hook runs - it
 * brings Wi-Fi back, among other things. By starting suspend.target, not
 * `systemctl suspend`: that returns before the device has even gone down
 * (28ms, measured 2026-09-29), while starting the target returns only once
 * systemd-suspend.service has finished, resume hook included. Skipped that
 * way: logind's PrepareForSleep, which nothing on the device waits on (no
 * delay inhibitors).
 *
 * Two things about the wake, both measured 2026-09-29. The panel comes back
 * at full brightness whatever the backlight file says, until someone writes
 * it. And the hook's resume half takes 2s, behind its Wi-Fi reconnect, then
 * writes ES's brightness. So this does not wait for the hook: it watches for
 * the wake itself - suspended time counts on CLOCK_BOOTTIME and not on
 * CLOCK_MONOTONIC - writes the player's level at once and returns, as the
 * Brick's does with its radios still coming up behind it. hook_watch sees
 * the hook out and puts the level back over ES's. */
bool plat_sleep_supported(void) { return true; }

/* The hook's brightness is ES's display.brightness, a percent, and it writes
 * it back into system.cfg itself on every wake through set_setting. Handing
 * it the player's level first makes that write land where the screen already
 * is, so hook_watch has at most a unit of rounding to fix instead of a flash.
 * ROCKNIX's own function and lock; the timeout because wait_lock waits
 * forever on a stale lock, and then the flash is all that is lost. */
static void hook_brightness_sync(void)
{
	static int synced = -1;
	extern char **environ;
	int pct = (bright_ladder[cur_bright] * 100 + 127) / 255;
	char cmd[128];
	char *argv[] = { "timeout", "2", "sh", "-c", cmd, NULL };
	pid_t pid;
	int st = -1;

	if (pct == synced) return;
	snprintf(cmd, sizeof cmd, ". /etc/profile.d/001-functions && "
	         "set_setting display.brightness %d", pct);
	if (posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ) == 0)
		waitpid(pid, &st, 0);
	if (st == 0) synced = pct;
	else fprintf(stderr, "sleep: display.brightness %d not set (%d)\n", pct, st);
}

static long long asleep_ms(void)
{
	struct timespec b, m;

	clock_gettime(CLOCK_BOOTTIME, &b);
	clock_gettime(CLOCK_MONOTONIC, &m);
	return (b.tv_sec - m.tv_sec) * 1000LL + (b.tv_nsec - m.tv_nsec) / 1000000;
}

/* Until systemctl exits: any backlight value that is neither the player's
 * level nor dark is the hook's, so the level goes back. Dark is left alone -
 * that is a tap sleeping again before the hook is done. */
static void *hook_watch(void *arg)
{
	pid_t pid = (pid_t)(intptr_t)arg;
	int st;

	while (waitpid(pid, &st, WNOHANG) == 0) {
		char s[16] = { 0 };
		int raw;

		if (disp_fd >= 0 && pread(disp_fd, s, sizeof s - 1, 0) > 0 &&
		    (raw = atoi(s)) != 0 && raw != bright_ladder[cur_bright]) {
			fprintf(stderr, "sleep: backlight %d after the wake, back to %d\n",
			        raw, bright_ladder[cur_bright]);
			apply_brightness(cur_bright);
		}
		usleep(10 * 1000);
	}
	return NULL;
}

bool plat_sleep(void)
{
	extern char **environ;
	char *argv[] = { "systemctl", "start", "suspend.target", NULL };
	unsigned t0 = plat_now_ms();
	long long went = asleep_ms();
	pthread_t th;
	pid_t pid;
	int st = -1;

	hook_brightness_sync();
	sync();
	if (posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ) != 0) return false;
	while (waitpid(pid, &st, WNOHANG) == 0) {
		if (asleep_ms() - went > 500) {
			apply_brightness(cur_bright);
			fprintf(stderr, "sleep: woke after %us\n",
			        (plat_now_ms() - t0) / 1000);
			if (pthread_create(&th, NULL, hook_watch, (void *)(intptr_t)pid) == 0)
				pthread_detach(th);
			else
				waitpid(pid, &st, 0);   /* no watcher: wait the hook out */
			/* The press that woke it, which would otherwise read as a tap. */
			drain(fd_power);
			return true;
		}
		usleep(10 * 1000);
	}
	/* Out before a wake was seen: a sleep too short to see (the hook done
	 * too), or systemd would not suspend. */
	fprintf(stderr, "sleep: suspend.target %s\n", st == 0 ? "ok" : "failed");
	drain(fd_power);
	return st == 0;
}

bool plat_battery(int *pct, bool *charging)
{
	return battery_read("/sys/class/power_supply/battery", pct, charging);
}
