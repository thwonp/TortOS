/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The TrimUI Brick and Brick Pro: everything platform.c would say differently
 * about another device's hardware. What the two halves share privately is
 * platform_dev.h. The host build (mk/native.mk) compiles this file too. */
#include "atomic.h"
#include "db.h"
#include "platform.h"
#include "audioout.h"

#include <dlfcn.h>
#include <glob.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#ifdef __linux__
#include <linux/input.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <stdarg.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "platform_dev.h"

/* The front function keys report on the gamepad node as BTN_THUMBL/BTN_THUMBR
 * (317/318). SDL maps them to joystick buttons 9 and 10, which emulators call
 * L3/R3 -- which is why the in-game input map has to drop L3/R3, or every
 * brightness press also reaches the core. */
#define CODE_FN_LEFT  317
#define CODE_FN_RIGHT 318

/* The Brick Pro (TG4040) is the same machine with sticks. There 317/318 are
 * the stick clicks, and its two function keys arrive on the same node as
 * KEY_F1/KEY_F2 - SDL buttons 11 and 12, which the Brick never sends. Its
 * Home key is the Brick's MENU, button 8. Pressed and logged 2026-09-28. The
 * pad also advertises KEY_HOMEPAGE (button 15), which no key sends. */
#define CODE_PRO_FN_LEFT  KEY_F1
#define CODE_PRO_FN_RIGHT KEY_F2

/* Where everything is, unless TORTOS_* says otherwise - see paths_init(). */
const char *P_ROOT = "/mnt/SDCARD/TortOS";
const char *P_CARD = "/mnt/SDCARD";
const char *P_ROMS = "/mnt/SDCARD/Roms";
const char *P_USERDATA = "/mnt/SDCARD/.userdata/tg3040";
const char *P_SHARED = "/mnt/SDCARD/.userdata/shared";
/* Over The Hare's page. On the card rather than in the binary so it can be
 * restyled with a text editor and a reload, which is the whole argument for
 * a file-transfer feature existing at all. */
const char *P_WEB = "/mnt/SDCARD/TortOS/res/web";
const char *P_FONT = "/mnt/SDCARD/TortOS/menu.ttf";

/* What a game's process is told about the device - see build_child_env(). */
const char *const plat_child_env[] = {
	"PLATFORM=tg3040", "DEVICE=brick",
	"SDCARD_PATH=/mnt/SDCARD",
	"BIOS_PATH=/mnt/SDCARD/Bios",
	"CHEATS_PATH=/mnt/SDCARD/Cheats",
	"SAVES_PATH=/mnt/SDCARD/Saves",
	NULL
};
const char plat_child_libpath[] = ":/usr/trimui/lib";

/* The brightness keys, as the Controls page names them. */
const char *plat_bright_keys(void) { return plat_is_brick_pro() ? "FN1/FN2" : "F1/F2"; }

/* Which of the two, from the line the boot script already trusts: cpuinfo's
 * hwserial names the model and nothing else on the device does. */
bool plat_is_brick_pro(void)
{
	static int pro = -1;
	if (pro < 0) {
		char line[256];
		FILE *f = fopen("/proc/cpuinfo", "r");
		pro = 0;
		while (f && fgets(line, sizeof line, f))
			if (strncmp(line, "hwserial", 8) == 0 && strstr(line, "TG4040"))
				pro = 1;
		if (f) fclose(f);
	}
	return pro;
}

bool plat_has_stick(void) { return plat_is_brick_pro(); }

/* SDL joystick button indices on the Brick's "TRIMUI Player1" device.
 *
 * SDL numbers these in ascending evdev-code order and the device declares
 * 304 305 307 308 310 311 314 315 316 317 318, so index 2 is BTN_NORTH and
 * index 3 is BTN_WEST. The buttons printed on this shell are Y and X
 * respectively - crossed from the evdev names, as A and B already are.
 *
 * These were swapped here on 2026-08-29 and swapped straight back, because
 * the change was reasoned from the evdev names rather than tested, and Eric
 * pressing the buttons settled it in one try. The table below is correct;
 * this note exists so the next person to notice that JOY_Y sits at the index
 * called BTN_NORTH does not "fix" it again. */
enum {
	JOY_B = 0, JOY_A = 1, JOY_Y = 2, JOY_X = 3,
	JOY_L1 = 4, JOY_R1 = 5, JOY_SELECT = 6, JOY_START = 7,
	JOY_MENU = 8, JOY_L3 = 9, JOY_R3 = 10,
	JOY_VOLDN = 13, JOY_VOLUP = 14,
};

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Joystick *joy;

static int fd_keys = -1;  /* sunxi-keyboard: volume keys */
static int fd_joy = -1;   /* TRIMUI Player1: raw, for the front F1/F2 keys */

/* The d-pad and the left stick both drive IN_LEFT..IN_DOWN; each keeps its
 * own state so letting go of one does not release a direction the other
 * still holds. Order: left, right, up, down. */
static bool hat_dir[4], stick_dir[4];

/* TORTOS_INPUT_DEBUG=1 logs raw evdev codes and SDL button indices, so one
 * press tells you exactly which device a control arrives on. */
static int dbg_input;

SDL_Renderer *plat_renderer(void) { return ren; }

bool plat_video_init(void)
{
	/* The Brick's panel, unless a host build is told to be another device's:
	 * TORTOS_WINDOW=1600x1440 lays out and renders as the GKD would. */
	const char *ws = getenv("TORTOS_WINDOW");
	int ww = TORTOS_SCREEN_W, wh = 768;

	if (ws && sscanf(ws, "%dx%d", &ww, &wh) != 2) { ww = TORTOS_SCREEN_W; wh = 768; }
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
#ifdef __linux__
	SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
#endif
	if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "video init: %s\n", SDL_GetError());
		return false;
	}
	SDL_ShowCursor(SDL_DISABLE);
	win = SDL_CreateWindow("plorpOS", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
	                       ww, wh, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
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
	/* Whether vsync was GRANTED, not whether it was asked for. PRESENTVSYNC
	 * is a request the driver may decline in silence, and the shelf loop has
	 * no delay in it - so a declined request is not a slower animation, it is
	 * an uncapped loop presenting mid-scanout. This line existed and read
	 * info.name only, which is how the request came to be talked about as if
	 * it were the result. */
	SDL_RendererInfo info;
	if (SDL_GetRendererInfo(ren, &info) == 0)
		fprintf(stderr, "renderer: %s, driver: %s, vsync %s\n",
		        info.name, SDL_GetCurrentVideoDriver(),
		        (info.flags & SDL_RENDERER_PRESENTVSYNC) ? "granted"
		                                                 : "DECLINED (requested)");
	/* Logged for diagnosis: anything drawn offscreen needs it. */
	if (SDL_GetRendererInfo(ren, &info) == 0)
		fprintf(stderr, "renderer: render-to-texture %s\n",
		        (info.flags & SDL_RENDERER_TARGETTEXTURE) ? "available"
		                                                  : "MISSING");
	return true;
}

void plat_video_quit(void)
{
	if (ren) { SDL_DestroyRenderer(ren); ren = NULL; }
	if (win) { SDL_DestroyWindow(win); win = NULL; }
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

/* One display, no compositor: whoever presents last is on glass. */
void screen_yield(bool to_game) { (void)to_game; }

static void open_joystick(void)
{
	for (int i = 0; i < SDL_NumJoysticks(); i++) {
		SDL_Joystick *j = SDL_JoystickOpen(i);
		if (!j) continue;
		fprintf(stderr, "joystick %d: %s (%d buttons, %d hats, %d axes)\n",
		        i, SDL_JoystickName(j), SDL_JoystickNumButtons(j),
		        SDL_JoystickNumHats(j), SDL_JoystickNumAxes(j));
		if (!joy) joy = j; /* the first is TRIMUI Player1 */
	}
}

bool plat_input_init(void)
{
	char marker[512];
	snprintf(marker, sizeof marker, "%s/.input_debug", P_ROOT);
	dbg_input = getenv("TORTOS_INPUT_DEBUG") != NULL || access(marker, F_OK) == 0;
	if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) != 0) {
		fprintf(stderr, "joystick init: %s\n", SDL_GetError());
		return false;
	}
	SDL_JoystickEventState(SDL_ENABLE);
	open_joystick();
#ifdef __linux__
	if (fd_power < 0) fd_power = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
	if (fd_keys < 0) fd_keys = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
	if (fd_joy < 0) fd_joy = open("/dev/input/event3", O_RDONLY | O_NONBLOCK);
#endif
	return true;
}

/* Throw away everything that queued up while a game owned the screen. The
 * joystick stayed open through the game, so SDL has been collecting presses
 * meant for it, and the raw descriptors have too. */
void plat_input_flush(void)
{
	SDL_Event e;
	SDL_PumpEvents();
	while (SDL_PollEvent(&e)) { }
	/* Their releases were in what was just thrown away. */
	memset(hat_dir, 0, sizeof hat_dir);
	memset(stick_dir, 0, sizeof stick_dir);
#ifdef __linux__
	{
		struct input_event ev;
		int fds[3], i;
		fds[0] = fd_power; fds[1] = fd_keys; fds[2] = fd_joy;
		for (i = 0; i < 3; i++)
			while (fds[i] >= 0 &&
			       read(fds[i], &ev, sizeof ev) == (ssize_t)sizeof ev) { }
	}
#endif
}

void plat_input_quit(void)
{
	if (joy) { SDL_JoystickClose(joy); joy = NULL; }
	SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
	/* keep the raw descriptors; they are display-independent */
}

/* Not wired: Menu reaches the launcher through the joystick, which is closed
 * while a game runs. The Brick's native PICO-8 is plorpos-gkd.50.13. */
int menu_key(int *code)
{
	*code = 0;
	return -1;
}

/* Not wired either: plorpos-gkd.50.13. */
int levels_fd(void) { return -1; }
bool levels_alt(void) { return false; }

/* No compositor to take a frozen window off the screen, and Menu never gets
 * here anyway (menu_key). plorpos-gkd.50.13. */
bool child_hide(pid_t pid) { (void)pid; return false; }
void child_restore(pid_t pid, bool show) { (void)pid; (void)show; }
/* Not on the Brick yet: plorpos-gkd.50.13. */
void child_quiet(pid_t pid, bool on) { (void)pid; (void)on; }

static in_button map_joy_button(int jb)
{
	switch (jb) {
	case JOY_A: return IN_ACCEPT;
	case JOY_B: return IN_BACK;
	case JOY_X: return IN_X;
	case JOY_Y: return IN_Y;
	case JOY_L1: return IN_L1;
	case JOY_R1: return IN_R1;
	case JOY_START: return IN_START;
	case JOY_SELECT: return IN_SELECT;
	case JOY_MENU: return IN_MENU;
	/* The Pro's left stick click. The plain Brick's 9 is its front
	 * brightness key, read from the raw node in poll_raw_fd. */
	case JOY_L3: return plat_is_brick_pro() ? IN_L3 : IN_NONE;
	case JOY_VOLUP: return IN_VOLUP;
	case JOY_VOLDN: return IN_VOLDN;
	default: return IN_NONE;
	}
}

static void poll_raw_fd(int fd, in_state *st)
{
#ifdef __linux__
	struct input_event ev;
	while (fd >= 0 && read(fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
		if (ev.type != EV_KEY) continue;
		if (dbg_input)
			fprintf(stderr, "[in] raw fd=%d code=%d val=%d\n", fd, ev.code, ev.value);
		in_button b = IN_NONE;
		if (ev.code == KEY_POWER) b = IN_POWER;
		else if (ev.code == KEY_VOLUMEUP) b = IN_VOLUP;
		else if (ev.code == KEY_VOLUMEDOWN) b = IN_VOLDN;
		else if (ev.code == (plat_is_brick_pro() ? CODE_PRO_FN_RIGHT : CODE_FN_RIGHT))
			b = IN_BRIGHTUP;
		else if (ev.code == (plat_is_brick_pro() ? CODE_PRO_FN_LEFT : CODE_FN_LEFT))
			b = IN_BRIGHTDN;
		if (b != IN_NONE && ev.value != 2)
			set_btn(st, b, ev.value == 1);
	}
#else
	(void)fd; (void)st;
#endif
}

/* keyboard fallback so the native dev build is drivable */
static in_button map_key(SDL_Keycode k)
{
	switch (k) {
	case SDLK_LEFT: return IN_LEFT;
	case SDLK_RIGHT: return IN_RIGHT;
	case SDLK_UP: return IN_UP;
	case SDLK_DOWN: return IN_DOWN;
	case SDLK_RETURN: return IN_ACCEPT;
	case SDLK_ESCAPE: return IN_BACK;
	case SDLK_BACKSPACE: return IN_BACK;
	case SDLK_MINUS: return IN_VOLDN;
	case SDLK_EQUALS: return IN_VOLUP;
	default: return IN_NONE;
	}
}

/* Half deflection to press, a third to let go: the gap is what stops a stick
 * resting near the threshold from chattering a direction on and off. */
#define STICK_PRESS   16384
#define STICK_RELEASE 10923

static void stick_axis(bool *neg_pos, int v)
{
	neg_pos[0] = v < -(neg_pos[0] ? STICK_RELEASE : STICK_PRESS);
	neg_pos[1] = v >  (neg_pos[1] ? STICK_RELEASE : STICK_PRESS);
}

static void set_dirs(in_state *st)
{
	set_btn(st, IN_LEFT,  hat_dir[0] || stick_dir[0]);
	set_btn(st, IN_RIGHT, hat_dir[1] || stick_dir[1]);
	set_btn(st, IN_UP,    hat_dir[2] || stick_dir[2]);
	set_btn(st, IN_DOWN,  hat_dir[3] || stick_dir[3]);
	for (int i = 0; i < 4; i++)
		set_btn(st, (in_button)(IN_SLEFT + i), stick_dir[i]);
}

void plat_input_poll(in_state *st)
{
	memset(st->pressed, 0, sizeof st->pressed);
	if (g_terminating) st->quit_requested = true;
	/* The jack, checked here because this is the one thing every screen does
	 * once a frame - and, more to the point, the one thing the launcher stops
	 * doing while a game runs, since it is blocked in plat_resident_wait. That
	 * is exactly the ownership boundary the level needs: whoever is pumping
	 * input owns the volume, so the launcher must not re-apply a level over
	 * Diatom's while Diatom holds it. Diatom polls the same switch on its own
	 * side for the same reason. */
	plat_audio_jack_poll();
	/* And the mute switch, for the same reason and on the same frame. */
	plat_mute_poll(true);
	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		switch (e.type) {
		case SDL_QUIT:
			st->quit_requested = true;
			break;
		case SDL_JOYBUTTONDOWN:
		case SDL_JOYBUTTONUP:
			if (dbg_input && e.type == SDL_JOYBUTTONDOWN)
				fprintf(stderr, "[in] joy button=%d\n", e.jbutton.button);
			set_btn(st, map_joy_button(e.jbutton.button),
			        e.type == SDL_JOYBUTTONDOWN);
			break;
		case SDL_JOYHATMOTION:
			hat_dir[0] = (e.jhat.value & SDL_HAT_LEFT)  != 0;
			hat_dir[1] = (e.jhat.value & SDL_HAT_RIGHT) != 0;
			hat_dir[2] = (e.jhat.value & SDL_HAT_UP)    != 0;
			hat_dir[3] = (e.jhat.value & SDL_HAT_DOWN)  != 0;
			set_dirs(st);
			break;
		case SDL_JOYAXISMOTION:
			/* The Brick Pro's left stick is a second d-pad. The Brick's
			 * virtual pad advertises these axes too but never moves them. */
			if (e.jaxis.axis == 0 || e.jaxis.axis == 1) {
				stick_axis(&stick_dir[e.jaxis.axis == 0 ? 0 : 2], e.jaxis.value);
				set_dirs(st);
			}
			/* L2/R2: axes 2 and 5, resting at -32768 and slamming to
			 * +32767 - digital switches in axis clothing, as diatom reads
			 * them. Only the Hotkeys screen listens. */
			if (e.jaxis.axis == 2 || e.jaxis.axis == 5)
				set_btn(st, e.jaxis.axis == 2 ? IN_L2 : IN_R2, e.jaxis.value > 16384);
			break;
		case SDL_JOYDEVICEADDED:
			if (!joy) open_joystick();
			break;
		case SDL_KEYDOWN:
		case SDL_KEYUP:
			if (e.key.repeat) break;
			set_btn(st, map_key(e.key.keysym.sym), e.type == SDL_KEYDOWN);
			if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_q)
				st->quit_requested = true;
			break;
		}
	}
	poll_raw_fd(fd_power, st);
	poll_raw_fd(fd_keys, st);
	poll_raw_fd(fd_joy, st);
}

#ifdef __linux__
static void write_str(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0) return;
	if (write(fd, val, strlen(val)) < 0) { /* best effort */ }
	close(fd);
}

/* True when a sysfs attribute reads 0. */
static bool reads_zero(const char *path)
{
	char c;
	int fd = open(path, O_RDONLY);
	if (fd < 0) return false;
	ssize_t n = read(fd, &c, 1);
	close(fd);
	return n == 1 && c == '0';	/* sysfs prints plain decimal */
}
#endif

void plat_leds_off(void)
{
#ifdef __linux__
	/* Stop the animation engine, scale every group to zero, then zero all 23
	 * raw channels (both models have 23) directly -- so it holds whatever state the stock input
	 * daemon left the engine in. Only channels that are lit get written: each
	 * raw write is a transfer on the LED controller, and 69 of them in a row
	 * overflow its FIFO. On the Brick Pro that interrupt storm starves the
	 * stick chip's i2c bus, and the stock kernel panics on the resulting
	 * timeout (TortOS-pky.9). Reads cost the controller nothing. */
	write_str("/sys/class/led_anim/effect_enable", "0");
	static const char *groups[] = { "l", "r", "lr", "m", "f1", "f2", "rear" };
	char path[96];
	for (size_t i = 0; i < sizeof groups / sizeof *groups; i++) {
		snprintf(path, sizeof path, "/sys/class/led_anim/effect_rgb_hex_%s", groups[i]);
		write_str(path, "000000 ");
	}
	write_str("/sys/class/led_anim/max_scale", "0");
	write_str("/sys/class/led_anim/max_scale_lr", "0");
	write_str("/sys/class/led_anim/max_scale_f1f2", "0");
	/* The Brick Pro's trigger ring; absent on the Brick, where the write
	 * just fails to open. */
	write_str("/sys/class/led_anim/max_scale_rear", "0");
	for (int n = 0; n < 23; n++) {
		for (const char *c = "rgb"; *c; c++) {
			snprintf(path, sizeof path,
			         "/sys/class/leds/sunxi_led%d%c/brightness", n, *c);
			if (!reads_zero(path)) write_str(path, "0");
		}
	}
#endif
}

/* ---- volume and brightness, straight at the hardware ----------------------
 *
 * TortOS's own code, against the same two device interfaces the emulator uses.
 * It replaced a third-party settings library, which bought three things beyond
 * independence:
 *
 *   - The scales MATCH. Diatom drives volume as 21 positions and brightness
 *     as a 12-rung ladder; libmsettings used 0-20 and 0-10. Every level
 *     crossing the socket had to be rescaled, and a rescale is where an
 *     off-by-one hides. Both sides now speak the same ladder and the
 *     conversion is the identity.
 *   - Settings persist in TortOS's own file rather than the stock firmware's
 *     /mnt/UDISK/system.json, which is TrimUI's state and not ours to own.
 *   - One fewer binary on the card.
 *
 * The device facts here were measured, not guessed, and two of them are traps:
 *
 *   `digital volume` (0-63) is the speaker level and is INVERTED - 0 is
 *   loudest, 63 is quietest - while the driver's own metadata advertises the
 *   opposite. It also declares mute=0, so its minimum is maximum attenuation
 *   (about -74 dB) rather than silence; the speaker switch carries the last
 *   step.
 *
 *   `Headphone Volume` is NOT a speaker level. Raising it routes audio to the
 *   jack and mutes the speakers. It stays at zero.
 *
 * The backlight has no /sys/class/backlight on this device; it is the
 * Allwinner disp2 engine, addressed with plain command numbers (not _IOWR) and
 * an unsigned long[4] argument block - the same call tools/setbright.c makes
 * before the launcher exists. */

#ifdef __linux__
#include <sys/ioctl.h>

/* From the kernel UAPI (sound/asound.h), vendored rather than depended on.
 * Only the integer case is needed; the union is declared at full size because
 * its size is what _IOWR bakes into the request number, and a wrong layout
 * produces a wrong request number rather than a failed call. */
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
#define PL_CTL_ELEM_READ   _IOWR('U', 0x12, struct pl_ctl_elem_value)
#define PL_CTL_ELEM_WRITE  _IOWR('U', 0x13, struct pl_ctl_elem_value)

#define GAIN_CTL     "digital volume"
#define GAIN_RAW_MAX 63          /* 0 is loudest, 63 quietest */

/* Only the top of that register is worth spending on the volume keys. The
 * control is 1.16 dB per step, so using all 63 puts 73 dB across 21 positions -
 * 3.65 dB a press, which made 60% of the scale -29 dB and the bottom two thirds
 * inaudible. That is not a curve shape problem: dB is already the perceptually
 * even axis, the same reason the brightness ladder below is geometric. It is a
 * RANGE problem, and 73 dB is simply more than a handheld speaker has.
 *
 * Measured on the device 2026-08-28 with a 440 Hz tone at -1.4 dBFS captured on
 * its own microphone. Coarse sweep, room baseline ~40 rms:
 *
 *   raw 0   10034 rms   201x room      raw 31    114 rms   2.3x room
 *   raw 16   1485 rms    30x room      raw 47     34 rms   inaudible
 *
 * The floor was first set at 34 by reading that table, and Eric reported the
 * bottom of the scale as dead. A finer sweep says why - room baseline 33:
 *
 *   raw 18  1091 rms  32.7x      raw 30    85 rms   2.5x
 *   raw 22   404 rms  12.1x      raw 34    55 rms   1.6x
 *   raw 26   173 rms   5.2x
 *
 * 34 is 1.6x room noise, so the last few positions were all sitting in the
 * noise and were indistinguishable from each other. 26 is 5.2x: quiet, and
 * unmistakably present. That is the floor, ~1.5 dB a press across the 21
 * positions.
 *
 * Those rms numbers were captured with HP_CTL already at 0 - the sweep script
 * sets it before measuring - so they describe the chain as it behaves now, and
 * the 18 dB that mixer_defaults() restored was never inside them. A note here
 * briefly claimed the opposite. That was inferred rather than read off the
 * script which produced the table, and the script was on disk the whole time.
 * The 18 dB gap was between the SWEEP and gameplay, not inside the sweep, which
 * is exactly why the table looked sane while the device sounded quiet.
 *
 * What is weak here is the instrument, not the conditions. Those readings came
 * from a microphone across the room, where raw 26 is 5.2x the room and raw 34
 * is indistinguishable from it. A handheld at arm's length is a different
 * question: what reads as silence over there is plainly audible in your hands.
 * That is why the bottom of the scale was still reported as too loud once the
 * chain was fixed, and why the floor below is now set by ear rather than by
 * that table.
 *
 * That session had ONE WORKING SPEAKER, which nobody knew at the time. The
 * quiet channel turned out on 2026-09-02 to be a loose connection on the PCB;
 * it was resoldered and both now play evenly. So 39 was originally judged
 * against roughly half this device's output.
 *
 * It stood anyway. Re-heard on the repaired hardware the same day - the quiet
 * end, the balance and the general sound - and called right, so the constant
 * now has two independent confirmations rather than one lucky derivation. Do
 * not weaken it back to a guess on the strength of the history above.
 *
 * 39, chosen on the device on 2026-08-31 with a game playing, stepping the
 * register down until Eric called it: raw 37 is barely audible and is where he
 * wanted position 1. 39 is the constant that puts position 1 on 37 in this
 * ladder AND in Diatom's, which round differently; 26 put it on 25. Position 20
 * still lands on raw 0, so nothing about maximum changes.
 *
 * It costs resolution. Twenty positions across 39 is about 2.3 dB a press
 * rather than 1.5, bought with a range of 45 dB rather than 30. That is the
 * trade worth making: 30 dB down is not quiet in a room that is quiet, which
 * a microphone across the room could not tell us and an ear could. */
#define GAIN_RAW_USABLE 39

/* Headphones need a different window, not the same one moved.
 *
 * Set by ear on 2026-09-02 with a game playing and a plug in the jack, the same
 * method that produced 39. The ceiling first: above raw 8 it is uncomfortable,
 * so 8 is where position 20 belongs. Then the floor, stepping down - 37, 45,
 * 49, 53 and 57 were each still too loud to be a minimum, and 61 was called
 * right.
 *
 * So the jack spends 53 register steps where the speaker spends 39, and an
 * offset cannot express that. An offset from the measured ceiling would have
 * put position 1 on 47, which was rejected on the way past. That is physically
 * unsurprising: headphones are far more efficient than this speaker, so the
 * same twenty presses have to cover more ground.
 *
 * The cost is resolution. 53 steps across 20 positions is about 3.1 dB a press
 * against the speaker's 2.3. Giving the jack its own number of positions would
 * fix that and is not worth it - 21 positions are shared verbatim with Diatom
 * and launch.sh so a level crossing the socket needs no conversion, and that
 * property is worth more than 0.8 dB of granularity.
 *
 * Both pairs are tied to one game's mix, exactly as 39 was. A quieter game sits
 * under this floor. Consistent with what was already here, not worse. */
#define SPK_RAW_TOP     0
#define SPK_RAW_BOTTOM  GAIN_RAW_USABLE
#define HP_RAW_TOP      8
#define HP_RAW_BOTTOM   61

#define SPEAKER_CTL  "HpSpeaker Switch"   /* the speaker's only true mute */
#define HP_CTL       "Headphone Volume"   /* 0-7, 6 dB a step, INVERTED */
#define HP_CTL_QUIET 7                    /* its quiet end, for the cut */
#define SWAP_CTL     "DAC Swap"           /* 1 crosses left and right */
#define VOL_MAX      PLAT_VOL_MAX         /* 21 positions, 0..20 - Diatom's scale */

#define DISP_LCD_SET_BRIGHTNESS 0x102
#define DISP_LCD_GET_BRIGHTNESS 0x103

/* Perceived brightness is proportional, not linear, so the rungs are
 * geometric. The first is the panel's measured floor: 0 and 1 are black on
 * this display and the driver clamps neither. Identical to Diatom's ladder,
 * which is what makes the level handoff exact. */
static const unsigned char bright_ladder[] = {
	2, 4, 8, 16, 32, 48, 72, 96, 128, 160, 192, 255
};
#define BRIGHT_MAX ((int)(sizeof bright_ladder / sizeof bright_ladder[0]) - 1)
/* The table is the definition; the header only publishes its extent. If they
 * ever disagree this stops the build rather than shipping a rescale that is
 * quietly off by a rung, which is the bug that put the maximum in a header. */
_Static_assert(BRIGHT_MAX == PLAT_BRIGHT_MAX, "bright_ladder vs PLAT_BRIGHT_MAX");

int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

int mixer_fd = -1, disp_fd = -1;
int cur_vol = -1, cur_bright = -1;

static int ctl_io(const char *name, long *val, int write)
{
	struct pl_ctl_elem_value v;

	if (mixer_fd < 0) return -1;
	memset(&v, 0, sizeof v);
	v.id.iface = 2;                                 /* SNDRV_CTL_ELEM_IFACE_MIXER */
	snprintf((char *)v.id.name, sizeof v.id.name, "%s", name);
	if (write) {
		v.value.integer.value[0] = *val;
		return ioctl(mixer_fd, PL_CTL_ELEM_WRITE, &v);
	}
	if (ioctl(mixer_fd, PL_CTL_ELEM_READ, &v) < 0) return -1;
	*val = v.value.integer.value[0];
	return 0;
}

/* Every nudge of the rocker lands here, including while a game is running, so
 * what this costs is a frame-budget question rather than a tidiness one.
 * Measured on the card 2026-09-05: the atomic_open/atomic_commit this replaces
 * was 3.03 ms median with a 9.27 ms tail, against a 16.7 ms frame. The two
 * writes below are 0.48 ms each with a 0.59 ms tail. tools/storeprobe.c. */
void levels_save(void)
{
	db_set_int(db_dev(), "volume", cur_vol);
	db_set_int(db_dev(), "brightness", cur_bright);
	/* launch.sh sets the panel from this before the boot animation and cannot
	 * read a database, so the export has to follow a brightness change.
	 *
	 * It costs nothing when nothing in it moved - db_write_boot_env compares
	 * against what it last wrote and returns. That matters because VOLUME is
	 * not in boot.env at all, and calling this unconditionally put a file
	 * replacement back on the volume rocker: 3.03 ms median, 9.27 ms at the
	 * tail, while a game is running. Which is the exact cost moving settings
	 * into the database was meant to remove. */
	db_write_boot_env();
}

/* Write a control and complain if it does not land. Discarding this return is
 * what cost the project 18 dB for its whole life; see mixer_defaults(). */
static void ctl_set(const char *name, long val)
{
	if (ctl_io(name, &val, 1) < 0)
		fprintf(stderr, "settings: mixer rejected '%s' = %ld\n", name, val);
}

/* Codec-wide state that the volume keys do not own, set once at init.
 *
 * HP_CTL is 0-7 at 6 dB a step and is INVERTED, exactly like GAIN_CTL above:
 * 0 is loudest, 7 is near silence. The driver's TLV claims the opposite
 * (dBscale-min=-42 dB, step +6 dB) and is wrong in the same way it is wrong
 * about GAIN_CTL, so the metadata cannot be trusted on this codec at all.
 *
 * Despite its name it is not the jack. SPEAKER_CTL drives the speaker off the
 * headphone stage, so this control gates everything the device plays. Diatom's
 * port carried a comment insisting that raising it rerouted output to the jack
 * and muted the speakers - what actually happens is that raising it attenuates
 * the speaker, which sounds identical and is not the same thing. Settled by
 * ear on 2026-08-31: at 7 the speaker is barely audible, at 0 it is loud.
 *
 * This write already existed, in apply_volume, spelled "Headphone" - a control
 * this codec does not have. PL_CTL_ELEM_WRITE matches names exactly, the
 * return was thrown away, and so every sound the device ever made came out
 * 18 dB down while the source read as though it were setting this to zero.
 *
 * SWAP_CTL at 1 crosses the channels. The stock hook clears it
 * (runtrimui-original.sh: `tinymix set 1 0`) and so does NextUI; we never did,
 * so left and right have been backwards the entire time. It is an enumerated
 * control rather than an integer, but the value union overlaps and we only
 * ever write item 0, so the integer path reaches it.
 *
 * HP_CTL stays at 0 for both outputs. The jack was calibrated on 2026-09-02 and
 * the answer was not this control: 9.3 dB was wanted at the ceiling and this
 * steps in sixes, and it attenuates the speaker as well, so it cannot be moved
 * for the jack alone. The headphone case is handled by its own window on
 * GAIN_CTL instead - see HP_RAW_TOP. The one exception is the cut, in
 * apply_volume. */
static void mixer_defaults(void)
{
	if (mixer_fd < 0) return;
	ctl_set(HP_CTL, 0);
	ctl_set(SWAP_CTL, 0);
}

/* Is there a plug in the headphone jack?
 *
 * SW_HEADPHONE_INSERT on the codec's own input node. This device exposes the
 * state nowhere else: there is no ALSA jack kcontrol among its seventeen
 * controls and nothing under /sys for it, so the only way to ask is EVIOCGSW,
 * which is why this opens an input device rather than reading a file.
 *
 * Found by capability rather than by number. It is /dev/input/event2 today, but
 * that is an enumeration order and not a promise, and the cost of being wrong
 * is a volume ladder silently calibrated for the wrong output. */
#define BITS_PER_LONG   (8 * (int)sizeof(long))
#define SW_NLONGS       ((SW_MAX + BITS_PER_LONG) / BITS_PER_LONG)
#define BIT_IS_SET(a,b) (((a)[(b) / BITS_PER_LONG] >> ((b) % BITS_PER_LONG)) & 1UL)

static int jack_fd = -1;

static void jack_open(void)
{
	unsigned long bits[SW_NLONGS];
	char path[32];
	int i, fd;

	for (i = 0; i < 32; i++) {
		snprintf(path, sizeof path, "/dev/input/event%d", i);
		if ((fd = open(path, O_RDONLY | O_NONBLOCK)) < 0) continue;
		memset(bits, 0, sizeof bits);
		if (ioctl(fd, EVIOCGBIT(EV_SW, sizeof bits), bits) >= 0 &&
		    BIT_IS_SET(bits, SW_HEADPHONE_INSERT)) {
			jack_fd = fd;
			return;
		}
		close(fd);
	}
	fprintf(stderr, "settings: no headphone jack input node; "
	                "volume will use the speaker ladder\n");
}

static int jack_present(void)
{
	unsigned long bits[SW_NLONGS];

	if (jack_fd < 0) return 0;
	memset(bits, 0, sizeof bits);
	if (ioctl(jack_fd, EVIOCGSW(sizeof bits), bits) < 0) return 0;
	return BIT_IS_SET(bits, SW_HEADPHONE_INSERT) ? 1 : 0;
}

static int jack_was = -1;         /* last state acted on; -1 = never asked */

/* THE MUTE SWITCH, and why it is read here rather than as an input event.
 *
 * Measured 2026-09-16 by watching every input device and every exported GPIO
 * while the switch was flipped. It reports two ways: as EV_SW code 1 on
 * /dev/input/event3, and as this pin. The pin is the one worth reading,
 * because an event only arrives on a CHANGE - it says nothing about which way
 * the switch is pointing at boot, after a resume, or after the launcher
 * restarts, which is exactly when a physical control and the software can
 * disagree. A file that always holds the truth cannot drift.
 *
 * DOWN, which reads 1, is muted. Eric's call: "off" is what the position says,
 * and on a mute switch that labels the sound.
 *
 * Opened once and pread, not opened per poll: this runs from plat_input_poll,
 * which every screen calls once a frame. */
#define MUTE_GPIO "/sys/class/gpio/gpio243/value"
static int mute_fd = -1;
/* Tri-state: -1 means "not known", which is not the same as "not muted".
 *
 * The switch can be flipped DURING a game, when Diatom owns the codec and this
 * side is blocked in plat_resident_wait seeing nothing. Coming back with a
 * remembered position, the poll below compares the hardware against it, finds
 * them equal and returns without re-applying - so the launcher would play at
 * full volume with the switch down. Exactly the fault jack_forget exists to
 * prevent, and found by reading its comment. */
static int muted = -1;
/* Button Lock mode (TortOS-ib9): the switch stops muting and becomes an
 * iPod-style hold switch instead - see plat_hold_switch. Cached, not read from
 * the db, because plat_mute_poll runs every frame. */
static bool switch_locks;

static bool mute_switch_down(void)
{
	char c = 0;

	if (mute_fd < 0) mute_fd = open(MUTE_GPIO, O_RDONLY | O_CLOEXEC);
	if (mute_fd < 0) return false;
	if (pread(mute_fd, &c, 1, 0) != 1) return false;
	return c == '1';
}

void apply_volume(int v)
{
	int hp = jack_present();
	bool cut = v == 0 || muted == 1;
	long raw = cut ? GAIN_RAW_MAX
	         : aout_level_to_raw(v, VOL_MAX,
	                             hp ? HP_RAW_TOP : SPK_RAW_TOP,
	                             hp ? HP_RAW_BOTTOM : SPK_RAW_BOTTOM);
	long quiet = cut ? HP_CTL_QUIET : 0;
	long on = !cut;

	jack_was = hp;
	cur_vol = v;
	/* Zero has to cut the path, not merely attenuate it - and so does the
	 * switch, which is why it lands here rather than beside the callers: every
	 * route that re-applies a level (a nudge, a jack coming out) passes
	 * through this line and honors the switch for free.
	 *
	 * SPEAKER_CTL cuts the speaker only, so with headphones in the switch and
	 * level 0 both left them playing until 2026-09-30. They are cut by level
	 * instead: GAIN_CTL and HP_CTL both at their quiet ends, about -116 dB and
	 * silent by ear that day. Not "Headphone Switch", tried the same day: with
	 * it and SPEAKER_CTL both off the codec stops taking samples (hw_ptr stood
	 * still), and whatever waits on its stream waits until one comes back on -
	 * a game froze switching to a headset until the mute came off. */
	ctl_io(GAIN_CTL, &raw, 1);
	ctl_io(HP_CTL, &quiet, 1);
	ctl_io(SPEAKER_CTL, &on, 1);
}

/* Re-apply if the plug went in or came out since the last time.
 *
 * Without this a level only moves to the right ladder at the next volume press,
 * so plugging in mid-game leaves the old register in place - which is precisely
 * the moment the difference is 9 dB and being worn on your head. Cheap enough
 * to call from a periodic path: one ioctl on an already-open fd, and it writes
 * nothing unless the state actually changed. */
bool plat_headphones_present(void) { return jack_present() != 0; }

/* The switch, checked wherever the jack is and for the same reason: this is
 * the one thing every screen does once a frame.
 *
 * It mutes the LAUNCHER's audio only. While a game runs the launcher is
 * blocked in plat_resident_wait and Diatom owns the codec - it must, or the
 * two would fight over one control - so the switch reaches a game by being
 * told to it, not by both of them reading the same pin. Deciding what a
 * physical control MEANS is the launcher's job; being quiet when asked is the
 * emulator's. See BACKLOG 28. */
bool plat_mute_poll(bool own_volume)
{
	int now = mute_switch_down() && !switch_locks ? 1 : 0;

	if (now == muted) return false;
	muted = now;
	/* Out of a game this side is the only writer. Diatom used to be told here
	 * too, and it answered by applying its own level - which out of a game is
	 * -1, unknown - so every unmute switched the speaker back off a moment
	 * after this side turned it on, and with headphones in wrote a gain past
	 * the register's end that wrapped around to full volume. Found
	 * 2026-09-30. It needs no telling here: every RUN sends the switch's
	 * state before the game starts. */
	if (own_volume) {
		if (cur_vol >= 0) apply_volume(cur_vol);
		return true;
	}
	/* DURING A GAME, ONLY THE CUT - never this side's level.
	 *
	 * apply_volume would write GAIN_CTL from cur_vol, which is this side's
	 * idea of the volume and is stale the moment Diatom takes over: the
	 * player can change it in-game and only Diatom knows where it ended up.
	 * Re-applying it here is the exact fault the comment above plat_input_poll
	 * warns about, arriving by a new road.
	 *
	 * Turning the speaker back ON is safe even if Diatom sits at level 0,
	 * because it attenuates as well as switching - its own comment puts the
	 * control's minimum near -74 dB - so the worst case is a path opened onto
	 * something already inaudible, and Diatom's own next write settles it.
	 *
	 * This side cuts because it is immediate: 100 ms, the resident loop's
	 * period. The SETMUTE is what makes the cut STICK, since Diatom writes
	 * the same controls whenever it applies a level (ADR-0031). Neither alone
	 * is enough - one is fast and one is durable. */
	dsend("SETMUTE\ton=%d", muted == 1 ? 1 : 0);
	{
		long on = (muted != 1);

		ctl_io(SPEAKER_CTL, &on, 1);
		/* And the headphones, by level as in apply_volume - the cut only.
		 * Coming back, the level is Diatom's, and the SETMUTE above has it
		 * put its own back. */
		if (!on) {
			long raw = GAIN_RAW_MAX, quiet = HP_CTL_QUIET;

			ctl_io(GAIN_CTL, &raw, 1);
			ctl_io(HP_CTL, &quiet, 1);
		}
	}
	return true;
}

bool plat_muted(void) { return muted == 1; }

/* Whatever this side remembers about the switch was formed while it was
 * driving; a game just was. Called where jack_forget is, and for its reason. */
void mute_forget(void) { muted = -1; }

/* Forgetting the position is what makes the change land: the next poll finds
 * a difference and re-applies, so switching modes with the switch down mutes
 * or unmutes at once - Diatom included, through the same SETMUTE. */
void plat_mute_switch_lock(bool lock)
{
	switch_locks = lock;
	mute_forget();
}

bool plat_hold_switch(void) { return switch_locks && mute_switch_down(); }

void plat_audio_jack_poll(void)
{
	if (!aout_should_reapply(jack_was, jack_present() != 0, cur_vol >= 0))
		return;
	apply_volume(cur_vol);
}

/* Forget which way the jack was, so the next poll re-applies whatever the
 * hardware says now instead of trusting a memory formed before a handover. */
void jack_forget(void) { jack_was = -1; }

void apply_brightness(int b)
{
	unsigned long a[4] = { 0, 0, 0, 0 };

	cur_bright = b;
	if (disp_fd < 0) return;
	a[1] = bright_ladder[b];
	ioctl(disp_fd, DISP_LCD_SET_BRIGHTNESS, a);
}

/* The backlight OFF, for sleep - NextUI's SetRawBrightness(0). Not
 * apply_brightness(0): rung 0 is 2/255, still lit, which is a dim screen
 * rather than a dark one (found on hardware, 2026-09-28). cur_bright is left
 * alone, so apply_brightness(cur_bright) puts the player's level back. */
void backlight_off(void)
{
	unsigned long a[4] = { 0, 0, 0, 0 };

	if (disp_fd >= 0) ioctl(disp_fd, DISP_LCD_SET_BRIGHTNESS, a);
}

void plat_settings_init(void)
{
	int v = -1, b = -1;

	mixer_fd = open("/dev/snd/controlC0", O_RDWR);
	disp_fd  = open("/dev/disp", O_RDWR);
	if (mixer_fd < 0) fprintf(stderr, "settings: no /dev/snd/controlC0\n");
	if (disp_fd  < 0) fprintf(stderr, "settings: no /dev/disp\n");
	mixer_defaults();
	jack_open();

	/* The player's last choice, and it wins. The two-tier lookup this
	 * replaces - a saved level, then a shipped default - is one key each,
	 * because seeding writes the default once and a nudge overwrites it. Same
	 * outcome, and it can no longer get the precedence wrong: the config used
	 * to be reapplied over the saved level at every boot, which put it ahead
	 * of the player. */
	v = db_get_int(db_dev(), "volume", -1);
	b = db_get_int(db_dev(), "brightness", -1);
	switch_locks = db_get_int(db_dev(), "muteswitch", 0) == 1;

	/* Both are stored in the units this code uses - a rung each - so there is
	 * no conversion here and no way for one key to mean two things. */

	/* Last, whatever the panel is already at, so the launcher's first OSD
	 * tells the truth even with nothing configured anywhere - launch.sh set a
	 * brightness before this process existed. Nearest rung, because the panel
	 * reports raw values and only the ladder has rungs. */
	if (b < 0 && disp_fd >= 0) {
		int raw = ioctl(disp_fd, DISP_LCD_GET_BRIGHTNESS, (unsigned long[4]){ 0, 0, 0, 0 });
		int i;
		if (raw >= 0) {
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

#else   /* not __linux__ */

/* The host build exists to look at the shelf while changing how it looks
 * (mk/native.mk). There is no codec and no display engine here, so the
 * settings are state and nothing more - enough that the OSD draws and the
 * levels the launcher reports are consistent. */
#define VOL_MAX    PLAT_VOL_MAX
#define BRIGHT_MAX PLAT_BRIGHT_MAX
int cur_vol = 8, cur_bright = 7;
int mixer_fd = 0, disp_fd = 0;    /* "present", so the nudges run */
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
void levels_save(void) { }
void apply_volume(int v) { cur_vol = v; }
void apply_brightness(int b) { cur_bright = b; }
/* No jack on the host, so nothing can be plugged into it. */
void plat_audio_jack_poll(void) { }
/* And no switch to flip. */
bool plat_mute_poll(bool own_volume) { (void)own_volume; return false; }
bool plat_muted(void) { return false; }
bool plat_headphones_present(void) { return false; }
void jack_forget(void) { }
void mute_forget(void) { }
void plat_mute_switch_lock(bool lock) { (void)lock; }
bool plat_hold_switch(void) { return false; }
void backlight_off(void) { }

/* No settings database on the host, so the config defaults are all there is.
 * Taken anyway rather than ignored: a shelf rendered by --shot should show the
 * OSD at the levels the card is configured for. */
void plat_settings_init(void)
{
	/* The host has no codec and no display engine, so the levels above are
	 * all there is and nothing needs applying to hardware. */
}

#endif  /* __linux__ */

/* ---- sleep -----------------------------------------------------------
 *
 * Real suspend-to-RAM, not the pseudo-sleep this device already has under
 * a different name: Auto Off (sys_menu.c) is a full power-off, and stats.h
 * says outright that the device "has no suspend and is not getting one" -
 * true when that comment was written, and now the thing this adds.
 *
 * UNVERIFIED ON HARDWARE. Whether this kernel's /sys/power/state lists
 * "mem" at all was this feature's open question from the day it was
 * proposed, and nothing in this environment can answer it - no device was
 * available to ask. So this is written to discover the answer safely
 * rather than to assume it: plat_sleep_supported() probes without
 * triggering anything, and plat_sleep() re-checks that "mem" is actually
 * among the listed states (not just that the file is writable) before
 * ever writing to it. A kernel that lacks it, or only offers "freeze" /
 * "standby", makes this a safe no-op rather than a wrong write.
 *
 * NO PROTOCOL MESSAGE TO DIATOM. NextUI's PWR_enterSleep pauses audio and
 * SIGSTOPs helper daemons before its own equivalent write, because on its
 * platforms sleep is assembled from several independently-paused pieces.
 * TortOS has no helper daemons, and this write is a REAL kernel-wide
 * suspend: it blocks until woken, and every process on the device -
 * including Diatom, mid-frame or not - is frozen by the kernel along with
 * it and resumes exactly where it was. There is nothing for the launcher
 * to tell Diatom in advance that the kernel is not already doing for
 * every process uniformly, audio codec included.
 *
 * RETROFITTED to match NextUI's real suspend script (skeleton/SYSTEM/tg5040/
 * bin/suspend) exactly, per this epic's 1:1 standing rule (see bd show
 * TortOS-1v7.1.2): ALSA mixer state is saved before sleep and deliberately
 * never restored (NextUI's own restore call is commented out in its shipped
 * script - replicated as-is, not fixed); Bluetooth/Wi-Fi are stopped before
 * sleep only if they were actually running, and restarted after waking in
 * the background via plat_spawn_detached() (mirrors NextUI's `after &`, so a
 * slow bt_on() never blocks wake); the write itself is wrapped in NextUI's
 * 5-attempt retry loop with its time-based false-negative workaround. Not
 * ported: NextUI's pre-sleep.d/post-resume.d hook-plugin system - verified a
 * no-op on every stock install including NextUI's own (run_hooks.sh exits
 * immediately when its hook directory doesn't exist, and it doesn't exist
 * anywhere in NextUI's tree), and TortOS has no PAK ecosystem to serve it. */
bool plat_sleep_supported(void)
{
#ifdef __linux__
	static int cached = -1;

	if (cached < 0) cached = access("/sys/power/state", W_OK) == 0 ? 1 : 0;
	return cached == 1;
#else
	return false;
#endif
}

#ifdef __linux__
/* Fork+exec argv, block for exit, report whether it exited zero. Stdin closed
 * since nothing here reads from a terminal; nothing between fork and exec
 * that is not async-signal-safe - the launcher has threads (see
 * child_environ() above for why that matters). */
static bool run_argv(char *const argv[])
{
	pid_t pid = fork();
	int st;

	if (pid < 0) return false;
	if (pid == 0) {
		int null = open("/dev/null", O_RDONLY);
		if (null >= 0) { dup2(null, 0); close(null); }
		execv(argv[0], argv);
		_exit(127);
	}
	if (waitpid(pid, &st, 0) != pid) return false;
	return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static bool sh_c(const char *cmd)
{
	char *argv[4];
	argv[0] = (char *)"/bin/sh";
	argv[1] = (char *)"-c";
	argv[2] = (char *)cmd;
	argv[3] = NULL;
	return run_argv(argv);
}

/* No -x: measured on-device (2026-09-27) that this busybox pgrep's -x fails
 * to match "bluetoothd" against its own /proc/pid/comm, even though plain
 * substring pgrep finds it fine - silently skipping the whole bt stop/
 * restart path. Matches radio.sh's and NextUI's own suspend script's pgrep
 * calls, neither of which uses -x either. */
static bool proc_running(const char *name)
{
	char cmd[64];
	if (snprintf(cmd, sizeof cmd, "pgrep %s > /dev/null", name)
	    >= (int)sizeof cmd)
		return false;
	return sh_c(cmd);
}

/* Runs one of sd/tortos/radio.sh's functions the same way bt_asoundrc()
 * (src/bt.c) runs bt_write_asoundrc: sourced from its own path, passed as
 * argv so it is never read as shell. `fn` is always a literal from a call
 * site below, never external data - see bt_asoundrc()'s own comment on why
 * that distinction is what makes this safe. `background` runs it detached
 * (plat_spawn_detached) rather than blocking, for the post-wake restart. */
static bool radio_sh_call(const char *fn, bool background)
{
	char script[512], cmd[64];
	char *argv[5];

	if (snprintf(script, sizeof script, "%s/radio.sh", P_ROOT)
	    >= (int)sizeof script)
		return false;
	if (snprintf(cmd, sizeof cmd, ". \"$0\" && %s", fn) >= (int)sizeof cmd)
		return false;
	argv[0] = (char *)"/bin/sh";
	argv[1] = (char *)"-c";
	argv[2] = cmd;
	argv[3] = script;
	argv[4] = NULL;
	return background ? plat_spawn_detached(argv, NULL, NULL) : run_argv(argv);
}
#endif

#define INPUTD_SUSPEND_FLAG "/tmp/system_suspend"

bool plat_sleep(void)
{
#ifdef __linux__
	bool slept = false;
	char states[128] = { 0 };
	int fd;
	ssize_t n;
	bool bt_was_up, wifi_was_up;
	int fd2, tries;

	if (!plat_sleep_supported()) return false;

	fd = open("/sys/power/state", O_RDONLY);
	if (fd < 0) return false;
	n = read(fd, states, sizeof states - 1);
	close(fd);
	if (n <= 0) return false;
	states[n] = '\0';
	if (!strstr(states, "mem")) return false;   /* listed states don't include it */

	/* Not plat_brightness_set(): a sleep-only level must never be persisted
	 * as the player's chosen brightness - see levels_save() above. */
	backlight_off();

	/* Park the stock input daemon for the whole transition. On the Brick Pro
	 * it polls the sticks over i2c3 about a thousand times a second, and
	 * those transfers running into suspend entry or resume hang the device
	 * until the watchdog reboots it (TortOS-pky.11). Its own handshake: while
	 * this file exists it parks its polling threads (measured: twi3 1017/s,
	 * 0/s with the file). Stock creates it when the screen blanks; nothing
	 * here did. The Brick's inputd reads it too and has no stick bus, so
	 * there it only pauses input nobody is using. The power key is read
	 * straight from fd_power, not through inputd, so waking is unaffected. */
	{
		int ff = open(INPUTD_SUSPEND_FLAG, O_WRONLY | O_CREAT, 0644);
		if (ff >= 0) close(ff);
	}

	/* before(): only touch a radio that was actually running - a player who
	 * turned Bluetooth or Wi-Fi off in settings must not find it back on
	 * after a sleep cycle. Mirrors NextUI's own pgrep-gated before(). */
	bt_was_up = proc_running("bluetoothd");
	wifi_was_up = proc_running("wpa_supplicant");
	sh_c("mkdir -p /tmp/asound-suspend && "
	     "alsactl --file /tmp/asound-suspend/asound.state.pre store");
	if (bt_was_up) radio_sh_call("bt_off", false);
	if (wifi_was_up) radio_sh_call("wifi_stop_once", false);

	/* The write itself, wrapped in NextUI's exact 5-attempt retry loop:
	 * wall-clock time (not plat_now_ms()/SDL_GetTicks(), which is monotonic
	 * and not guaranteed to advance across a real suspend) measures how long
	 * the write call itself was blocked, to catch a kernel that suspended
	 * fine but still returned nonzero - see the time_asleep check below,
	 * ported verbatim from the shipped script's own comment on it. */
	/*
	 * One divergence from the script, which cannot see the button: a POWER
	 * press is a wake, never a failure. Suspending takes 2-3s after the
	 * screen goes dark, and a press in that window makes the kernel abort
	 * with EBUSY at suspend_noirq (measured 2026-09-28: 6 of 7 failures,
	 * wakeup IRQ = the axp2202 PMIC). The script retries 3s later and
	 * suspends again, swallowing the press that asked to wake. So: a press
	 * before a write, or during the wait after a failed one, ends it awake.
	 * Other failures (a charger plugged in, same IRQ) still retry.
	 *
	 * Not covered, and cannot be from here: a press in the ~1.1s after the
	 * kernel freezes this process and before suspend_noirq. axp2101-pek is
	 * no wakeup source, so nothing aborts; the press waits unread and the
	 * NEXT press wakes. NextUI on this device has the same window. Closing
	 * it takes the wakeup_count handshake plus EPOLLWAKEUP on fd_power,
	 * whose read blocks while USB holds usb_connecting - weighed and
	 * declined 2026-09-28 (TortOS-1v7.1.2.7).
	 */
	for (tries = 0; tries < 5; tries++) {
		time_t went, woke;
		ssize_t wrote;
		int err;

		if (power_key_within(0, 1)) { slept = true; break; }
		went = time(NULL);
		fd2 = open("/sys/power/state", O_WRONLY);
		wrote = fd2 < 0 ? -1 : write(fd2, "mem", 3);
		err = errno;
		if (fd2 >= 0) close(fd2);
		woke = time(NULL);
		/* The script's own trace, one line an attempt: the kernel's answer
		 * is the one thing a log can't reconstruct afterwards. */
		fprintf(stderr, "sleep: attempt %d of 5: %s%s, %lds\n", tries + 1,
		        wrote == 3 ? "ok" : "failed: ",
		        wrote == 3 ? "" : strerror(err), (long)(woke - went));
		if (wrote == 3) { slept = true; break; }
		if (woke - went > 5) { slept = true; break; }  /* false-negative override */
		if (power_key_within(3000, 1)) { slept = true; break; }  /* sleep(3) */
	}

	unlink(INPUTD_SUSPEND_FLAG);

	/* POWER is the ordinary way to wake the device, and that very press is
	 * still sitting on fd_power once we resume - without this, the next
	 * poll reads it as a fresh press and treats it as a request to power
	 * off, which looks like "resumed fine, then shut itself down a moment
	 * later." Same drain idiom as the escape hatch above. */
	{
		struct input_event ev;
		while (fd_power >= 0 &&
		       read(fd_power, &ev, sizeof ev) == (ssize_t)sizeof ev)
			; /* drain the wake press */
	}

	/* after(): backgrounded, same as NextUI's `after &`, so a slow bt_on()
	 * (rfkill rail-cycle, hciattach retries) never blocks the first frame
	 * after wake. */
	if (wifi_was_up) radio_sh_call("wifi_on", true);
	if (bt_was_up) radio_sh_call("bt_on", true);

	apply_brightness(cur_bright);
	/* The script's exit status: PWR_deepSleep's ret, which PWR_waitForWake
	 * answers with a power-off when it is not 0. A wake press counts as
	 * success - the player is asking for exactly what a resume gives. */
	return slept;
#else
	return false;
#endif
}

/* ---- battery ---- */

bool plat_battery(int *pct, bool *charging)
{
	return battery_read("/sys/class/power_supply/axp2202-battery", pct, charging);
}
