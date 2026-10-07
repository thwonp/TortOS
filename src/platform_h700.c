/* SPDX-License-Identifier: MIT */
/* Anbernic's H700 handhelds on BaseOS (plorpos-7ny): the RG SP first. BaseOS
 * is the stock Anbernic kernel and Mali blob under a BusyBox userland, and
 * starts us from the card's System/launch_frontend.sh (sd/h700/). The panel
 * is fbdev, drawn through the Mali fbdev EGL winsys by our own SDL2's "mali"
 * driver (mk/fetch-h700-sysroot.sh), so the shape of this file is the
 * Brick's: one display, no compositor, diatom on the same fb0.
 *
 * Not here yet, by plan: deep sleep, the lid and the jack's own ladder
 * (plorpos-7ny.6), Wi-Fi (.5, wifi.c's host stubs meanwhile), Bluetooth (v2,
 * bt.c's host stubs). The power key's tap still turns the screen off, which
 * is platform.c's light sleep and needs nothing from this file. */
#include "atomic.h"
#include "db.h"
#include "platform.h"
#include "audioout.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "platform_dev.h"

/* BaseOS mounts the frontend card at /mnt/sdcard and links /mnt/SDCARD to
 * it, so the Brick's spelling holds. The userdata directory is the device's:
 * launch_frontend.sh sets TORTOS_USERDATA from /etc/baseos-release, and this
 * default is the RG SP's, the card's existing one. */
const char *P_ROOT = "/mnt/SDCARD/TortOS";
const char *P_CARD = "/mnt/SDCARD";
const char *P_ROMS = "/mnt/SDCARD/Roms";
const char *P_USERDATA = "/mnt/SDCARD/.userdata/rgsp";
const char *P_SHARED = "/mnt/SDCARD/.userdata/shared";
const char *P_WEB = "/mnt/SDCARD/TortOS/res/web";
const char *P_FONT = "/mnt/SDCARD/TortOS/menu.ttf";

/* What a game's process is told about the device - see build_child_env(). */
const char *const plat_child_env[] = {
	"PLATFORM=h700", "DEVICE=rgsp",
	"SDCARD_PATH=/mnt/SDCARD",
	"BIOS_PATH=/mnt/SDCARD/Bios",
	"CHEATS_PATH=/mnt/SDCARD/Cheats",
	"SAVES_PATH=/mnt/SDCARD/Saves",
	NULL
};
/* Our SDL2 lives on the card; BaseOS has no aarch64 one. */
const char plat_child_libpath[] = ":/mnt/SDCARD/TortOS/lib";
/* Native PICO-8 quiet while Muse plays, as on the Brick (child_quiet). */
const char *const plat_pico8_preload = "/mnt/SDCARD/TortOS/pico8sdl.so";
/* Splore downloads with wget, and BaseOS's BusyBox wget has no TLS: the
 * shim in this folder hands them to its curl (sd/tortos/pico8/wget). */
const char *const plat_pico8_path = "/mnt/SDCARD/TortOS/pico8";

bool plat_is_brick_pro(void) { return false; }
bool plat_two_sticks(void) { return false; }
/* No brightness keys: Menu held turns the volume keys into them. */
const char *plat_bright_keys(void) { return "Menu+Vol"; }

/* The RG SP's "ANBERNIC-keys" (event1): every button, the d-pad as hat 0,
 * and the volume keys. The power key is the PMIC's (event0). Pressed and
 * logged 2026-10-06; X and Y cross their evdev names, as on the Brick. */
#define PAD_NODE   "/dev/input/event1"
#define POWER_NODE "/dev/input/event0"
#define CODE_MENU  312   /* BTN_TL2 by name */
static const struct { int code; in_button b; } padmap[] = {
	{ 304, IN_ACCEPT }, { 305, IN_BACK }, { 306, IN_Y }, { 307, IN_X },
	{ 308, IN_L1 }, { 309, IN_R1 }, { 314, IN_L2 }, { 315, IN_R2 },
	{ 310, IN_SELECT }, { 311, IN_START },
};
/* The same buttons for native PICO-8's SDL. With no mapping SDL guesses one
 * from the evdev names, and these cross them: Start (BTN_TR) went to the
 * right shoulder, which PICO-8 ignores, and R2 (BTN_START) paused it
 * (plorpos-7ny.41). SDL numbers the buttons by code from BTN_JOYSTICK up:
 * b0..b8 = 304..312, b9 = 314, b10 = 315. Menu (b8) is left out - it is the
 * launcher's. The GUID is bus 0x19, vendor 1, product 1, version 0x100 with
 * no name CRC, which SDL matches whatever the name. */
const char plat_pico8_pad[] =
	"SDL_GAMECONTROLLERCONFIG=19000000010000000100000000010000,ANBERNIC-keys,"
	"a:b0,b:b1,y:b2,x:b3,leftshoulder:b4,rightshoulder:b5,back:b6,start:b7,"
	"lefttrigger:b9,righttrigger:b10,"
	"dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,platform:Linux,";

static SDL_Window *win;
static SDL_Renderer *ren;
static int fd_pad = -1;

/* Order: left, right, up, down. */
static bool hat_dir[4];
/* Menu is a modifier as well as a button: held with another button it goes
 * down with it (the hotkey combos), with a volume key it is spent on
 * brightness, and alone it is a press on release. menu_pulse lets that
 * release-press go up on the next poll. */
static bool menu_down, menu_sent, menu_spent, menu_pulse;

SDL_Renderer *plat_renderer(void) { return ren; }

/* ---- video ----------------------------------------------------------- */

bool plat_video_init(void)
{
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
	SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
	if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "video init: %s\n", SDL_GetError());
		return false;
	}
	SDL_ShowCursor(SDL_DISABLE);
	/* TWO PAGES FOR US, THE TOP ONE FOR DIATOM. Diatom grows fb0 to three
	 * pages and parks a paused game on the top one, trusting a GL launcher
	 * to double-buffer on the two below (its present_stop). But Mali's fbdev
	 * winsys takes every page the virtual height offers, fixed when the window
	 * is made - and diatom starts first, so the launcher triple-buffered over
	 * the park page and the in-game menu flickered against the game
	 * (2026-10-06). So the window is made on two pages and the height put
	 * back; a window keeps the buffers it was made with. */
	{
		int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
		struct fb_var_screeninfo v, two;

		if (fd >= 0 && ioctl(fd, FBIOGET_VSCREENINFO, &v) == 0 && v.yres_virtual > 2 * v.yres) {
			two = v;
			two.yres_virtual = 2 * v.yres;
			two.yoffset = 0;
			if (ioctl(fd, FBIOPUT_VSCREENINFO, &two) == 0) {
				/* The mali driver makes every window the whole panel. */
				win = SDL_CreateWindow("plorpOS", 0, 0, 720, 480,
				                       SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
				v.yoffset = 0;
				ioctl(fd, FBIOPUT_VSCREENINFO, &v);
			}
		}
		if (fd >= 0) close(fd);
	}
	if (!win)
		win = SDL_CreateWindow("plorpOS", 0, 0, 720, 480, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
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
	plat_geometry_init(ren);
	SDL_RendererInfo info;
	if (SDL_GetRendererInfo(ren, &info) == 0)
		fprintf(stderr, "renderer: %s, driver: %s, vsync %s, render-to-texture %s\n",
		        info.name, SDL_GetCurrentVideoDriver(),
		        (info.flags & SDL_RENDERER_PRESENTVSYNC) ? "granted" : "DECLINED (requested)",
		        (info.flags & SDL_RENDERER_TARGETTEXTURE) ? "available" : "MISSING");
	return true;
}

/* Over a frozen child, as on the Brick: no second EGL client on one Mali, so
 * SDL's software renderer draws into a surface and plat_present copies it
 * onto whichever fb0 page is on glass. */
static SDL_Surface *ov_surf;
static uint8_t *ov_fb;
static int ov_fd = -1;
static struct fb_fix_screeninfo ov_fix;
/* The page on glass as the frozen child left it, written back when the menu
 * goes: Mali transaction elimination skips the child's tiles that match its
 * own last write to that page, so a re-rendered still frame never replaced
 * the menu the CPU copied over it - native PICO-8 flickered between its game
 * and our menu after Continue (plorpos-7ny.36, 2026-10-06). */
static uint8_t *ov_saved;
static size_t ov_saved_at, ov_saved_len;

bool plat_video_init_over_child(void)
{
	struct fb_var_screeninfo v;

	ov_fd = open("/dev/fb0", O_RDWR);
	if (ov_fd < 0 || ioctl(ov_fd, FBIOGET_FSCREENINFO, &ov_fix) != 0 ||
	    ioctl(ov_fd, FBIOGET_VSCREENINFO, &v) != 0) {
		fprintf(stderr, "overlay: fb0: %s\n", strerror(errno));
		plat_video_quit();
		return false;
	}
	if (v.bits_per_pixel != 32 || v.red.offset != 16 || v.green.offset != 8 ||
	    v.blue.offset != 0) {
		fprintf(stderr, "overlay: fb0 is not 32-bit xRGB\n");
		plat_video_quit();
		return false;
	}
	ov_fb = mmap(NULL, ov_fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, ov_fd, 0);
	if (ov_fb == MAP_FAILED) ov_fb = NULL;
	ov_surf = SDL_CreateRGBSurfaceWithFormat(0, (int)v.xres, (int)v.yres, 32,
	                                         SDL_PIXELFORMAT_ARGB8888);
	ren = ov_surf ? SDL_CreateSoftwareRenderer(ov_surf) : NULL;
	if (!ov_fb || !ren) {
		fprintf(stderr, "overlay: %s\n", ov_fb ? SDL_GetError() : "mmap fb0 failed");
		plat_video_quit();
		return false;
	}
	ov_saved_at  = (size_t)v.yoffset * ov_fix.line_length;
	ov_saved_len = (size_t)v.yres * ov_fix.line_length;
	if (ov_saved_at + ov_saved_len <= ov_fix.smem_len &&
	    (ov_saved = malloc(ov_saved_len)) != NULL)
		memcpy(ov_saved, ov_fb + ov_saved_at, ov_saved_len);
	SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
	plat_geometry_init(ren);
	return true;
}

/* Mali transaction elimination: the GPU skips writing a 16x16 tile whose
 * content matches what THIS process last wrote to that buffer. Diatom draws
 * into the same fb0 pages while a game runs, so a launcher frame after a
 * handback kept the game in every tile it drew unchanged - the in-game menu's
 * plain panel after a launch from Muse, or a whole menu in the buffer Continue
 * left it in (plorpos-7ny.31, 2026-10-06). One near-black pixel per tile on
 * the first frames after a handback makes every tile differ; the next frame
 * into each buffer writes them all. Three frames: the window was measured
 * cycling all three pages. Diatom's gl_te_mark is the same cure, its side. */
static int te_mark;   /* frames still to mark */

static void te_mark_draw(SDL_Renderer *r)
{
	static SDL_Point pt[(720 / 16) * (480 / 16)];
	SDL_BlendMode bm;
	Uint8 cr, cg, cb, ca;
	float sx, sy;
	int w = 0, h = 0, n = 0, x, y;

	if (SDL_GetRendererOutputSize(r, &w, &h) != 0) return;
	for (y = 0; y < h && y < 480; y += 16)
		for (x = 0; x < w && x < 720; x += 16)
			pt[n++] = (SDL_Point){ x, y };
	SDL_GetRenderDrawBlendMode(r, &bm);
	SDL_GetRenderDrawColor(r, &cr, &cg, &cb, &ca);
	SDL_RenderGetScale(r, &sx, &sy);
	SDL_RenderSetScale(r, 1.0f, 1.0f);
	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
	SDL_SetRenderDrawColor(r, 0, 0, 1, 255);
	SDL_RenderDrawPoints(r, pt, n);
	SDL_SetRenderDrawColor(r, cr, cg, cb, ca);
	SDL_SetRenderDrawBlendMode(r, bm);
	SDL_RenderSetScale(r, sx, sy);
}

void plat_present(SDL_Renderer *r)
{
	if (te_mark > 0 && r == ren && !ov_surf) {
		te_mark--;
		te_mark_draw(r);
	}
	SDL_RenderPresent(r);
	if (ov_surf && r == ren) {
		struct fb_var_screeninfo v;
		size_t row = (size_t)ov_surf->w * 4;
		int y;

		if (ioctl(ov_fd, FBIOGET_VSCREENINFO, &v) != 0 ||
		    ((size_t)v.yoffset + (size_t)ov_surf->h) * ov_fix.line_length > ov_fix.smem_len)
			return;
		for (y = 0; y < ov_surf->h; y++)
			memcpy(ov_fb + ((size_t)v.yoffset + (size_t)y) * ov_fix.line_length +
			       (size_t)v.xoffset * 4,
			       (uint8_t *)ov_surf->pixels + (size_t)y * (size_t)ov_surf->pitch, row);
	}
}

void plat_video_quit(void)
{
	bool overlay = ov_fd >= 0;

	if (ren) { SDL_DestroyRenderer(ren); ren = NULL; }
	if (ov_surf) { SDL_FreeSurface(ov_surf); ov_surf = NULL; }
	if (ov_saved && ov_fb) memcpy(ov_fb + ov_saved_at, ov_saved, ov_saved_len);
	free(ov_saved);
	ov_saved = NULL;
	if (ov_fb) munmap(ov_fb, ov_fix.smem_len);
	ov_fb = NULL;
	if (ov_fd >= 0) { close(ov_fd); ov_fd = -1; }
	if (win) { SDL_DestroyWindow(win); win = NULL; }
	if (!overlay) SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

/* One display, no compositor: whoever presents last is on glass. Handed
 * back, the next frames are marked (te_mark). */
void screen_yield(bool to_game) { if (!to_game) te_mark = 3; }

/* ---- input: raw evdev, no SDL joystick --------------------------------- */

bool plat_input_init(void)
{
	if (fd_power < 0) fd_power = open(POWER_NODE, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd_pad < 0) fd_pad = open(PAD_NODE, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd_pad < 0) fprintf(stderr, "input: %s: %s\n", PAD_NODE, strerror(errno));
	return fd_pad >= 0;
}

void plat_input_flush(void)
{
	struct input_event ev;
	SDL_Event e;

	SDL_PumpEvents();
	while (SDL_PollEvent(&e)) { }
	while (fd_power >= 0 && read(fd_power, &ev, sizeof ev) == (ssize_t)sizeof ev) { }
	while (fd_pad >= 0 && read(fd_pad, &ev, sizeof ev) == (ssize_t)sizeof ev) { }
	memset(hat_dir, 0, sizeof hat_dir);
	menu_down = menu_sent = menu_spent = menu_pulse = false;
}

/* The descriptors are display-independent and stay open. */
void plat_input_quit(void) { }

/* Menu on the pad's node, which plat_run reads while a child has the screen.
 * The volume keys are on it too, so they come through levels_key. */
int menu_key(int *code)
{
	*code = CODE_MENU;
	return fd_pad;
}

int levels_fd(void) { return -1; }

/* Asked of the kernel: while plat_run has the pad, only it reads it. */
bool levels_alt(void)
{
	unsigned char bits[KEY_MAX / 8 + 1] = { 0 };

	return fd_pad >= 0 && ioctl(fd_pad, EVIOCGKEY(sizeof bits), bits) >= 0 &&
	       (bits[CODE_MENU / 8] >> (CODE_MENU % 8) & 1);
}

int levels_key(int code, bool *bright)
{
	*bright = levels_alt();
	if (code == KEY_VOLUMEUP)   return +1;
	if (code == KEY_VOLUMEDOWN) return -1;
	return 0;
}

/* The pad is grabbed while native_menu is up over a frozen child, so the
 * child's own read does not hold every press and act on them when it thaws. */
bool child_hide(pid_t pid)
{
	(void)pid;
	if (fd_pad < 0 || ioctl(fd_pad, EVIOCGRAB, 1) != 0) {
		fprintf(stderr, "run: pad grab failed: %s\n", strerror(errno));
		return false;
	}
	memset(hat_dir, 0, sizeof hat_dir);
	return true;
}

void child_restore(pid_t pid, bool show)
{
	(void)pid; (void)show;
	if (fd_pad >= 0) ioctl(fd_pad, EVIOCGRAB, 0);
	memset(hat_dir, 0, sizeof hat_dir);
}

/* Native PICO-8's quiet flag; nothing reads it here. */
/* Native PICO-8 quiet while Muse plays (plorpos-7ny.35), the Brick's way:
 * pico8sdl.so, preloaded into it, writes silence over its audio while this
 * flag exists. On tmpfs: no card writes. */
#define PICO8_QUIET "/tmp/plorpos-pico8-quiet"   /* tools/pico8sdl.c's too */

void child_quiet(pid_t pid, bool on)
{
	static pid_t q_pid;
	static int q_said = -1;

	if (pid != q_pid) { q_pid = pid; q_said = -1; }
	if (q_said == (int)on) return;
	if (on) {
		int fd = open(PICO8_QUIET, O_WRONLY | O_CREAT, 0644);
		if (fd < 0) return;
		close(fd);
	} else if (unlink(PICO8_QUIET) != 0 && errno != ENOENT) {
		return;
	}
	q_said = on;
	fprintf(stderr, "run: child %s\n", on ? "quiet" : "heard");
}

static void pad_read(in_state *st)
{
	struct input_event ev;

	if (menu_pulse) { set_btn(st, IN_MENU, false); menu_pulse = false; }
	while (fd_pad >= 0 && read(fd_pad, &ev, sizeof ev) == (ssize_t)sizeof ev) {
		if (ev.type == EV_ABS && (ev.code == ABS_HAT0X || ev.code == ABS_HAT0Y)) {
			bool *d = &hat_dir[ev.code == ABS_HAT0X ? 0 : 2];
			d[0] = ev.value < 0;
			d[1] = ev.value > 0;
			continue;
		}
		if (ev.type != EV_KEY || ev.value == 2) continue;
		bool down = ev.value == 1;
		in_button vol = IN_NONE, bright = IN_NONE;

		if (ev.code == CODE_MENU) {
			menu_down = down;
			if (down) { menu_sent = menu_spent = false; continue; }
			if (menu_sent) set_btn(st, IN_MENU, false);
			else if (!menu_spent) { set_btn(st, IN_MENU, true); menu_pulse = true; }
			menu_sent = false;
			continue;
		}
		if (ev.code == KEY_VOLUMEUP)   { vol = IN_VOLUP; bright = IN_BRIGHTUP; }
		if (ev.code == KEY_VOLUMEDOWN) { vol = IN_VOLDN; bright = IN_BRIGHTDN; }
		if (vol != IN_NONE) {
			/* A press goes to one or the other by Menu; a release lets go
			 * of both, so letting go of Menu first strands nothing. */
			if (down && menu_down) menu_spent = true;
			if (down) set_btn(st, menu_down ? bright : vol, true);
			else { set_btn(st, vol, false); set_btn(st, bright, false); }
			continue;
		}
		if (down && menu_down && !menu_sent && !menu_spent) {
			set_btn(st, IN_MENU, true);
			menu_sent = true;
		}
		for (size_t k = 0; k < sizeof padmap / sizeof padmap[0]; k++)
			if (padmap[k].code == ev.code) set_btn(st, padmap[k].b, down);
	}
	for (int i = 0; i < 4; i++)
		set_btn(st, (in_button)(IN_LEFT + i), hat_dir[i]);
}

/* POWER, and the lid closing as a tap of it: down now, up on the next poll. */
static void power_read(in_state *st)
{
	static bool lid_pulse;
	struct input_event ev;

	if (lid_pulse) { set_btn(st, IN_POWER, false); lid_pulse = false; }
	while (fd_power >= 0 && read(fd_power, &ev, sizeof ev) == (ssize_t)sizeof ev) {
		if (ev.type != EV_KEY || ev.value == 2) continue;
		if (ev.code == KEY_POWER) set_btn(st, IN_POWER, ev.value == 1);
		else if (ev.code == LID_CLOSE_KEY && ev.value == 1) {
			set_btn(st, IN_POWER, true);
			lid_pulse = true;
		}
	}
}

void plat_input_poll(in_state *st)
{
	SDL_Event e;

	memset(st->pressed, 0, sizeof st->pressed);
	if (g_terminating) st->quit_requested = true;
	/* The jack, on the frame every screen runs and a game does not - see
	 * the Brick's plat_input_poll for why that is the ownership boundary. */
	plat_audio_jack_poll();
	while (SDL_PollEvent(&e))
		if (e.type == SDL_QUIT) st->quit_requested = true;
	pad_read(st);
	power_read(st);
}

/* No LEDs to darken. */
void plat_leds_off(void) { }

/* ---- volume and brightness ---------------------------------------------
 *
 * The codec's level is `lineout volume`, 0-31 at 1.5 dB a step with 31 at
 * 0 dB and 0 a mute (its TLV, unlike the Brick's, is the right way up).
 * BaseOS's asound.conf opens every stream with the speaker and line-out
 * switches on and `digital volume` pinned at 63, so those stay its business.
 *
 * PROVISIONAL: position 1 is raw VOL_RAW_FLOOR, ~45 dB down like the
 * Brick's speaker window, and one ladder serves speaker and headphones.
 * Both get set by ear with the jack work (plorpos-7ny.6). */
#define LINEOUT_CTL   "lineout volume"
#define VOL_RAW_TOP   31
#define VOL_RAW_FLOOR 3
#define VOL_MAX       PLAT_VOL_MAX

/* The Allwinner disp2 engine, as on the Brick: no /sys/class/backlight. */
#define DISP_LCD_SET_BRIGHTNESS 0x102
#define DISP_LCD_GET_BRIGHTNESS 0x103
/* The Brick's ladder and diatom's, until judged on this panel. */
static const unsigned char bright_ladder[] = {
	2, 4, 8, 16, 32, 48, 72, 96, 128, 160, 192, 255
};
#define BRIGHT_MAX ((int)(sizeof bright_ladder / sizeof bright_ladder[0]) - 1)
_Static_assert(BRIGHT_MAX == PLAT_BRIGHT_MAX, "bright_ladder vs PLAT_BRIGHT_MAX");

/* The jack: the PMIC's speaker state, 0 while a plug is in. extcon0's
 * HEADPHONE never moves on the SP (both watched while the user replugged,
 * 2026-10-06). */
#define JACK_STATE "/sys/class/power_supply/axp2202-battery/spk_state"

int mixer_fd = -1, disp_fd = -1;
int cur_vol = -1, cur_bright = -1;
static int jack_fd = -1, jack_was = -1;

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void levels_save(void)
{
	db_set_int(db_dev(), "volume", cur_vol);
	db_set_int(db_dev(), "brightness", cur_bright);
	db_write_boot_env();
}

/* From the kernel UAPI (sound/asound.h), as platform_brick.c vendors it: the
 * union at full size, because its size is baked into the request number. */
struct pl_ctl_elem_id {
	unsigned int  numid;
	int           iface;
	unsigned int  device;
	unsigned int  subdevice;
	unsigned char name[44];
	unsigned int  index;
};
struct pl_aes_iec958 {
	unsigned char status[24], subcode[147], pad, dig_subframe[4];
};
struct pl_ctl_elem_value {
	struct pl_ctl_elem_id id;
	unsigned int indirect: 1;
	union {
		union { long value[128]; long *value_ptr; } integer;
		union { long long value[64]; long long *value_ptr; } integer64;
		union { unsigned int item[128]; unsigned int *item_ptr; } enumerated;
		union { unsigned char data[512]; unsigned char *data_ptr; } bytes;
		struct pl_aes_iec958 iec958;
	} value;
	unsigned char reserved[128];
};
#define PL_CTL_ELEM_WRITE  _IOWR('U', 0x13, struct pl_ctl_elem_value)

static void ctl_set(const char *name, long val)
{
	struct pl_ctl_elem_value v;

	if (mixer_fd < 0) return;
	memset(&v, 0, sizeof v);
	v.id.iface = 2;                                 /* SNDRV_CTL_ELEM_IFACE_MIXER */
	snprintf((char *)v.id.name, sizeof v.id.name, "%s", name);
	v.value.integer.value[0] = val;
	if (ioctl(mixer_fd, PL_CTL_ELEM_WRITE, &v) < 0)
		fprintf(stderr, "settings: mixer rejected '%s' = %ld\n", name, val);
}

static int jack_present(void)
{
	char s[32];
	ssize_t n;

	if (jack_fd < 0) return 0;
	n = pread(jack_fd, s, sizeof s - 1, 0);
	if (n <= 0) return 0;
	s[n] = '\0';
	return s[0] == '0';
}

void apply_volume(int v)
{
	/* 0 is the register's mute; position 1 is the floor and 20 the top. */
	long raw = v <= 0 ? 0
	         : VOL_RAW_FLOOR + ((long)(v - 1) * (VOL_RAW_TOP - VOL_RAW_FLOOR) +
	                            (VOL_MAX - 1) / 2) / (VOL_MAX - 1);

	jack_was = jack_present();
	cur_vol = v;
	ctl_set(LINEOUT_CTL, raw);
}

bool plat_headphones_present(void) { return jack_present() != 0; }

void plat_audio_jack_poll(void)
{
	if (!aout_should_reapply(jack_was, jack_present() != 0, cur_vol >= 0))
		return;
	apply_volume(cur_vol);
}

void jack_forget(void) { jack_was = -1; }

/* No mute switch on the H700s. */
bool plat_mute_poll(bool own_volume) { (void)own_volume; return false; }
bool plat_muted(void) { return false; }
void mute_forget(void) { }
void plat_mute_switch_lock(bool lock) { (void)lock; }
bool plat_hold_switch(void) { return false; }

void apply_brightness(int b)
{
	unsigned long a[4] = { 0, 0, 0, 0 };

	cur_bright = b;
	if (disp_fd < 0) return;
	a[1] = bright_ladder[b];
	ioctl(disp_fd, DISP_LCD_SET_BRIGHTNESS, a);
}

/* Dark, not dim: rung 0 is still lit. cur_bright is kept for the way back. */
void backlight_off(void)
{
	unsigned long a[4] = { 0, 0, 0, 0 };

	if (disp_fd >= 0) ioctl(disp_fd, DISP_LCD_SET_BRIGHTNESS, a);
}

void plat_settings_init(void)
{
	int v, b;

	mixer_fd = open("/dev/snd/controlC0", O_RDWR | O_CLOEXEC);
	disp_fd  = open("/dev/disp", O_RDWR | O_CLOEXEC);
	jack_fd  = open(JACK_STATE, O_RDONLY | O_CLOEXEC);
	if (mixer_fd < 0) fprintf(stderr, "settings: no /dev/snd/controlC0\n");
	if (disp_fd  < 0) fprintf(stderr, "settings: no /dev/disp\n");
	if (jack_fd  < 0) fprintf(stderr, "settings: no %s\n", JACK_STATE);

	v = db_get_int(db_dev(), "volume", -1);
	b = db_get_int(db_dev(), "brightness", -1);
	/* Whatever the panel is at already, to the nearest rung, so the first
	 * OSD tells the truth with nothing saved. */
	if (b < 0 && disp_fd >= 0) {
		int raw = ioctl(disp_fd, DISP_LCD_GET_BRIGHTNESS, (unsigned long[4]){ 0, 0, 0, 0 });
		if (raw >= 0) {
			b = 0;
			for (int i = 1; i <= BRIGHT_MAX; i++)
				if (abs(bright_ladder[i] - raw) < abs(bright_ladder[b] - raw)) b = i;
		}
	}
	/* No saved volume is "unknown" (-1), as on the Brick: the codec keeps
	 * what it has and nothing is sent to diatom. Defaulting it to the middle
	 * put every game back at 50% when its menu closed (2026-10-06). */
	cur_vol    = v >= 0 ? clampi(v, 0, VOL_MAX) : -1;
	cur_bright = b >= 0 ? clampi(b, 0, BRIGHT_MAX) : BRIGHT_MAX / 2;
	if (cur_vol >= 0) apply_volume(cur_vol);
	apply_brightness(cur_bright);
}

/* ---- sleep -----------------------------------------------------------
 *
 * Suspend-to-RAM as BaseOS documents it for a frontend (its docs/05): write
 * "mem" to /sys/power/state, which blocks until the device wakes. On the SP,
 * Super Standby (os_sleep = 16, which BaseOS also sets at boot) stops the USB
 * host side and leaves POWER as the wake key - opening the lid alone does not
 * wake it. ALSA's mixer is BaseOS's to save and restore.
 *
 * Reached only from platform.c's light sleep, after the Suspend Timeout,
 * which treats false as "no suspend here" and powers off - so false is kept
 * for a kernel that truly cannot, not for a write that was merely refused. */
#define POWER_STATE "/sys/power/state"
#define OS_SLEEP    "/sys/class/power_supply/axp2202-battery/os_sleep"

bool plat_sleep_supported(void)
{
	char st[64] = "";
	int fd = open(POWER_STATE, O_RDONLY | O_CLOEXEC);
	ssize_t n = fd >= 0 ? read(fd, st, sizeof st - 1) : -1;

	if (fd >= 0) close(fd);
	if (n > 0) st[n] = '\0';
	return strstr(st, "mem") != NULL && access(POWER_STATE, W_OK) == 0;
}

/* One of TortOS/radio.sh's functions (sd/h700/radio.sh), as the Brick's
 * radio_sh_call does it: the script's path as $0, never pasted into the
 * command, and `fn` always a literal from below. Waited for, or detached for
 * the bring-up after a wake, so its seconds never hold the wake up. */
static void radio_call(const char *fn, bool background)
{
	char script[512], cmd[64];
	char *argv[5];
	pid_t pid;

	snprintf(script, sizeof script, "%s/radio.sh", P_ROOT);
	snprintf(cmd, sizeof cmd, ". \"$0\" && %s", fn);
	argv[0] = (char *)"/bin/sh";
	argv[1] = (char *)"-c";
	argv[2] = cmd;
	argv[3] = script;
	argv[4] = NULL;
	if (background) { plat_spawn_detached(argv, NULL, NULL); return; }
	if ((pid = fork()) < 0) return;
	if (pid == 0) {
		int null = open("/dev/null", O_RDONLY);
		if (null >= 0) { dup2(null, 0); close(null); }
		execv(argv[0], argv);
		_exit(127);
	}
	waitpid(pid, NULL, 0);
}

static bool suspend_mem(void);

/* Bluetooth down for the sleep and back after it, as on the Brick: the links
 * closed while bluetoothd still owns them, before the kernel suspends under
 * them (radio.sh's bt_off says why the order matters). hci0 exists exactly
 * while the radio is attached, so it is the "was up" answer without a fork.
 * Wi-Fi is not touched here, as before. */
bool plat_sleep(void)
{
	bool bt_was_up, slept;

	if (!plat_sleep_supported()) return false;
	bt_was_up = access("/sys/class/bluetooth/hci0", F_OK) == 0;
	if (bt_was_up) radio_call("bt_off", false);
	slept = suspend_mem();
	if (bt_was_up) radio_call("bt_on", true);
	return slept;
}

static bool suspend_mem(void)
{
	int tries;

	{
		int fd = open(OS_SLEEP, O_WRONLY | O_CLOEXEC);   /* SP only */
		if (fd >= 0) {
			if (write(fd, "16", 2) < 0) { /* BaseOS set it at boot anyway */ }
			close(fd);
		}
	}
	sync();
	/* A pending wake source makes the kernel refuse with EBUSY rather than
	 * sleep; a few tries a second apart get past a stray one. POWER pressed
	 * meanwhile is the player waking it, which is what a resume gives. */
	for (tries = 0; tries < 5; tries++) {
		int fd = open(POWER_STATE, O_WRONLY | O_CLOEXEC);
		ssize_t n = fd >= 0 ? write(fd, "mem", 3) : -1;

		if (fd >= 0) close(fd);
		if (n == 3) {
			fprintf(stderr, "sleep: resumed\n");
			return true;
		}
		fprintf(stderr, "sleep: suspend refused: %s\n", strerror(errno));
		if (power_key_within(1000, 1)) return true;
	}
	return false;
}

/* ---- battery ---- */

bool plat_battery(int *pct, bool *charging)
{
	return battery_read("/sys/class/power_supply/axp2202-battery", pct, charging);
}
