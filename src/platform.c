/* SPDX-License-Identifier: MIT */
#include "atomic.h"
#include "db.h"
#include "platform.h"
#include "audioout.h"

#include "ui.h"   /* the settings line shares the rail's weight and palette */

#include <dlfcn.h>
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

/* The front function keys report on the gamepad node as BTN_THUMBL/BTN_THUMBR
 * (317/318). SDL maps them to joystick buttons 9 and 10, which emulators call
 * L3/R3 -- which is why the in-game input map has to drop L3/R3, or every
 * brightness press also reaches the core. */
#define CODE_FN_LEFT  317
#define CODE_FN_RIGHT 318

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

/* Hold-to-repeat. Short delay and a quick rate: this is a shelf you sweep
 * along, and waiting on a key repeat is the most obvious kind of slow. */
#define REPEAT_DELAY_MS 300
#define REPEAT_RATE_MS  90

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Joystick *joy;
static int fd_power = -1; /* axp2202-pek: KEY_POWER */
static int fd_keys = -1;  /* sunxi-keyboard: volume keys */
static int fd_joy = -1;   /* TRIMUI Player1: raw, for the front F1/F2 keys */

/* TORTOS_INPUT_DEBUG=1 logs raw evdev codes and SDL button indices, so one
 * press tells you exactly which device a control arrives on. */
static int dbg_input;

const char *P_ROOT = "/mnt/SDCARD/TortOS";
const char *P_CARD = "/mnt/SDCARD";
const char *P_ROMS = "/mnt/SDCARD/Roms";
const char *P_USERDATA = "/mnt/SDCARD/.userdata/tg3040";
const char *P_SHARED = "/mnt/SDCARD/.userdata/shared";
/* Over The Hare's page. On the card rather than in the binary so it can be
 * restyled with a text editor and a reload, which is the whole argument for
 * a file-transfer feature existing at all. */
const char *P_WEB = "/mnt/SDCARD/TortOS/res/web";

/* ---- core options, read once from the library database ------------------- */
/* Lines before any [SECTION] apply to every game; a [TAG] section applies only
 * to that system, keyed on the same tag systems.cfg uses for saves and states.
 *
 * The per-system half exists because mgba_gb_model cannot be set globally.
 * Autodetect is right for Game Boy Color and GBA and wrong only for Game Boy,
 * where it reads the SGB flag and boots a Super Game Boy - a SNES accessory -
 * so a DMG cartridge comes out colorized and framed in a border, on a shelf
 * that says Game Boy. Pinning the model globally would force GBC titles into
 * DMG mode too, and a 0xC0 cartridge would refuse to boot. */
#define COREOPT_MAX 48
static struct { char tag[8]; char kv[192]; } coreopts[COREOPT_MAX];
static int  ncoreopts = -1;   /* -1 = not read yet */

static bool coreopt_row(const char *key, const char *value, void *ctx)
{
	const char *rest = key + strlen("coreopt.");
	const char *dot = strchr(rest, '.');
	char kv[192];

	(void)ctx;
	if (!dot || ncoreopts >= COREOPT_MAX) return ncoreopts < COREOPT_MAX;

	/* Refused, not stored short. Half a key=value pair is still a
	 * valid-looking one, and it would be sent to a core as though someone
	 * meant it. */
	if ((size_t)(dot - rest) >= sizeof coreopts[0].tag ||
	    snprintf(kv, sizeof kv, "%s=%s", dot + 1, value) >= (int)sizeof kv) {
		fprintf(stderr, "coreopts: entry too long, ignoring: %.40s\n", key);
		return true;
	}
	snprintf(coreopts[ncoreopts].tag, sizeof coreopts[0].tag, "%.*s",
	         (int)(dot - rest), rest);
	snprintf(coreopts[ncoreopts].kv, sizeof coreopts[0].kv, "%s", kv);
	ncoreopts++;
	return true;
}

/* Keys are "coreopt.<tag>.<option>", with an empty tag for a global - so
 * "coreopt..mgba_sgb_borders" is global and "coreopt.GB.mgba_gb_model" is not.
 * The empty segment looks odd and is the point: one prefix scan brings back
 * both kinds, and coreopt_nth below still decides precedence. */
static void coreopts_load(void)
{
	ncoreopts = 0;
	db_each_prefix(db_lib(), "coreopt.", coreopt_row, NULL);
}

/* Global entries first, then this tag's, so a system can override a global. */
static int coreopt_nth(const char *tag, int want, const char **out)
{
	int pass, i, seen = 0;
	for (pass = 0; pass < 2; pass++)
		for (i = 0; i < ncoreopts; i++) {
			bool global = coreopts[i].tag[0] == '\0';
			if (pass == 0 ? !global : global) continue;
			if (pass == 1 && (!tag || strcmp(coreopts[i].tag, tag))) continue;
			if (seen++ == want) { if (out) *out = coreopts[i].kv; return 1; }
		}
	return 0;
}

int plat_coreopt_count(const char *tag)
{
	int n = 0;
	if (ncoreopts < 0) coreopts_load();
	while (coreopt_nth(tag, n, NULL)) n++;
	return n;
}

const char *plat_coreopt(const char *tag, int i)
{
	const char *kv = "";
	if (ncoreopts < 0) coreopts_load();
	coreopt_nth(tag, i, &kv);
	return kv;
}

/* ---- turbo, read once from the library database -------------------------- */
/* One canonical map per system tag, handed to Diatom after RUN. Its ADR-0028
 * makes a pulse a property of a BINDING, so `x:a~3,y:b~3` is the whole feature:
 * X becomes a turbo A and Y a turbo B, three frames pressed and three released.
 *
 * Per system because it is only safe where those two buttons are SPARE. Seven of
 * the eleven consoles here have two face buttons; a Genesis 6-button pad and a
 * SNES pad use X and Y for real, and turbo would take them away.
 *
 * A file rather than a table in the binary because the rate is exactly the sort
 * of thing a player wants to change, and because a remap screen would one day
 * write this same field over the same protocol. */
#define TURBO_MAX 16
static struct { char tag[8]; char map[96]; } turbos[TURBO_MAX];
static int nturbos = -1;                     /* -1 = not read yet */

static bool turbo_row(const char *key, const char *value, void *ctx)
{
	const char *tag = key + strlen("turbo.");

	(void)ctx;
	if (nturbos >= TURBO_MAX) return false;
	/* Refused, not stored short. Half a map is a map nobody wrote, and Diatom
	 * rejects a bad one whole - after the game is already up, where the
	 * refusal is invisible. */
	if (strlen(tag) >= sizeof turbos[0].tag ||
	    strlen(value) >= sizeof turbos[0].map) {
		fprintf(stderr, "turbo: entry too long, ignoring: %.32s\n", tag);
		return true;
	}
	snprintf(turbos[nturbos].tag, sizeof turbos[0].tag, "%s", tag);
	snprintf(turbos[nturbos].map, sizeof turbos[0].map, "%s", value);
	nturbos++;
	return true;
}

static void turbos_load(void)
{
	nturbos = 0;
	db_each_prefix(db_lib(), "turbo.", turbo_row, NULL);
}

/* The map for this system, or NULL for one that wants none. */
const char *plat_turbo_map(const char *tag)
{
	int i;

	if (nturbos < 0) turbos_load();
	if (!tag) return NULL;
	for (i = 0; i < nturbos; i++)
		if (!strcmp(turbos[i].tag, tag)) return turbos[i].map;
	return NULL;
}

/* ---- hotkeys, same shape as turbo above but player-editable at runtime --
 * One canonical binding string per system tag - "l2:ff,r2:rewind,x:savestate,
 * y:loadstate" - handed to Diatom as SETHOTKEYS after RUN, the same way
 * turbo's map already rides SETMAP. See diatom's ADR-0035.
 *
 * Unlike turbo, there is no shipped default: this is new and opt-in, and
 * seeding a binding the player never asked for is not what "opt-in" means.
 * A tag with no hotkey.<tag> row plays exactly as it always has. */
#define HOTKEY_MAX 16
static struct { char tag[8]; char map[64]; } hotkeys[HOTKEY_MAX];
static int nhotkeys = -1;                     /* -1 = not read yet */

static bool hotkey_row(const char *key, const char *value, void *ctx)
{
	const char *tag = key + strlen("hotkey.");

	(void)ctx;
	if (nhotkeys >= HOTKEY_MAX) return false;
	if (strlen(tag) >= sizeof hotkeys[0].tag ||
	    strlen(value) >= sizeof hotkeys[0].map) {
		fprintf(stderr, "hotkey: entry too long, ignoring: %.32s\n", tag);
		return true;
	}
	snprintf(hotkeys[nhotkeys].tag, sizeof hotkeys[0].tag, "%s", tag);
	snprintf(hotkeys[nhotkeys].map, sizeof hotkeys[0].map, "%s", value);
	nhotkeys++;
	return true;
}

static void hotkeys_load(void)
{
	nhotkeys = 0;
	db_each_prefix(db_lib(), "hotkey.", hotkey_row, NULL);
}

/* The binding string for this system, or "" for one with none set - never
 * NULL, so a caller building a SETHOTKEYS line needs no extra branch (an
 * empty spec is itself valid: Diatom's hotkeys_set("") means "no bindings",
 * which is exactly what "none set" should send). */
const char *plat_hotkey_map(const char *tag)
{
	int i;

	if (nhotkeys < 0) hotkeys_load();
	if (!tag) return "";
	for (i = 0; i < nhotkeys; i++)
		if (!strcmp(hotkeys[i].tag, tag)) return hotkeys[i].map;
	return "";
}

/* Persists AND updates the cache in the same call - unlike turbo's map,
 * this is written from a live settings screen, not only ever read, so a
 * second lookup on the same tag before the next full reload must see what
 * was just set rather than a stale row. */
void plat_hotkey_set(const char *tag, const char *spec)
{
	char key[16];
	int i;

	if (nhotkeys < 0) hotkeys_load();
	if (!tag) return;
	snprintf(key, sizeof key, "hotkey.%s", tag);
	db_set_str(db_lib(), key, spec ? spec : "");

	for (i = 0; i < nhotkeys; i++)
		if (!strcmp(hotkeys[i].tag, tag)) break;
	if (i == nhotkeys && nhotkeys < HOTKEY_MAX) nhotkeys++;
	if (i < HOTKEY_MAX) {
		snprintf(hotkeys[i].tag, sizeof hotkeys[0].tag, "%s", tag);
		snprintf(hotkeys[i].map, sizeof hotkeys[0].map, "%s", spec ? spec : "");
	}
}
const char *P_FONT = "/mnt/SDCARD/TortOS/menu.ttf";

void paths_init(void)
{
	const char *v;
	if ((v = getenv("TORTOS_ROOT"))) P_ROOT = v;
	if ((v = getenv("TORTOS_CARD"))) P_CARD = v;
	if ((v = getenv("TORTOS_ROMS"))) P_ROMS = v;
	if ((v = getenv("TORTOS_USERDATA"))) P_USERDATA = v;
	if ((v = getenv("TORTOS_SHARED"))) P_SHARED = v;
	if ((v = getenv("TORTOS_WEB")))    P_WEB = v;
	if ((v = getenv("TORTOS_FONT"))) P_FONT = v;
}

SDL_Renderer *plat_renderer(void) { return ren; }
unsigned plat_now_ms(void) { return SDL_GetTicks(); }

bool plat_video_init(void)
{
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
#ifdef __linux__
	SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
#endif
	if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "video init: %s\n", SDL_GetError());
		return false;
	}
	SDL_ShowCursor(SDL_DISABLE);
	win = SDL_CreateWindow("TortOS", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
	                       TORTOS_SCREEN_W, TORTOS_SCREEN_H,
	                       SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
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
	/* The cube turns two offscreen faces, so this is load-bearing rather than
	 * informational: without it the vertical shelves have nothing to draw. */
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

static void set_btn(in_state *st, in_button b, bool down)
{
	if (b == IN_NONE) return;
	if (down && !st->down[b]) {
		st->pressed[b] = true;
		st->down_since[b] = SDL_GetTicks();
		st->last_repeat[b] = 0;
	}
	st->down[b] = down;
}

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
		else if (ev.code == CODE_FN_RIGHT) b = IN_BRIGHTUP;
		else if (ev.code == CODE_FN_LEFT) b = IN_BRIGHTDN;
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

/* Set from main's SIGTERM handler. Surfaced here rather than checked at each
 * call site because `quit_requested` is already polled by every loop in the
 * program, so one assignment reaches all of them - including modal loops that
 * were written later and would otherwise have to remember. The keyboard was
 * exactly that case: it checked quit_requested and not main's want_quit, so
 * SIGTERM was ignored while it was open and `make adb-restart` silently did
 * nothing until the process was killed with -9. */
static volatile sig_atomic_t g_terminating;

void plat_terminate(void) { g_terminating = 1; }

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
		case SDL_JOYHATMOTION: {
			Uint8 v = e.jhat.value;
			set_btn(st, IN_LEFT,  (v & SDL_HAT_LEFT)  != 0);
			set_btn(st, IN_RIGHT, (v & SDL_HAT_RIGHT) != 0);
			set_btn(st, IN_UP,    (v & SDL_HAT_UP)    != 0);
			set_btn(st, IN_DOWN,  (v & SDL_HAT_DOWN)  != 0);
			break;
		}
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

bool in_repeat(in_state *st, in_button b)
{
	if (st->pressed[b]) return true;
	if (!st->down[b]) return false;
	Uint32 now = SDL_GetTicks();
	if (now - st->down_since[b] < REPEAT_DELAY_MS) return false;
	if (now - st->last_repeat[b] < REPEAT_RATE_MS) return false;
	st->last_repeat[b] = now;
	return true;
}

/* Whether the last game ended because the player hit power rather than because
 * they quit. The launcher owns the shutdown sequence, so it has to be able to
 * tell the two apart when control comes back. */
static bool run_power_pressed;

bool plat_run_power_pressed(void) { return run_power_pressed; }
void plat_note_power_pressed(void) { run_power_pressed = true; }

extern char **environ;

/* The environment a child starts with: this process's, with `envkv` laid over
 * it - each "KEY=VALUE" replacing any KEY already there. Built BEFORE the fork
 * and handed to execve, so between fork and exec the child calls nothing but
 * chdir, setsid, execve, write and _exit.
 *
 * Both forks below used to putenv each pair in the child, and one reported a
 * failed exec with fprintf. Neither is async-signal-safe, which is all POSIX
 * promises a child of a threaded process may call before it execs - and this
 * process has threads, the art resizer among them writing to stderr. Whether
 * glibc's own fork makes those two safe anyway was never checked; building the
 * environment first removes the question.
 *
 * The strings are borrowed, not copied: environ's and the caller's both outlive
 * the exec that uses them. Only the array is allocated, and the parent frees it
 * once the fork is done. */
static char **child_environ(const char *const envkv[])
{
	size_t have = 0, add = 0, i, k, n = 0;
	char **out;

	while (environ && environ[have]) have++;
	while (envkv && envkv[add]) add++;
	if (!(out = malloc((have + add + 1) * sizeof *out))) return NULL;
	for (i = 0; i < have; i++) {
		const char *eq = strchr(environ[i], '=');
		size_t klen = eq ? (size_t)(eq - environ[i]) : strlen(environ[i]);
		bool replaced = false;

		for (k = 0; k < add && !replaced; k++)
			replaced = !strncmp(envkv[k], environ[i], klen) && envkv[k][klen] == '=';
		if (!replaced) out[n++] = environ[i];
	}
	for (k = 0; k < add; k++)
		if (strchr(envkv[k], '=')) out[n++] = (char *)envkv[k];
	out[n] = NULL;
	return out;
}

int plat_run(char *const argv[], const char *const envkv[], const char *workdir)
{
	pid_t pid;
	char **env = child_environ(envkv);
	size_t alen = strlen(argv[0]);

	if (!env) return -1;
	run_power_pressed = false;
	pid = fork();
	if (pid < 0) { free(env); return -1; }
	if (pid == 0) {
		ssize_t w;

		if (workdir) {
			if (chdir(workdir) != 0) { /* still try to run */ }
		}
		execve(argv[0], argv, env);
		/* write, not fprintf - see child_environ. */
		w = write(2, "execve ", 7);
		w = write(2, argv[0], alen);
		w = write(2, ": failed\n", 9);
		(void)w;
		_exit(127);
	}
	free(env);
	int status = 0;
#ifdef __linux__
	/* Escape hatch: a core that dead-ends (a bad ROM, a missing BIOS) leaves
	 * the child showing an error screen that eats no input. Watch the power
	 * button while waiting and end the child on a press. */
	struct input_event ev;
	while (fd_power >= 0 && read(fd_power, &ev, sizeof ev) == (ssize_t)sizeof ev)
		; /* drain stale events */
	int ms_since_term = -1;
	for (;;) {
		pid_t r = waitpid(pid, &status, WNOHANG);
		if (r == pid) break;
		if (r < 0) return -1;
		usleep(100 * 1000);
		while (fd_power >= 0 && read(fd_power, &ev, sizeof ev) == (ssize_t)sizeof ev) {
			if (ev.type == EV_KEY && ev.code == KEY_POWER && ev.value == 1 &&
			    ms_since_term < 0) {
				run_power_pressed = true;
				kill(pid, SIGTERM);
				ms_since_term = 0;
			}
		}
		if (ms_since_term >= 0) {
			ms_since_term += 100;
			if (ms_since_term > 3000) {
				kill(pid, SIGKILL);
				ms_since_term = -1;
			}
		}
	}
#else
	if (waitpid(pid, &status, 0) < 0) return -1;
#endif
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Start a child that outlives this process. Double-forked, so the middle
 * child is reaped here and the grandchild is inherited by init rather than
 * becoming a zombie nobody waits for. */
bool plat_spawn_detached(char *const argv[], const char *const envkv[],
                         const char *workdir)
{
	char **env = child_environ(envkv);
	struct rlimit rl;
	int top = 1024, fd;
	pid_t pid;

	/* EVERYTHING ABOVE STDERR IS CLOSED IN THE CHILD, before it becomes the
	 * program asked for.
	 *
	 * Fork hands a child every descriptor this process has open: the resident
	 * emulator's socket, Muse's, the display, the GPU, four input devices,
	 * the font. A short-lived child drops them when it exits. A DAEMON keeps
	 * them for as long as it lives, and Muse is meant to outlive the launcher.
	 * That froze the Brick on 2026-09-18: Muse held a dead launcher's end of
	 * Diatom's socket, so Diatom never saw it close, and the restarted
	 * launcher blocked in connect before its first frame. Diatom's half is
	 * its ADR-0033; this is ours.
	 *
	 * By number up to the limit, not by listing /proc/self/fd: listing
	 * allocates, and after fork in a threaded process only async-signal-safe
	 * calls are safe - close is one. The limit is read here, before fork, for
	 * the same reason. 1024 on the Brick, with about thirty open. stdout and
	 * stderr stay: they are the log, and a daemon's lines belong in it. */
	if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
		top = rl.rlim_cur < 65536 ? (int)rl.rlim_cur : 65536;

	if (!env) return false;
	pid = fork();
	if (pid < 0) { free(env); return false; }
	if (pid == 0) {
		if (fork() == 0) {
			setsid();
			for (fd = 3; fd < top; fd++) close(fd);
			if (workdir) { if (chdir(workdir) != 0) { /* still try */ } }
			execve(argv[0], argv, env);
			_exit(127);
		}
		_exit(0);
	}
	free(env);
	waitpid(pid, NULL, 0);
	return true;
}


/* ---------------- the Diatom transport ---------------------------------- */

/* One connection, held across games. Diatom is fine with that - it is the
 * fifo pair that needed reopening per game - and holding it means peer death
 * is a POLLHUP here rather than a pid file that lies after a crash. */
static int    dsock = -1;
static char   dbuf[4096];
static size_t dused;
static char   d_preview[1024];
static SDL_Rect d_rect;
static bool     d_rect_known;
static int    d_pend_vol = -1, d_pend_vol_n;      /* LEVEL events, held until */
static int    d_pend_bri = -1, d_pend_bri_n;      /* EXIT hands levels back  */
/* Diatom's QUIET, its ADR-0032: what the launcher wants, and what this
 * connection was last told. -1 is "never told", which a new connection is. */
static int    d_quiet, d_quiet_said = -1;

const char *plat_resident_socket(void)
{
	const char *v = getenv("TORTOS_DIATOM_SOCKET");
	return v ? v : "/tmp/diatom.sock";
}

static void dclose(void)
{
	if (dsock >= 0) close(dsock);
	dsock = -1;
	dused = 0;
}

static bool dsend(const char *fmt, ...)
{
	char line[2048];
	va_list ap;
	int n;

	if (dsock < 0) return false;
	va_start(ap, fmt);
	n = vsnprintf(line, sizeof line - 1, fmt, ap);
	va_end(ap);
	if (n < 0) return false;
	if (n >= (int)sizeof line - 1) n = (int)sizeof line - 2;
	if (n == 0 || line[n - 1] != '\n') line[n++] = '\n';
	/* send and MSG_NOSIGNAL, not write. Nothing in this process ignores
	 * SIGPIPE, so a write into a connection Diatom had closed - it restarted,
	 * or a newer client displaced this one (its ADR-0033) - killed the
	 * launcher outright, and launch.sh had to bring it back. A closed socket
	 * is an answer here, not a crash: the next call connects again. */
	if (send(dsock, line, (size_t)n, MSG_NOSIGNAL) != n) { dclose(); return false; }
	return true;
}

/* One line, or NULL after timeout_ms with nothing complete. -1 blocks. */
static char *dline(int timeout_ms)
{
	static char out[2048];

	for (;;) {
		char *nl = memchr(dbuf, '\n', dused);
		struct pollfd p = { dsock, POLLIN, 0 };
		ssize_t n;

		if (nl) {
			size_t len = (size_t)(nl - dbuf);
			if (len >= sizeof out) len = sizeof out - 1;
			memcpy(out, dbuf, len);
			out[len] = '\0';
			memmove(dbuf, nl + 1, dused - (size_t)(nl - dbuf) - 1);
			dused -= (size_t)(nl - dbuf) + 1;
			return out;
		}
		if (dsock < 0) return NULL;
		if (poll(&p, 1, timeout_ms) <= 0) return NULL;
		n = read(dsock, dbuf + dused, sizeof dbuf - dused);
		if (n <= 0) { dclose(); return NULL; }
		dused += (size_t)n;
	}
}

/* launch.sh writes the pid when it starts Diatom. Verified against
 * /proc/<pid>/cmdline before any signal is sent: a stale file after a crash
 * and respawn would otherwise aim SIGTERM at whoever inherited the number. */
static pid_t diatom_pid(void)
{
	char path[64], cmd[256];
	FILE *f = fopen("/tmp/diatom.pid", "r");
	long v = 0;
	int fd, n;

	if (!f) return -1;
	if (fscanf(f, "%ld", &v) != 1) v = 0;
	fclose(f);
	if (v <= 0) return -1;

	snprintf(path, sizeof path, "/proc/%ld/cmdline", v);
	fd = open(path, O_RDONLY);
	if (fd < 0) return -1;
	n = (int)read(fd, cmd, sizeof cmd - 1);
	close(fd);
	if (n <= 0) return -1;
	cmd[n] = '\0';
	return strstr(cmd, "diatom") ? (pid_t)v : -1;
}

static char d_audio_dev[128];
static bool d_audio_known;

/* Bumped on every successful connect. State the launcher pushed into a PREVIOUS
 * Diatom has to be pushed again into a new one, and this is how a caller tells
 * "still the same emulator" from "a different one that knows nothing". */
static unsigned d_generation;

unsigned plat_resident_generation(void) { return d_generation; }

/* Connect if not connected, and cope with what READY says. state=running
 * means a previous launcher died mid-game and this one just started: the
 * game on screen is real, but this launcher believes it owns the display, so
 * end the session and start clean rather than draw over live output. */
static bool dconnect(void)
{
	struct sockaddr_un a;
	const char *path = plat_resident_socket();
	char *l;

	if (dsock >= 0) return true;
	if (!path) return false;

	/* A new socket may be a new Diatom, which starts on its default output.
	 * Forgetting what the old one reported is what stops the launcher from
	 * believing a sink that this process was never told about. The generation
	 * bump is how a caller notices the same thing without polling for it. */
	d_audio_known = false;
	d_audio_dev[0] = '\0';
	d_quiet_said = -1;
	d_generation++;

	dsock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (dsock < 0) return false;
	memset(&a, 0, sizeof a);
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
	if (connect(dsock, (struct sockaddr *)&a, sizeof a) != 0) { dclose(); return false; }

	while ((l = dline(400))) {
		if (strncmp(l, "READY", 5) == 0) {
			if (strstr(l, "state=running")) {
				unsigned t0 = plat_now_ms();
				fprintf(stderr, "diatom: had a game running; stopping it\n");
				dsend("STOP");
				while ((l = dline(500)) || plat_now_ms() - t0 < 4000)
					if (l && strncmp(l, "EXIT", 4) == 0) break;
			}
			return true;
		}
	}
	/* No READY inside the window: not our protocol on the other end. */
	dclose();
	return false;
}

bool plat_resident_ready(void)
{
	return dconnect();
}



static void (*d_on_unlock)(int id);

void plat_resident_on_unlock(void (*fn)(int id)) { d_on_unlock = fn; }

static void (*d_on_tick)(void);

void plat_resident_on_tick(void (*fn)(void)) { d_on_tick = fn; }

/* Has THIS GAME reported RUNNING yet?
 *
 * A session-long fact, so it cannot live in diatom_wait: the launcher calls
 * that in a loop, returning to it after every in-game menu, and a local reset
 * to 0 on each re-entry made a running game look like one that never started.
 * The cost was not cosmetic - see the ERROR arm in diatom_wait. */
static int d_got_running;

bool plat_resident_send(const char *tag, const char *core, const char *rom,
                        const char *resume, const char *exit_state,
                        const char *preview,
                        int console, const char *cheevos)
{
	run_power_pressed = false;
	/* A new game has not reported RUNNING yet. Cleared HERE rather than in
	 * diatom_wait, because the launcher re-enters that after every in-game
	 * menu and the answer must survive those. */
	d_got_running = 0;

	{
		char *l;
		int vol, bri;

		if (!dconnect()) return false;
		while ((l = dline(0))) { }                 /* drop stale events */
		/* The drain is also what finds a connection Diatom closed under us:
		 * it reads the EOF and closes our end. Connect again rather than
		 * send a game into nothing - which failed the first launch after
		 * Diatom restarted, or after something displaced this connection. */
		if (!dconnect()) return false;
		d_preview[0] = '\0';
		d_rect_known = false;
		d_pend_vol = d_pend_bri = -1;

		/* Before RUN, not after. mgba_sgb_borders and mgba_use_bios are both
		 * marked (Restart) by the core, meaning they are read during
		 * retro_load_game; a SETOPT that arrives after the game is up applies
		 * to the NEXT launch and looks like it did nothing. Diatom holds a
		 * value for a key the active core has not declared and applies it when
		 * one does, so sending the whole list every time is correct. */
		{
			int i;
			for (i = 0; i < plat_coreopt_count(tag); i++) {
				const char *kv = plat_coreopt(tag, i), *eq = strchr(kv, '=');
				if (!eq) continue;
				dsend("SETOPT\tkey=%.*s\tvalue=%s",
				      (int)(eq - kv), kv, eq + 1);
			}
		}

		/* Quiet BEFORE RUN, and on every RUN. Diatom holds it across RUN
		 * (its ADR-0032), so sent first it is in force from the game's first
		 * sample - a game started while music plays never makes a sound -
		 * and sent every time, a resident that restarted cannot start loud. */
		if (dsend("SETQUIET\ton=%d", d_quiet)) d_quiet_said = d_quiet;

		/* console and cheevos on RUN rather than after it, so a set is
		 * watched from the first frame - an achievement can fire in the
		 * opening seconds and Diatom cannot evaluate what it has not been
		 * given yet. Both are ignored by an older Diatom, which is what
		 * ADR-0009 promises about unknown keys. */
		if (!dsend("RUN\tcore=%s\trom=%s\ttag=%s"
		           "\tresume=%s\texit_state=%s\tpreview=%s"
		           "\tconsole=%d\tcheevos=%s",
		           core, rom, tag,
		           resume ? resume : "", exit_state ? exit_state : "",
		           preview ? preview : "",
		           console, cheevos ? cheevos : ""))
			return false;

		/* AFTER RUN, never before: RUN resets the map to identity (Diatom's
		 * ADR-0020, so a table sent for one game cannot silently govern the
		 * next), and a map sent first would be discarded by the very launch it
		 * was meant for. The race is benign - no input reaches a core before
		 * RUNNING, which Diatom emits once the load completes.
		 *
		 * Logged either way. A map that does nothing looks identical to one
		 * that was never sent, one that was refused, and a test rig that could
		 * not press the button - which cost an hour on 2026-08-31. Diatom logs
		 * the receiving half for the same reason. */
		{
			const char *tm = plat_turbo_map(tag);
			fprintf(stderr, "turbo: %s %s\n", tag ? tag : "?",
			        tm && *tm ? tm : "(none)");
			if (tm && *tm) dsend("SETMAP\tmap=%s", tm);
		}

		/* Same reasoning as turbo's SETMAP above: AFTER RUN, which resets
		 * Diatom's bindings to none (its ADR-0035), so a table sent first
		 * would be discarded by the very launch it was meant for. Sent even
		 * when empty - unlike turbo, which skips an empty map - so a game
		 * that HAD bindings last session and had them cleared this one
		 * actually loses them rather than keeping whatever the previous
		 * RUN left behind (Diatom resets to none on every RUN regardless,
		 * so this is belt and suspenders, not load-bearing; sent anyway for
		 * the same "a state that does nothing looks identical to a bug"
		 * reasoning the comment above already gives). */
		{
			const char *hk = plat_hotkey_map(tag);
			fprintf(stderr, "hotkey: %s %s\n", tag ? tag : "?",
			        hk && *hk ? hk : "(none)");
			dsend("SETHOTKEYS\thotkeys=%s", hk ? hk : "");
		}

		/* The launcher owns levels while it draws (Diatom's ADR-0020), and
		 * it is done drawing the moment the game is up - so the last thing
		 * it does is hand over WHERE THE LEVELS ARE. Without this, a game
		 * starts at whatever the mixer was left at rather than at the
		 * volume the shelf shows.
		 *
		 * `count` is POSITIONS, not the top index - Diatom answers with
		 * BRIGHT_LEVELS + 1 and rescales by it. Brightness said 11 for a
		 * twelve-rung ladder, so every level handed to a game arrived one rung
		 * too bright, and the inbound rescale then took a rung off on the way
		 * back. The two canceled often enough to look like nothing was wrong.
		 * Both are written off the shared maxima now. */
		vol = plat_volume_get();
		bri = plat_brightness_get();
		if (vol >= 0) dsend("SETLEVEL\tkind=volume\tindex=%d\tcount=%d",
		                    vol, PLAT_VOL_MAX + 1);
		if (bri >= 0) dsend("SETLEVEL\tkind=brightness\tindex=%d\tcount=%d",
		                    bri, PLAT_BRIGHT_MAX + 1);
		/* And the mute, ON EVERY RUN and not only when it changes. A state
		 * sent only on transition is wrong exactly once - the first time -
		 * which here means a game started with the switch already down comes
		 * up loud. Diatom's ADR-0031. */
		dsend("SETMUTE\ton=%d", plat_muted() ? 1 : 0);
		return true;
	}

}

void plat_resident_quiet(bool on)
{
	d_quiet = on ? 1 : 0;
	/* Only over a connection that exists. Asking to connect for this would
	 * start nothing useful - with no connection there is no game to quiet,
	 * and the next RUN sends it anyway. */
	if (dsock >= 0 && d_quiet != d_quiet_said && dsend("SETQUIET\ton=%d", d_quiet))
		d_quiet_said = d_quiet;
}

bool plat_resident_line(const char *fmt, ...)
{
	char line[1600];
	va_list ap;

	/* Connect if we are not already. dsend refuses on a closed socket, and
	 * before the first game of a session there IS no socket - the connection
	 * was only ever made by plat_resident_send on the way into a game.
	 *
	 * That made every state write from the shelf a silent no-op. ADR-0029's
	 * audio output is the first state the launcher sets while nothing is
	 * running, and it spent 2026-09-05 announcing routes in the log that
	 * Diatom had never been told about, because the line went nowhere and
	 * nobody looked at the return.
	 *
	 * Cheap when it fails: connecting to a unix socket that is not there
	 * returns immediately. */
	if (!dconnect()) return false;

	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	if (dsend("%s", line)) return true;
	/* Closed under us - Diatom restarted, or a newer client displaced this
	 * connection (its ADR-0033). dsend has closed our end, so this is a fresh
	 * connection, and nothing of the line reached the old one to repeat. */
	if (!dconnect()) return false;
	return dsend("%s", line);
}

const char *plat_resident_last_preview(void) { return d_preview; }

/* Block until the game is over, watching the power button throughout -- the
 * same job plat_run does for the one-shot binary, except the signal ends the
 * GAME rather than the process, so residency survives to serve the next one. */
/* One LEVEL line: "LEVEL\tkind=volume\tindex=8\tcount=21". The values are
 * held rather than applied - Diatom owns the hardware while the game runs
 * (its ADR-0020), and applying over the top is exactly the fight the state
 * plane exists to end. They are applied when EXIT hands ownership back. */
/* Where Diatom says the sound ACTUALLY is (its ADR-0029). Not necessarily
 * where it was told to put it: a sink that will not open, or one that died
 * under it, makes the port fall back and say so. Reported to the launcher so a
 * menu can show the truth instead of the request. */
static void d_note_audio(const char *l)
{
	const char *d = strstr(l, "device=");

	if (!d) return;
	snprintf(d_audio_dev, sizeof d_audio_dev, "%s", d + 7);
	d_audio_dev[strcspn(d_audio_dev, "\r\n")] = '\0';
	d_audio_known = true;
}

int plat_resident_volume(int *count)
{
	if (count) *count = d_pend_vol_n;
	return d_pend_vol_n > 1 ? d_pend_vol : -1;
}

void plat_resident_audio_asked(void) { d_audio_known = false; }

bool plat_resident_audio(char *out, size_t cap)
{
	if (!d_audio_known) return false;
	if (out && cap) snprintf(out, cap, "%s", d_audio_dev);
	return true;
}

static void d_note_level(const char *l)
{
	const char *k = strstr(l, "kind=");
	const char *i = strstr(l, "index=");
	const char *c = strstr(l, "count=");
	int idx, cnt;

	if (!k || !i || !c) return;
	idx = atoi(i + 6);
	cnt = atoi(c + 6);
	if (cnt < 2) return;
	if (strncmp(k + 5, "volume", 6) == 0)          { d_pend_vol = idx; d_pend_vol_n = cnt; }
	else if (strncmp(k + 5, "brightness", 10) == 0) { d_pend_bri = idx; d_pend_bri_n = cnt; }
}

/* "CHEEVO\tid=24698\tstate=unlocked". Diatom sends this the moment a
 * condition fires, which is mid-game, when the emulator owns the display and
 * this process cannot draw a thing. So it is recorded and shown at the next
 * moment the launcher owns the screen: the in-game menu, or the shelf. */
static void d_note_cheevo(const char *l)
{
	const char *i = strstr(l, "id=");
	const char *st = strstr(l, "state=");

	if (!i || !st) return;
	if (strncmp(st + 6, "unlocked", 8) != 0) return;
	if (d_on_unlock) d_on_unlock(atoi(i + 3));
}

/* "DISPLAY\tmode=native\tfilter=nearest\trect=256x224+384+272" */
static void d_note_display(const char *l)
{
	const char *r = strstr(l, "rect=");
	int w, h, x, y;

	if (!r || sscanf(r + 5, "%dx%d+%d+%d", &w, &h, &x, &y) != 4) return;
	if (w <= 0 || h <= 0) return;
	d_rect.w = w; d_rect.h = h; d_rect.x = x; d_rect.y = y;
	d_rect_known = true;
}

bool plat_resident_rect(SDL_Rect *out)
{
	if (!d_rect_known) return false;
	*out = d_rect;
	return true;
}

/* Only safe while Diatom is paused, which is the only place it is called from:
 * a game that is running can send EXIT, and this would eat it. Paused, the only
 * traffic is what our own SETDISPLAY provoked. */
bool plat_resident_sync_rect(int timeout_ms)
{
	unsigned t0 = SDL_GetTicks();
	char *l;

	if (dsock < 0) return false;
	while ((int)(SDL_GetTicks() - t0) < timeout_ms) {
		l = dline(timeout_ms);
		if (!l) break;
		if (strncmp(l, "DISPLAY\t", 8) == 0) { d_note_display(l); return true; }
		if (strncmp(l, "LEVEL\t", 6) == 0) d_note_level(l);
		else if (strncmp(l, "AUDIO\t", 6) == 0) d_note_audio(l);
	}
	return false;
}

static void d_apply_levels(void)
{
	/* Through the public setters (defined below, past this point in the
	 * file): they own the device handles. Both sides use the same ladders, so
	 * `count` matches and the rescale is the identity - but it is written as a
	 * rescale anyway, because a launcher should not break if the emulator ever
	 * changes its scale.
	 *
	 * The rescale has to target THIS side's top of range. It read 10 while the
	 * brightness ladder had grown to twelve rungs, which is not the identity:
	 * a level of 7 came back as 6, and every brightness set inside a game lost
	 * a rung on the way out. That is why the maxima are in platform.h now. */
	if (d_pend_vol >= 0 && d_pend_vol_n > 1)
		plat_volume_set_pct((d_pend_vol * 100 + (d_pend_vol_n - 1) / 2)
		                    / (d_pend_vol_n - 1));
	if (d_pend_bri >= 0 && d_pend_bri_n > 1)
		plat_brightness_set((d_pend_bri * PLAT_BRIGHT_MAX + (d_pend_bri_n - 1) / 2)
		                    / (d_pend_bri_n - 1));
	d_pend_vol = d_pend_bri = -1;
}

static int diatom_wait(void)
{
	int sent_stop = 0;
	unsigned stop_at = 0, start = plat_now_ms();
	int autostop_s = getenv("TORTOS_AUTOSTOP_S") ? atoi(getenv("TORTOS_AUTOSTOP_S")) : 0;
	bool pwr_down = false;   /* the button's level, not just its edges - see below */

#ifdef __linux__
	{
		struct input_event ev;
		while (fd_power >= 0 && read(fd_power, &ev, sizeof ev) == (ssize_t)sizeof ev)
			; /* drain stale presses */
	}
#endif

	for (;;) {
		char *l;

		if (dsock < 0) return RES_DEAD;
		/* THE MUTE SWITCH IS READ DURING A GAME TOO, and this is the half
		 * that matters: a player reaching for it is almost always reaching
		 * for it because a game is loud. plat_input_poll is not called from
		 * here, so without this the switch does nothing for as long as
		 * anything is playing.
		 *
		 * This loop turns every 100ms - dline's timeout - which is the whole
		 * reason the launcher can own the switch at all. An earlier design
		 * assumed this side was blocked and handed the job to the emulator.
		 *
		 * The cut reaches the game's audio because HpSpeaker Switch sits
		 * below the mixer: measured 2026-09-16 by muting a tone played from a
		 * second process while Diatom held the dmix slave. See BACKLOG 28 and
		 * Diatom's ADR-0031. */
		plat_mute_poll(false);
		while ((l = dline(100))) {
			if      (strncmp(l, "RUNNING", 7) == 0) d_got_running = 1;
			else if (strncmp(l, "PAUSED", 6) == 0)  return RES_PAUSED;
			else if (strncmp(l, "PREVIEW\tpath=", 13) == 0)
				snprintf(d_preview, sizeof d_preview, "%s", l + 13);
			else if (strncmp(l, "LEVEL\t", 6) == 0) d_note_level(l);
			else if (strncmp(l, "AUDIO\t", 6) == 0) d_note_audio(l);
			else if (strncmp(l, "DISPLAY\t", 8) == 0) d_note_display(l);
			else if (strncmp(l, "CHEEVO\t", 7) == 0) d_note_cheevo(l);
			/* Nobody has pressed anything for as long as the player asked.
			 * Sleep needs none of what follows - it freezes the game
			 * along with everything else and there is nothing to stop -
			 * so only the poweroff decision still goes through STOP and
			 * the launcher's existing after-the-game check. */
			else if (strncmp(l, "IDLE", 4) == 0 && !sent_stop) {
				if (plat_power_idle_action() == PWR_SLEEP) {
					plat_sleep();
				} else {
					run_power_pressed = true;
					sent_stop = 1;
					stop_at = plat_now_ms();
					dsend("STOP");
				}
			}
			else if (strncmp(l, "EXIT", 4) == 0) {
				/* A crash and a quit arrive on the SAME line, and only the
				 * reason tells them apart. This threw the line away and
				 * returned, so a game that died of SIGSEGV was logged as
				 * "exit: back in 50 ms" - identical to backing out of a
				 * game normally, and the only hint that anything was wrong
				 * came later, from the resident being gone.
				 *
				 * Diatom has always sent it: on_crash emits
				 * "EXIT reason=crash signal=SIGSEGV" from the signal
				 * handler, and ADR-0009 makes `signal=` free to add because
				 * unknown keys are ignored. It was reported and discarded,
				 * which is worse than never having been sent - it looks
				 * like a firmware with nothing to say about the failure.
				 *
				 * Only the ones that are not a clean quit. Logging every
				 * EXIT would put a line in the log for every game anyone
				 * ever finishes, and a log that says something on every
				 * exit says nothing about the interesting ones. */
				if (strncmp(l, "EXIT\treason=user", 16) != 0)
					fprintf(stderr, "diatom: %s\n", l);
				d_apply_levels();
				return RES_EXIT;
			}
			else if (strncmp(l, "ERROR", 5) == 0) {
				fprintf(stderr, "diatom: %s\n", l);
				/* Before RUNNING it means the game never started and the
				 * display is still ours. After, it is a failed menu op the
				 * menu already showed; the game is still going.
				 *
				 * Getting this wrong is expensive, which is why the flag is
				 * no longer a local. Reported 2026-09-06: Load -> Auto asked
				 * for a state that could not exist, the ERROR arrived on a
				 * re-entry where the local had reset, and a failed menu
				 * operation was read as a dead emulator. The launcher then
				 * started a SECOND emulator while the first still held the
				 * framebuffer - two presenters, which wedges the display
				 * engine in-kernel, and the supervisor powered the device
				 * off. */
				if (!d_got_running) return RES_DEAD;
			}
		}
		if (dsock < 0) return RES_DEAD;            /* EOF mid-game */

		/* The launcher's own slice of the game session. Bounded work only -
		 * see plat_resident_on_tick. */
		if (d_on_tick) d_on_tick();

#ifdef __linux__
		{
			struct input_event ev;
			while (fd_power >= 0 &&
			       read(fd_power, &ev, sizeof ev) == (ssize_t)sizeof ev)
				if (ev.type == EV_KEY && ev.code == KEY_POWER)
					pwr_down = ev.value != 0;

			/* plat_input_poll isn't running - the game owns input - so this
			 * is the only place watching the level at all; checked every
			 * pass regardless of whether a new event just arrived, the same
			 * as the shelf/menu path checks a->in.down every frame. */
			if (!sent_stop) {
				pwr_action pa = plat_power_tap_or_hold(pwr_down);
				if (pa == PWR_SLEEP) {
					plat_sleep();
				} else if (pa == PWR_POWEROFF) {
					run_power_pressed = true;
					sent_stop = 1;
					stop_at = plat_now_ms();
					dsend("STOP");
				}
			}
		}
#endif
		/* A termination arriving mid-game. Without this the loop waits for a
		 * game that nobody is going to end, and the process cannot be
		 * signaled out of it - which is how `killall tortos.elf` came to do
		 * nothing at all while a game was up. STOP, then the existing
		 * escalation below applies if the core will not honor it. */
		if (g_terminating && !sent_stop) {
			sent_stop = 1;
			stop_at = plat_now_ms();
			dsend("STOP");
		}

		if (autostop_s > 0 && !sent_stop &&
		    plat_now_ms() - start > (unsigned)autostop_s * 1000u) {
			sent_stop = 1;
			stop_at = plat_now_ms();
			dsend("STOP");
		}

		/* STOP is a request; a core wedged inside retro_run cannot honor
		 * it. Escalate on the clock: SIGTERM still flushes saves, SIGKILL
		 * is the end of the line and reports the emulator dead. */
		if (sent_stop && stop_at) {
			unsigned waited = plat_now_ms() - stop_at;
			pid_t pid = diatom_pid();
			if (waited > 5000) {
				if (pid > 0) kill(pid, SIGKILL);
				dclose();
				return RES_DEAD;
			}
			if (waited > 2500 && pid > 0) kill(pid, SIGTERM);
		}
	}
}

/* Declared with the ladder, defined far below it; called here because this is
 * the one place the launcher is known to be taking input back. */
static void jack_forget(void);
static void mute_forget(void);

int plat_resident_wait(void)
{
	int r = diatom_wait();

	/* Input ownership just came back to this process, so anything remembered
	 * about the jack was formed while something else was driving.
	 *
	 * A cable that went in or out DURING the game was seen by Diatom and not
	 * by this side, which leaves jack_was describing a world that is over. The
	 * poll then compares the hardware against that memory, finds them equal,
	 * and returns without re-mapping - so a headphone-window value stays in
	 * the register with the jack out. Measured 2026-09-05: 29, against a
	 * speaker window whose quiet end is 39, which is a working speaker at
	 * almost no volume.
	 *
	 * It is here rather than at the call site because forgetting it is not
	 * optional and a caller cannot be relied on to remember. Diatom does the
	 * same on its side, in diatom_port_level_invalidate. */
	jack_forget();
	mute_forget();
	return r;
}

void plat_request_poweroff(void)
{
	FILE *f = fopen(TORTOS_POWEROFF_FLAG, "w");
	if (f) fclose(f);
}

#ifdef __linux__
static void write_str(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0) return;
	if (write(fd, val, strlen(val)) < 0) { /* best effort */ }
	close(fd);
}
#endif

void plat_leds_off(void)
{
#ifdef __linux__
	/* Stop the animation engine, scale every group to zero, then zero all 23
	 * raw channels directly -- so it holds whatever state the stock input
	 * daemon left the engine in. */
	write_str("/sys/class/led_anim/effect_enable", "0");
	static const char *groups[] = { "l", "r", "lr", "m", "f1", "f2" };
	char path[96];
	for (size_t i = 0; i < sizeof groups / sizeof *groups; i++) {
		snprintf(path, sizeof path, "/sys/class/led_anim/effect_rgb_hex_%s", groups[i]);
		write_str(path, "000000 ");
	}
	write_str("/sys/class/led_anim/max_scale", "0");
	write_str("/sys/class/led_anim/max_scale_lr", "0");
	write_str("/sys/class/led_anim/max_scale_f1f2", "0");
	for (int n = 0; n < 23; n++) {
		for (const char *c = "rgb"; *c; c++) {
			snprintf(path, sizeof path,
			         "/sys/class/leds/sunxi_led%d%c/brightness", n, *c);
			write_str(path, "0");
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

#define SPEAKER_CTL  "HpSpeaker Switch"   /* the only true mute on this codec */
#define HP_CTL       "Headphone Volume"   /* 0-7, 6 dB a step, INVERTED */
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

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static int mixer_fd = -1, disp_fd = -1;
static int cur_vol = -1, cur_bright = -1;

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
static void levels_save(void)
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
 * GAIN_CTL instead - see HP_RAW_TOP. */
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

static bool mute_switch_down(void)
{
	char c = 0;

	if (mute_fd < 0) mute_fd = open(MUTE_GPIO, O_RDONLY | O_CLOEXEC);
	if (mute_fd < 0) return false;
	if (pread(mute_fd, &c, 1, 0) != 1) return false;
	return c == '1';
}

static void apply_volume(int v)
{
	int hp = jack_present();
	long raw = aout_level_to_raw(v, VOL_MAX,
	                             hp ? HP_RAW_TOP : SPK_RAW_TOP,
	                             hp ? HP_RAW_BOTTOM : SPK_RAW_BOTTOM);
	long on;

	jack_was = hp;
	cur_vol = v;
	ctl_io(GAIN_CTL, &raw, 1);
	/* Zero has to cut the path, not merely attenuate it - and so does the
	 * switch, which is why it lands here rather than beside the callers: every
	 * route that re-applies a level (a nudge, a jack coming out) passes
	 * through this line and honors the switch for free. */
	on = (v > 0) && muted != 1;
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
	int now = mute_switch_down() ? 1 : 0;

	if (now == muted) return false;
	muted = now;
	/* Tell the resident whichever side of a game we are on. It cuts the stage
	 * itself below, which is what makes the switch feel immediate; this is
	 * what stops Diatom putting it back on its next level write. Harmless
	 * with no game running - the resident is there either way, and knowing
	 * early means a RUN cannot race the flip. */
	dsend("SETMUTE\ton=%d", muted == 1 ? 1 : 0);
	if (own_volume) {
		if (cur_vol >= 0) apply_volume(cur_vol);
		return true;
	}
	/* DURING A GAME, ONLY THE SWITCH - never the gain.
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
	 * period. The SETMUTE above is what makes the cut STICK, since Diatom
	 * writes the same control whenever it applies a level (ADR-0031). Neither
	 * alone is enough - one is fast and one is durable. */
	{
		long on = (muted != 1);

		ctl_io(SPEAKER_CTL, &on, 1);
	}
	return true;
}

bool plat_muted(void) { return muted == 1; }

/* Whatever this side remembers about the switch was formed while it was
 * driving; a game just was. Called where jack_forget is, and for its reason. */
static void mute_forget(void) { muted = -1; }

void plat_audio_jack_poll(void)
{
	if (!aout_should_reapply(jack_was, jack_present() != 0, cur_vol >= 0))
		return;
	apply_volume(cur_vol);
}

/* Forget which way the jack was, so the next poll re-applies whatever the
 * hardware says now instead of trusting a memory formed before a handover. */
static void jack_forget(void) { jack_was = -1; }

static void apply_brightness(int b)
{
	unsigned long a[4] = { 0, 0, 0, 0 };

	cur_bright = b;
	if (disp_fd < 0) return;
	a[1] = bright_ladder[b];
	ioctl(disp_fd, DISP_LCD_SET_BRIGHTNESS, a);
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
static int cur_vol = 8, cur_bright = 7;
static int mixer_fd = 0, disp_fd = 0;    /* "present", so the nudges run */
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static void levels_save(void) { }
static void apply_volume(int v) { cur_vol = v; }
static void apply_brightness(int b) { cur_bright = b; }
/* No jack on the host, so nothing can be plugged into it. */
void plat_audio_jack_poll(void) { }
/* And no switch to flip. */
bool plat_mute_poll(bool own_volume) { (void)own_volume; return false; }
bool plat_muted(void) { return false; }
bool plat_headphones_present(void) { return false; }
static void jack_forget(void) { }
static void mute_forget(void) { }

/* No settings database on the host, so the config defaults are all there is.
 * Taken anyway rather than ignored: a shelf rendered by --shot should show the
 * OSD at the levels the card is configured for. */
void plat_settings_init(void)
{
	/* The host has no codec and no display engine, so the levels above are
	 * all there is and nothing needs applying to hardware. */
}

#endif  /* __linux__ */


/* The settings indicator: a thin line across the very top on any volume or
 * brightness change, tinted by which of the two it is -- warm for brightness,
 * cyan for volume. Diatom draws its own thin bar in game for the same reason,
 * so the feedback is one thing everywhere instead of a launcher line here and
 * another firmware's pill there; the tint is the part Diatom has to be taught
 * to match. Still no glyph and no number: the color and the button you just
 * pressed agree, and neither needs a label. */
#define OSD_LINE_H     UI_BAR_H
#define OSD_PAD        3    /* half-black scrim above and below */
#define OSD_WINDOW_MS  900  /* visible this long after the last change */

static int    osd_kind = 0; /* 0 none, 1 brightness, 2 volume */
static int    osd_val = 0, osd_max = 1;
static Uint32 osd_shown_at = 0;

void plat_osd_show(int kind, int val, int max)
{
	osd_kind = kind;
	osd_max = max > 0 ? max : 1;
	osd_val = val < 0 ? 0 : (val > osd_max ? osd_max : val);
	osd_shown_at = SDL_GetTicks();
}

/* One past the last tick plat_draw_osd still draws it on, since that test is
 * `>`: asking at exactly the window's end would still find it showing. */
Uint32 plat_osd_until(void)
{
	return osd_kind ? osd_shown_at + OSD_WINDOW_MS + 1 : UINT32_MAX;
}

int plat_volume_get(void)     { return cur_vol; }
int plat_brightness_get(void) { return cur_bright; }

void plat_draw_osd(SDL_Renderer *r)
{
	if (!osd_kind) return;
	if (SDL_GetTicks() - osd_shown_at > OSD_WINDOW_MS) { osd_kind = 0; return; }

	float pct = (float)osd_val / osd_max;
	if (pct < 0) pct = 0; else if (pct > 1) pct = 1;

	SDL_Color fill = osd_kind == 1 ? UI_OSD_BRIGHT : UI_OSD_VOLUME;
	int W = TORTOS_SCREEN_W;
	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(r, 0, 0, 0, 128);                 /* scrim */
	SDL_RenderFillRect(r, &(SDL_Rect){ 0, 0, W, OSD_LINE_H + OSD_PAD * 2 });
	SDL_SetRenderDrawColor(r, 60, 62, 72, 255);              /* faint track */
	SDL_RenderFillRect(r, &(SDL_Rect){ 0, OSD_PAD, W, OSD_LINE_H });
	SDL_SetRenderDrawColor(r, fill.r, fill.g, fill.b, 255);
	SDL_RenderFillRect(r, &(SDL_Rect){ 0, OSD_PAD, (int)(W * pct), OSD_LINE_H });
}

void plat_volume_nudge(int delta)
{
	if (mixer_fd < 0) return;
	apply_volume(clampi((cur_vol < 0 ? 0 : cur_vol) + delta, 0, VOL_MAX));
	levels_save();
	plat_osd_show(2, cur_vol, VOL_MAX);
}

void plat_brightness_nudge(int delta)
{
	if (disp_fd < 0) return;
	apply_brightness(clampi(cur_bright + delta, 0, BRIGHT_MAX));
	levels_save();
	plat_osd_show(1, cur_bright, BRIGHT_MAX);
}

void plat_volume_set_pct(int pct)
{
	if (mixer_fd < 0) return;
	apply_volume(clampi((pct * VOL_MAX + 50) / 100, 0, VOL_MAX));
	levels_save();
}

void plat_brightness_set(int level)
{
	if (disp_fd < 0) return;
	apply_brightness(clampi(level, 0, BRIGHT_MAX));
	levels_save();
}

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

void plat_sleep(void)
{
#ifdef __linux__
	char states[128] = { 0 };
	int fd, saved;
	ssize_t n;
	bool bt_was_up, wifi_was_up;
	int fd2, tries;

	if (!plat_sleep_supported()) return;

	fd = open("/sys/power/state", O_RDONLY);
	if (fd < 0) return;
	n = read(fd, states, sizeof states - 1);
	close(fd);
	if (n <= 0) return;
	states[n] = '\0';
	if (!strstr(states, "mem")) return;   /* listed states don't include it */

	/* apply_brightness(), not plat_brightness_set(): this dims the panel and
	 * puts it back, and must not persist a sleep-only value as the player's
	 * chosen brightness - see levels_save() in plat_brightness_set above. */
	saved = cur_bright;
	apply_brightness(0);

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
	for (tries = 0; tries < 5; tries++) {
		time_t went, woke;
		ssize_t wrote;

		went = time(NULL);
		fd2 = open("/sys/power/state", O_WRONLY);
		wrote = fd2 < 0 ? -1 : write(fd2, "mem", 3);
		if (fd2 >= 0) close(fd2);
		if (wrote == 3) break;

		woke = time(NULL);
		if (woke - went > 5) break;   /* false-negative override */
		sleep(3);
	}

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

	apply_brightness(saved);
#endif
}

#ifdef __linux__
/* Blocks up to ms waiting for a power-button event on the raw evdev fd -
 * press or release, either is evidence someone is at the device. Used only
 * to detect wake, never tap/hold intent (plat_power_tap_or_hold already
 * owns that), so any KEY_POWER event is enough. */
static bool power_event_within(int ms)
{
	struct pollfd pfd;
	struct input_event ev;

	if (fd_power < 0) return false;
	pfd.fd = fd_power;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, ms) <= 0) return false;
	while (read(fd_power, &ev, sizeof ev) == (ssize_t)sizeof ev)
		if (ev.type == EV_KEY && ev.code == KEY_POWER) return true;
	return false;
}
#endif

/* Task B (TortOS-1v7.1.2.3): the intermediate power state entered when Auto
 * Sleep (not Auto Off) is the active setting and idle fires - mirrors
 * NextUI's PWR_enterSleep/PWR_waitForWake/PWR_exitSleep shape (screen off,
 * audio muted, wait for the power button, restore both) but waits
 * indefinitely, with none of PWR_waitForWake's own further suspend-timeout
 * escalation into deep sleep/poweroff: Auto Sleep and Auto Off are
 * mutually exclusive (task A), so there is nothing for this state to
 * escalate into - Auto Off's real suspend (task D) only ever fires from
 * its own independent idle branch, never out of this loop. The CPU stays
 * awake throughout and plat_sleep() is never called here - this is
 * NextUI's "hybrid sleep," a screen-off idle state, not a kernel suspend. */
void plat_light_sleep(void)
{
#ifdef __linux__
	int saved_bright = cur_bright;
	int saved_vol = cur_vol;

	apply_brightness(0);
	plat_volume_set_pct(0);

	while (!power_event_within(200))
		;

	apply_brightness(saved_bright);
	if (mixer_fd >= 0 && saved_vol >= 0) apply_volume(saved_vol);
#endif
}

int plat_suspend_timeout_secs(void)
{
	int v = db_get_int(db_dev(), "suspendtimeout", 30);
	return v > 0 ? v : 30;          /* NextUI's default; 0 is not a choice */
}

#define POWER_HOLD_MS 400u

/* The one press either caller could currently be watching - a->in.down every
 * frame from the shelf/menu path, or a level tracked locally from raw evdev
 * once a tick during a game. Only one of those runs at a time, so one timer
 * is enough. 0 means no press is open right now. */
static unsigned pwr_since;

pwr_action plat_power_tap_or_hold(bool down)
{
	unsigned now = plat_now_ms();

	if (down) {
		if (!pwr_since) pwr_since = now ? now : 1;
		if (now - pwr_since >= POWER_HOLD_MS) {
			pwr_since = 0;          /* fire once per press */
			return PWR_POWEROFF;
		}
		return PWR_NONE;
	}
	if (!pwr_since) return PWR_NONE;   /* not pressed, nothing just ended */
	pwr_since = 0;
	return (plat_sleep_supported() && db_get_int(db_dev(), "power.tap", 1))
	       ? PWR_SLEEP : PWR_POWEROFF;
}

pwr_action plat_power_idle_action(void)
{
	return (plat_sleep_supported() && db_get_int(db_dev(), "power.idle", 1))
	       ? PWR_SLEEP : PWR_POWEROFF;
}

/* ---- battery ---- */

bool plat_battery(int *pct, bool *charging)
{
	const char *fake = getenv("TORTOS_FAKE_BATT");
	if (fake && *fake) {
		if (pct) *pct = atoi(fake);
		if (charging) *charging = false;
		return true;
	}
#ifdef __linux__
	FILE *f = fopen("/sys/class/power_supply/axp2202-battery/capacity", "r");
	if (!f) return false;
	int v = -1;
	if (fscanf(f, "%d", &v) != 1) v = -1;
	fclose(f);
	if (v < 0) return false;
	if (pct) *pct = v;
	if (charging) {
		*charging = false;
		FILE *s = fopen("/sys/class/power_supply/axp2202-battery/status", "r");
		if (s) {
			char st[32] = { 0 };
			if (fgets(st, sizeof st, s) &&
			    (strncmp(st, "Charging", 8) == 0 || strncmp(st, "Full", 4) == 0))
				*charging = true;
			fclose(s);
		}
	}
	return true;
#else
	(void)pct;
	(void)charging;
	return false;
#endif
}
