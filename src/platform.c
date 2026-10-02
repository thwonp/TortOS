/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include "atomic.h"
#include "db.h"
#include "platform.h"
#include "audioout.h"

#include "ui.h"   /* the settings line shares the rail's weight and palette */
#include "platform_dev.h"

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

/* Hold-to-repeat. Short delay and a quick rate: this is a shelf you sweep
 * along, and waiting on a key repeat is the most obvious kind of slow. */
#define REPEAT_DELAY_MS 300
#define REPEAT_RATE_MS  90

int fd_power = -1; /* axp2202-pek: KEY_POWER */

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
static struct { char tag[8]; char map[128]; } hotkeys[HOTKEY_MAX];
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

/* ---- the hotkey modifier: one key, chosen per device, held for every binding
 * above (diatom's ADR-0038, plorpos-gkd.43.1). Global, not per system, so the
 * muscle memory is the same in every game. The first entry is the default.
 * Stick Click is the Brick Pro's; on the plain Brick that index is a front
 * brightness key. */
#if defined(PLATFORM_GKD)
/* The GKD's free Home key and its stick click as well (ADR-0037). */
static const char *const HKMOD_WIRE[]  = { "menu", "home", "l3", "select" };
static const char *const HKMOD_LABEL[] = { "Menu", "Home", "Stick Click", "Select" };
#else
static const char *const HKMOD_WIRE[]  = { "menu", "select", "l3" };
static const char *const HKMOD_LABEL[] = { "Menu", "Select", "Stick Click" };
#endif

int plat_hotkey_modifiers(const char *const **wire, const char *const **label)
{
	*wire = HKMOD_WIRE;
	*label = HKMOD_LABEL;
#if defined(PLATFORM_GKD)
	return 4;
#else
	return plat_is_brick_pro() ? 3 : 2;
#endif
}

/* The stored choice if this device offers it, else the default - never NULL. */
const char *plat_hotkey_modifier(void)
{
	static char cur[16];
	const char *const *wire, *const *label;
	int i, n = plat_hotkey_modifiers(&wire, &label);

	db_get_str(db_dev(), "hkmod", cur, sizeof cur, wire[0]);
	for (i = 0; i < n; i++)
		if (!strcmp(cur, wire[i])) return wire[i];
	return wire[0];
}

void plat_hotkey_modifier_set(const char *wire)
{
	db_set_str(db_dev(), "hkmod", wire);
}

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

unsigned plat_now_ms(void) { return SDL_GetTicks(); }

/* Set from main's SIGTERM handler. Surfaced here rather than checked at each
 * call site because `quit_requested` is already polled by every loop in the
 * program, so one assignment reaches all of them - including modal loops that
 * were written later and would otherwise have to remember. The keyboard was
 * exactly that case: it checked quit_requested and not main's want_quit, so
 * SIGTERM was ignored while it was open and `make adb-restart` silently did
 * nothing until the process was killed with -9. */
volatile sig_atomic_t g_terminating;

void plat_terminate(void) { g_terminating = 1; }

void set_btn(in_state *st, in_button b, bool down)
{
	if (b == IN_NONE) return;
	if (down && !st->down[b]) {
		st->pressed[b] = true;
		st->down_since[b] = SDL_GetTicks();
		st->last_repeat[b] = 0;
	}
	st->down[b] = down;
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

static unsigned run_asleep_ms;
static unsigned run_menu_at;   /* plat_now_ms of the last Menu press */
bool plat_run_power_pressed(void) { return run_power_pressed; }
unsigned plat_run_asleep_ms(void) { return run_asleep_ms; }
unsigned plat_run_menu_age_ms(void) { return plat_now_ms() - run_menu_at; }
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

/* fork and exec, with the environment child_environ built before the fork. */
static pid_t run_fork(char *const argv[], char **env, const char *workdir)
{
	size_t alen = strlen(argv[0]);
	pid_t pid = fork();

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
	return pid;
}

#ifdef __linux__
/* End a child and wait for it: TERM, then KILL after 3 s. CONT as well, as a
 * stopped process does not act on TERM. For Reset, which has to know the old
 * one is gone before it starts the new. */
static void run_end(pid_t pid)
{
	int ms, status;

	kill(pid, SIGTERM);
	kill(pid, SIGCONT);
	for (ms = 0; ms < 3000; ms += 100) {
		if (waitpid(pid, &status, WNOHANG) != 0) return;
		usleep(100 * 1000);
	}
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
}
#endif

int plat_run(char *const argv[], const char *const envkv[], const char *workdir,
             run_menu_fn on_menu, void *ctx)
{
	pid_t pid;
	char **env = child_environ(envkv);
	int status = 0;

	if (!env) return -1;
	run_power_pressed = false;
	run_asleep_ms = 0;
	pid = run_fork(argv, env, workdir);
	if (pid < 0) { free(env); return -1; }
#ifdef __linux__
	/* Power as in a resident game: a tap sleeps with the child frozen and
	 * thawed on wake, a hold ends it and powers off (the caller's
	 * plat_run_power_pressed). The hold is also the escape hatch for a core
	 * that dead-ends on an error screen that eats no input. Menu, when
	 * asked, freezes the child under the caller's menu: native PICO-8 has
	 * no menu a pad can reach.
	 *
	 * Woken by the keys, not a 100 ms nap: the menu has to be up within
	 * 100 ms of the press (user, 2026-10-02), and the nap alone was up to
	 * all of it. The timeout is for the hold and the TERM->KILL clock. */
	struct input_event ev;
	bool pwr_down = false;
	int menu_code = 0;
	int fd_menu = on_menu ? menu_key(&menu_code) : -1;
	struct pollfd pfd[2] = { { fd_power, POLLIN, 0 }, { fd_menu, POLLIN, 0 } };
	while (fd_power >= 0 && read(fd_power, &ev, sizeof ev) == (ssize_t)sizeof ev)
		; /* drain stale events */
	while (fd_menu >= 0 && read(fd_menu, &ev, sizeof ev) == (ssize_t)sizeof ev)
		;
	unsigned term_at = 0;   /* when TERM went, 0: not ending it */
	for (;;) {
		pid_t r = waitpid(pid, &status, WNOHANG);
		if (r == pid) break;
		if (r < 0) { free(env); return -1; }
		poll(pfd, 2, 100);
		bool quit = false, menu = false;
		while (fd_power >= 0 && read(fd_power, &ev, sizeof ev) == (ssize_t)sizeof ev)
			if (ev.type == EV_KEY && ev.code == KEY_POWER)
				pwr_down = ev.value != 0;
		if (!term_at) {
			pwr_action pa = plat_power_tap_or_hold(pwr_down);

			if (pa == PWR_SLEEP) {
				unsigned t0 = plat_now_ms();
				bool awake;

				kill(pid, SIGSTOP);
				awake = plat_light_sleep(0);
				kill(pid, SIGCONT);
				run_asleep_ms += plat_now_ms() - t0;
				fprintf(stderr, "sleep: %s after %us (child)\n",
				        awake ? "awake" : "no suspend, powering off",
				        (plat_now_ms() - t0) / 1000);
				pwr_down = false;
				if (!awake) pa = PWR_POWEROFF;
			}
			if (pa == PWR_POWEROFF) {
				run_power_pressed = true;
				quit = true;
			}
		}
		while (fd_menu >= 0 && read(fd_menu, &ev, sizeof ev) == (ssize_t)sizeof ev)
			if (ev.type == EV_KEY && ev.code == menu_code && ev.value == 1) {
				struct timespec now;

				/* The press's own time, on the clock the kernel stamped it
				 * with, carried over to plat_now_ms - what the caller's
				 * press-to-menu figure is measured from. */
				clock_gettime(CLOCK_REALTIME, &now);
				run_menu_at = plat_now_ms() -
				    (unsigned)((now.tv_sec - ev.time.tv_sec) * 1000 +
				               (now.tv_nsec / 1000 - ev.time.tv_usec) / 1000);
				menu = true;
			}
		if (menu && !quit && !term_at) {
			run_choice c = RUN_QUIT;

			kill(pid, SIGSTOP);
			if (child_hide(pid)) {
				fprintf(stderr, "run: menu, child %d frozen\n", (int)pid);
				c = on_menu(ctx);
				child_restore(pid, c == RUN_CONTINUE);
				fprintf(stderr, "run: %s\n", c == RUN_CONTINUE ? "continue"
				        : c == RUN_RESET ? "reset" : "quit");
				/* What the menu was driven with is not for this loop. */
				while (fd_power >= 0 && read(fd_power, &ev, sizeof ev) == (ssize_t)sizeof ev)
					;
				while (fd_menu >= 0 && read(fd_menu, &ev, sizeof ev) == (ssize_t)sizeof ev)
					;
				pwr_down = false;
			}
			if (c == RUN_CONTINUE) kill(pid, SIGCONT);
			else if (c == RUN_RESET) {
				run_end(pid);
				if ((pid = run_fork(argv, env, workdir)) < 0) { free(env); return -1; }
			} else quit = true;
		}
		if (quit && !term_at) {
			kill(pid, SIGTERM);
			kill(pid, SIGCONT);   /* a frozen child acts on TERM only once thawed */
			term_at = plat_now_ms() | 1;
		}
		if (term_at && plat_now_ms() - term_at > 3000) {
			kill(pid, SIGKILL);
			term_at = 0;
		}
	}
#else
	(void)on_menu; (void)ctx;
	if (waitpid(pid, &status, 0) < 0) { free(env); return -1; }
#endif
	free(env);
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

bool dsend(const char *fmt, ...)
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
			dsend("SETHOTKEYS\thotkeys=%s\tmodifier=%s", hk ? hk : "",
			      plat_hotkey_modifier());
		}
#if defined(PLATFORM_GKD)
		/* The player's rewind speed (plorpos-gkd.40): Diatom resets it to
		 * its build default on every RUN, so it is re-sent every run, as
		 * SETHOTKEYS is. Set on the Hotkeys screen. */
		dsend("SETREWINDSPEED\tevery=%d", db_get_int(db_dev(), "rewindspeed", 5));
#endif

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

/* ---- geometry --------------------------------------------------------- */

int plat_screen_h = 768;
static float g_scale = 1.0f;

float plat_scale(void) { return g_scale; }

/* At scale 1 the renderer is left exactly as SDL made it, so the Brick draws
 * the same bytes it did before there was a scale. */
void plat_geometry_init(SDL_Renderer *r)
{
	int w = 0, h = 0;

	if (SDL_GetRendererOutputSize(r, &w, &h) != 0 || w <= 0 || h <= 0) return;
	g_scale = (float)w / TORTOS_SCREEN_W;
	plat_screen_h = (int)(h / g_scale);
	if (g_scale != 1.0f) SDL_RenderSetScale(r, g_scale, g_scale);
}

/* Diatom's rect is in panel pixels, so this one copy is made at scale 1: the
 * frame lands where the game is, at the game's size, with no second resample.
 * A render scale set on the default target is not reset by switching targets,
 * so restoring it is enough. */
void plat_draw_paused(SDL_Renderer *r, SDL_Texture *bg)
{
	SDL_Rect d;

	if (!plat_resident_rect(&d)) { SDL_RenderCopy(r, bg, NULL, NULL); return; }
	if (g_scale == 1.0f) { SDL_RenderCopy(r, bg, NULL, &d); return; }
	SDL_RenderSetScale(r, 1.0f, 1.0f);
	SDL_RenderCopy(r, bg, NULL, &d);
	SDL_RenderSetScale(r, g_scale, g_scale);
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

bool plat_resident_saved(const char *path, int timeout_ms)
{
	unsigned t0 = SDL_GetTicks();
	char *l;

	if (dsock < 0) return false;
	while ((int)(SDL_GetTicks() - t0) < timeout_ms) {
		l = dline(timeout_ms - (int)(SDL_GetTicks() - t0));
		if (!l) break;
		/* The path, not just the word: a manual Save a moment earlier left
		 * its own SAVED unread on this socket. */
		if (strncmp(l, "SAVED\tpath=", 11) == 0 && !strcmp(l + 11, path))
			return true;
		if (strncmp(l, "ERROR\t", 6) == 0) return false;
		if (strncmp(l, "DISPLAY\t", 8) == 0) d_note_display(l);
		else if (strncmp(l, "LEVEL\t", 6) == 0) d_note_level(l);
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

/* A mid-game tap asked Diatom to PAUSE so the device can sleep; the PAUSED
 * it answers with is that, not the menu. */
static bool d_sleep_asked;

bool plat_resident_sleep_asked(void)
{
	bool was = d_sleep_asked;
	d_sleep_asked = false;
	return was;
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
			else if (strncmp(l, "PAUSED", 6) == 0) {
				/* A sleep's PAUSE overtaken by a hold's STOP: the game is
				 * ending, and its EXIT is what to wait for. */
				if (d_sleep_asked && sent_stop) { d_sleep_asked = false; continue; }
				return RES_PAUSED;
			}
			else if (strncmp(l, "PREVIEW\tpath=", 13) == 0)
				snprintf(d_preview, sizeof d_preview, "%s", l + 13);
			else if (strncmp(l, "LEVEL\t", 6) == 0) d_note_level(l);
			else if (strncmp(l, "AUDIO\t", 6) == 0) d_note_audio(l);
			else if (strncmp(l, "DISPLAY\t", 8) == 0) d_note_display(l);
			else if (strncmp(l, "CHEEVO\t", 7) == 0) d_note_cheevo(l);
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
				/* Asleep in-process, as minarch is: the game stops first.
				 * PAUSE, and the PAUSED that answers it comes back to
				 * launch() as a sleep rather than the menu - see
				 * plat_resident_sleep_asked. */
				if (pa == PWR_SLEEP) {
					if (!d_sleep_asked) { d_sleep_asked = true; dsend("PAUSE"); }
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

int plat_resident_wait(void)
{
	int r;

	screen_yield(true);
	r = diatom_wait();
	screen_yield(false);

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

/* The device file's ladders are the header's extents - platform_brick.c
 * asserts it - so the shared half can use the published ones. */
#define VOL_MAX    PLAT_VOL_MAX
#define BRIGHT_MAX PLAT_BRIGHT_MAX

void plat_screen(bool on)
{
	if (on) apply_brightness(cur_bright);
	else    backlight_off();
}

/* ---- power: sleep and the button. The Brick's, shared unchanged - only
 * plat_sleep() and plat_sleep_supported() are the device file's. */

#ifdef __linux__
/* Blocks up to ms for a POWER event with this value - 1 a press, 0 a
 * release - reading whatever else is queued on the way.
 *
 * Light sleep waits for the RELEASE, as NextUI's PLAT_shouldWake wakes on
 * SDL_KEYUP of POWER: waking on the press would leave its release for the
 * tap dispatcher to read as a fresh tap, and put the device straight back to
 * sleep. plat_sleep's retries look for the PRESS, and its release is drained
 * with the rest when it returns. */
bool power_key_within(int ms, int value)
{
	struct pollfd pfd;
	struct input_event ev;
	bool hit = false;

	if (fd_power < 0) { usleep(ms * 1000); return false; }
	pfd.fd = fd_power;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, ms) <= 0) return false;
	while (read(fd_power, &ev, sizeof ev) == (ssize_t)sizeof ev)
		if (ev.type == EV_KEY && ev.code == KEY_POWER && ev.value == value)
			hit = true;
	return hit;
}
#endif

/* The one press either caller could currently be watching - a->in.down every
 * frame from the shelf/menu path, or a level tracked locally from raw evdev
 * once a tick during a game. Only one of those runs at a time, so one timer
 * is enough. 0 means no press is open right now. */
static unsigned pwr_since;

/* When the last light sleep ended - NextUI's pwr.resume_tick. A power press
 * that STARTS within a second of it is the tail of the wake, not a request,
 * and plat_power_tap_or_hold drops it whole (api.c's "ignoring spurious
 * power button press (just resumed)"). 0 until the first sleep. */
static unsigned resume_at;


/* NextUI's PWR_sleep, whole: PWR_enterSleep, PWR_waitForWake, PWR_exitSleep
 * (api.c:4262-4380), for every way into sleep - a tap, Auto Sleep's idle,
 * the game menu's Sleep row. The CPU stays awake; the screen is off and the
 * sound muted, neither persisted (NextUI's SetRawVolume, not SetVolume - a
 * crash asleep must not boot dark and silent).
 *
 * Left unwoken for the Suspend Timeout it escalates into real suspend,
 * plat_sleep(), and returns once that resumes. Charging, or a computer
 * attached, puts the escalation off a minute at a time instead - suspending
 * on external power hangs this kernel (TortOS-2pv), which is why NextUI
 * never does.
 * Returns false only when escalation found no suspend to go to - unsupported,
 * or every attempt failed - which NextUI answers with PWR_powerOff; the
 * caller does, since powering off is the launcher's (main.c's power_off).
 *
 * What NextUI's enter/exit also do and why none of it is here: the LED
 * sleep profile is the player's LED setup set to breathe, and TortOS keeps
 * every LED off, so it stays off; keymon/batmon/audiomon are NextUI daemons
 * TortOS has no counterpart of; the status-poll frequency drop is automatic,
 * the launcher's loop being blocked right here; audio the launcher owns
 * (the music player) is paused by the caller, main.c's sleep_cycle; the
 * haptic pulse is gated on a setting NextUI ships off and TortOS lacks
 * (TortOS-1v7.1.2.11). */
bool plat_light_sleep(unsigned waited_ms)
{
	bool awake = true;
#ifdef __linux__
	int saved_vol = cur_vol;
	int timeout_ms = plat_suspend_timeout_secs() * 1000;
	unsigned since;

	plat_input_flush();                   /* PAD_reset */
	backlight_off();
	if (mixer_fd >= 0) apply_volume(0);
	sync();

	since = plat_now_ms() - waited_ms;    /* already dark that long */
	for (;;) {
		bool charging = false;

		if (power_key_within(200, 0)) break;
		/* Signed: "a minute from now" makes since run ahead of now. */
		if ((int)(plat_now_ms() - since) < timeout_ms) continue;
		plat_battery(NULL, &charging);
		if (charging || plat_usb_host()) {
			since += 60000;               /* check again in a minute */
			continue;
		}
		awake = plat_sleep();
		break;
	}

	apply_brightness(cur_bright);
	if (mixer_fd >= 0 && saved_vol >= 0) apply_volume(saved_vol);
	sync();
	plat_input_flush();                   /* PAD_reset, and whatever was
	                                       * pressed in the dark */
#else
	(void)waited_ms;
#endif
	pwr_since = 0;
	resume_at = plat_now_ms();
	if (!resume_at) resume_at = 1;
	return awake;
}

bool plat_usb_host(void)
{
	static int was = -1;
	glob_t g;
	size_t i;
	bool host = false;

	/* NextUI's PLAT_isUSBConnected (tg5040): the UDC says "configured" once
	 * a host has enumerated the device, which a wall charger never does. */
	if (glob("/sys/class/udc/*/state", 0, NULL, &g) != 0) return false;
	for (i = 0; i < g.gl_pathc && !host; i++) {
		char st[32] = "";
		FILE *f = fopen(g.gl_pathv[i], "r");
		if (!f) continue;
		if (fgets(st, sizeof st, f)) host = !strncmp(st, "configured", 10);
		fclose(f);
	}
	globfree(&g);
	if (host != was) {
		fprintf(stderr, "power: usb host %s\n", host ? "attached" : "detached");
		was = host;
	}
	return host;
}

int plat_suspend_timeout_secs(void)
{
	int v = db_get_int(db_dev(), "suspendtimeout", 30);
	return v > 0 ? v : 30;          /* NextUI's default; 0 is not a choice */
}

#define POWER_HOLD_MS 400u

pwr_action plat_power_tap_or_hold(bool down)
{
	static bool spurious;             /* this press began just after a wake */
	unsigned now = plat_now_ms();

	/* NextUI's resume_tick check: a press that starts inside a second of
	 * waking is dropped whole - neither its hold nor its release counts. */
	if (down && !pwr_since && !spurious && resume_at &&
	    now - resume_at < 1000) {
		spurious = true;
		fprintf(stderr, "power: press ignored, %ums after a wake\n",
		        now - resume_at);
	}
	if (spurious) {
		if (!down) spurious = false;
		return PWR_NONE;
	}
	if (down) {
		if (!pwr_since) pwr_since = now ? now : 1;
		if (now - pwr_since >= POWER_HOLD_MS) {
			pwr_since = 0;          /* fire once per press */
			fprintf(stderr, "power: hold -> power off\n");
			return PWR_POWEROFF;
		}
		return PWR_NONE;
	}
	if (!pwr_since) return PWR_NONE;   /* not pressed, nothing just ended */
	/* One line per decision, so an unexpected power-off leaves the press
	 * that caused it in the log (TortOS-1v7.1.2.7: one did, and didn't). */
	fprintf(stderr, "power: tap (%ums)\n", now - pwr_since);
	pwr_since = 0;
	/* Not gated on plat_sleep_supported(): NextUI's tap always enters light
	 * sleep, and suspend support only matters at its escalation. */
	return db_get_int(db_dev(), "power.tap", 1) ? PWR_SLEEP : PWR_POWEROFF;
}


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

/* ---- battery ---- */

/* A power_supply node's gauge, which each device file points at. Percent is the
 * kernel's, so a bigger cell needs nothing here: it only means more minutes
 * per percent. */
bool battery_read(const char *dir, int *pct, bool *charging)
{
	const char *fake = getenv("TORTOS_FAKE_BATT");
	if (fake && *fake) {
		if (pct) *pct = atoi(fake);
		if (charging) *charging = false;
		return true;
	}
#ifdef __linux__
	char path[128];
	snprintf(path, sizeof path, "%s/capacity", dir);
	FILE *f = fopen(path, "r");
	if (!f) return false;
	int v = -1;
	if (fscanf(f, "%d", &v) != 1) v = -1;
	fclose(f);
	if (v < 0) return false;
	if (pct) *pct = v;
	if (charging) {
		*charging = false;
		snprintf(path, sizeof path, "%s/status", dir);
		FILE *s = fopen(path, "r");
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
	(void)dir;
	(void)pct;
	(void)charging;
	return false;
#endif
}
