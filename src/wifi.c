/* SPDX-License-Identifier: MIT */
/* WiFi over the stock wpa_supplicant. See wifi.h for the position: none of
 * this is TortOS's own networking, it is a client of firmware that already
 * works.
 *
 * Everything runs through fork/execv rather than popen, and that is not
 * fastidiousness. An SSID is 32 arbitrary octets chosen by whoever owns the
 * access point, it arrives here from the air, and this process is root. A
 * network named `"; rm -rf / #` handed to a shell is a remote command
 * execution from anyone standing near the device. execv passes arguments as
 * a vector and never parses them, which removes the whole class rather than
 * escaping around it -- and it fixes the ordinary bugs too, since SSIDs with
 * spaces and quotes in them are common and legal.
 *
 * The passphrase still reaches wpa_cli as an argument, so it is briefly
 * visible in /proc/<pid>/cmdline. Closing that means speaking the control
 * protocol over its unix socket directly instead of shelling out at all.
 * Worth doing if this device ever runs anything untrusted; today the only
 * processes on it are the launcher, the emulator and the stock firmware, and
 * anything that could read that cmdline is already root.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wifi.h"

#if defined(__linux__)

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

/* Run a command, capture stdout, return its exit status (-1 if it could not
 * be run at all). `out` is always NUL-terminated. */
static int run(char *const argv[], char *out, size_t cap)
{
	int fd[2], status = -1;
	pid_t pid;
	size_t used = 0;

	if (out && cap) out[0] = '\0';
	if (pipe(fd) < 0) return -1;

	pid = fork();
	if (pid < 0) { close(fd[0]); close(fd[1]); return -1; }
	if (pid == 0) {
		close(fd[0]);
		dup2(fd[1], STDOUT_FILENO);
		dup2(fd[1], STDERR_FILENO);
		close(fd[1]);
		execv(argv[0], argv);
		_exit(127);
	}
	close(fd[1]);
	for (;;) {
		ssize_t n;
		char scratch[256];
		char *dst = (out && used + 1 < cap) ? out + used : scratch;
		size_t room = (out && used + 1 < cap) ? cap - used - 1 : sizeof scratch;
		n = read(fd[0], dst, room);
		if (n <= 0) break;
		if (dst != scratch) used += (size_t)n;
	}
	if (out && cap) out[used] = '\0';
	close(fd[0]);
	if (waitpid(pid, &status, 0) < 0) return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* The TrimUI radio. */
#if !defined(PLATFORM_GKD)

#define WPA_CLI  "/usr/sbin/wpa_cli"
#define WPA_SOCK "/etc/wifi/sockets"
#define WLAN     "wlan0"

/* wpa_cli with the socket and interface always supplied, because forgetting
 * either makes it talk to a default path that does not exist here and fail in
 * a way that looks like "no wifi" rather than "wrong arguments". */
static int wpa(char *out, size_t cap, const char *a, const char *b,
               const char *c, const char *d)
{
	char *argv[10];
	int n = 0;

	argv[n++] = (char *)WPA_CLI;
	argv[n++] = (char *)"-p";
	argv[n++] = (char *)WPA_SOCK;
	argv[n++] = (char *)"-i";
	argv[n++] = (char *)WLAN;
	if (a) argv[n++] = (char *)a;
	if (b) argv[n++] = (char *)b;
	if (c) argv[n++] = (char *)c;
	if (d) argv[n++] = (char *)d;
	argv[n] = NULL;
	return run(argv, out, cap);
}

static bool wpa_ok(const char *a, const char *b, const char *c, const char *d)
{
	char out[128];
	return wpa(out, sizeof out, a, b, c, d) == 0 && strncmp(out, "OK", 2) == 0;
}

/* One value out of `key=value` output. */
static bool kv(const char *text, const char *key, char *out, int cap)
{
	size_t klen = strlen(key);
	const char *p = text;

	while (p && *p) {
		if (!strncmp(p, key, klen) && p[klen] == '=') {
			const char *v = p + klen + 1;
			const char *e = strchr(v, '\n');
			int n = e ? (int)(e - v) : (int)strlen(v);
			if (n >= cap) n = cap - 1;
			memcpy(out, v, (size_t)n);
			out[n] = '\0';
			return true;
		}
		p = strchr(p, '\n');
		if (p) p++;
	}
	return false;
}

bool wifi_up(void)
{
	char out[128];
	int i;

	if (wpa(out, sizeof out, "ping", NULL, NULL, NULL) == 0 &&
	    strstr(out, "PONG")) return true;

	{
		char *argv[] = { (char *)"/etc/init.d/wpa_supplicant",
		                 (char *)"start", NULL };
		run(argv, NULL, 0);
	}
	/* The stock start_service retries `ifconfig wlan0 up` five times with
	 * usleep 500000 between, so the socket is not there the instant this
	 * returns. Wait for the socket rather than for a guessed duration. */
	for (i = 0; i < 20; i++) {
		sleep(1);
		if (wpa(out, sizeof out, "ping", NULL, NULL, NULL) == 0 &&
		    strstr(out, "PONG")) return true;
	}
	return false;
}

void wifi_down(void)
{
	char *argv[] = { (char *)"/etc/init.d/wpa_supplicant",
	                 (char *)"stop", NULL };
	run(argv, NULL, 0);
}

/* Saved networks, so the list can say which ones are already known. */
static int known_ssids(char list[][WIFI_SSID_MAX], int max)
{
	char out[4096];
	char *line, *save;
	int n = 0;

	if (wpa(out, sizeof out, "list_networks", NULL, NULL, NULL) != 0) return 0;
	for (line = strtok_r(out, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char *tab = strchr(line, '\t');
		char *end;
		if (!tab) continue;                       /* the header has no tabs */
		if (!strncmp(line, "network id", 10)) continue;
		end = strchr(++tab, '\t');
		if (end) *end = '\0';
		if (n < max) snprintf(list[n++], WIFI_SSID_MAX, "%s", tab);
	}
	return n;
}

/* Parse a scan_results block. Split out so the blocking and non-blocking
 * entry points share one parser rather than drifting apart. Destroys `buf`. */
static int parse_results(char *buf, wifi_net *out, int max)
{
	char known[WIFI_MAX_NETS][WIFI_SSID_MAX];
	char *line, *save;
	int n = 0, nknown, i;

	nknown = known_ssids(known, WIFI_MAX_NETS);

	for (line = strtok_r(buf, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char *f[5], *p = line;
		int col = 0, sig;
		wifi_net e;

		if (!strncmp(line, "bssid", 5)) continue; /* header */
		/* bssid \t freq \t signal \t flags \t ssid, and the ssid is last
		 * because it may itself contain anything except a tab. */
		while (col < 5) {
			f[col++] = p;
			p = strchr(p, '\t');
			if (!p) break;
			*p++ = '\0';
		}
		if (col < 5) continue;                    /* hidden: no ssid column */
		if (!f[4][0]) continue;

		memset(&e, 0, sizeof e);
		snprintf(e.ssid, sizeof e.ssid, "%s", f[4]);
		sig = atoi(f[2]);
		e.signal = sig;
		e.secured = strstr(f[3], "WPA") || strstr(f[3], "WEP");
		for (i = 0; i < nknown; i++)
			if (!strcmp(known[i], e.ssid)) { e.known = true; break; }

		/* One row per SSID. An access point with two radios answers twice and
		 * a mesh answers once per node; showing the same name three times
		 * would be a list of hardware, not a list of networks. Strongest
		 * wins, because that is the one it will actually associate with. */
		for (i = 0; i < n; i++)
			if (!strcmp(out[i].ssid, e.ssid)) break;
		if (i < n) {
			if (e.signal > out[i].signal) out[i].signal = e.signal;
			continue;
		}
		if (n < max) out[n++] = e;
	}

	/* Strongest first. n is at most WIFI_MAX_NETS, so an insertion sort is
	 * the right amount of machinery. */
	for (i = 1; i < n; i++) {
		wifi_net t = out[i];
		int j = i - 1;
		while (j >= 0 && out[j].signal < t.signal) { out[j + 1] = out[j]; j--; }
		out[j + 1] = t;
	}
	return n;
}

/* The networks the supplicant already holds, with no scan and no radio time.
 * Instant, so a screen has something to show while a scan runs. Signal is 0 and
 * meaningless until the scan lands - a saved network is not necessarily in
 * range, and this deliberately does not pretend to know. */
int wifi_known(wifi_net *out, int max)
{
	char list[WIFI_MAX_NETS][WIFI_SSID_MAX];
	int n, i, k = 0;

	n = known_ssids(list, WIFI_MAX_NETS);
	for (i = 0; i < n && k < max; i++) {
		memset(&out[k], 0, sizeof out[k]);
		/* Bounded read, not just a bounded write. known_ssids terminates
		 * every row, but the compiler cannot see that and assumes the copy
		 * could run the whole 24 x 33 array. */
		snprintf(out[k].ssid, sizeof out[k].ssid, "%.*s",
		         (int)(WIFI_SSID_MAX - 1), list[i]);
		out[k].known = true;
		out[k].secured = true;    /* a saved network needed a key, or did not */
		k++;
	}
	return k;
}

/* A scan that does not block the caller.
 *
 * The settling rule is the same one wifi_scan has always used and it is not
 * negotiable: results fill in over several seconds and the WEAK entries arrive
 * last. Measured 2026-08-29, a flat 3-second wait returned one network where
 * the radio could see two, and the one it dropped was the weaker of the pair -
 * exactly the network somebody is squinting at the list for. Two identical
 * counts in a row means it has finished; a cap stops a radio that never settles
 * from scanning forever.
 *
 * What changed is only WHO waits. This holds the same state across calls and
 * does at most one scan_results per second, so a screen can poll it from the
 * frame it is already drawing instead of standing still for nine. */
static char g_scan[8192];
static int  g_scan_last, g_scan_stable, g_scan_ticks;
static bool g_scanning;
static time_t g_scan_next;

bool wifi_scan_start(void)
{
	char buf[512];

	if (wpa(buf, sizeof buf, "ping", NULL, NULL, NULL) != 0 ||
	    !strstr(buf, "PONG")) return false;
	wpa(buf, sizeof buf, "scan", NULL, NULL, NULL);
	g_scan[0] = '\0';
	g_scan_last = -1;
	g_scan_stable = 0;
	g_scan_ticks = 0;
	g_scan_next = time(NULL) + 1;
	g_scanning = true;
	return true;
}

/* 0 while running, 1 when results are ready to take, -1 if no scan is running.
 * Safe to call every frame: it does nothing until its next second is due. */
int wifi_scan_poll(void)
{
	const char *q;
	int c = 0;

	if (!g_scanning) return -1;
	if (time(NULL) < g_scan_next) return 0;
	g_scan_next = time(NULL) + 1;

	if (++g_scan_ticks >= 9) { g_scanning = false; return 1; }  /* cap */
	if (wpa(g_scan, sizeof g_scan, "scan_results", NULL, NULL, NULL) != 0)
		return 0;

	for (q = g_scan; *q; q++) if (*q == '\n') c++;
	if (c > 0 && c == g_scan_last) {
		if (++g_scan_stable >= 2) { g_scanning = false; return 1; }
	} else {
		g_scan_stable = 0;
		g_scan_last = c;
	}
	return 0;
}

int wifi_scan_take(wifi_net *out, int max)
{
	if (g_scan_last <= 0) return 0;           /* radio up, nothing heard */
	return parse_results(g_scan, out, max);
}

/* The blocking form, kept for the --wifi diagnostic where standing still is
 * exactly what is wanted. Built on the same three calls so there is one scan
 * implementation rather than two that can disagree. */
int wifi_scan(wifi_net *out, int max)
{
	if (!wifi_scan_start()) return -1;
	while (wifi_scan_poll() == 0) sleep(1);
	return wifi_scan_take(out, max);
}

/* Ask for a lease. The vendor's own invocation, from /etc/wifi/udhcpc_wlan0.
 *
 * Lifted out of wifi_connect because associating is not the only way to end up
 * associated: wpa_supplicant rejoins a saved network by itself the moment it
 * starts, and nothing was fetching an address for that.
 *
 * Fire and forget, NOT run(). run() waits, and this can take `-t 5 -T 7` -
 * thirty-five seconds - before -b gives up and backgrounds itself. wifi_status
 * is polled from the menu, so waiting there would stall the shelf for half a
 * minute on a network that is simply slow to answer. Double-forked so the
 * child is reparented to init and there is no zombie to reap, because nothing
 * here is going to come back and wait for it. */
static bool g_dhcp_asked;   /* a lease has been requested for this join */

static void dhcp_start(void)
{
	pid_t pid = fork();

	if (pid < 0) return;
	if (pid == 0) {
		if (fork() == 0) {
			char *argv[] = { (char *)"/sbin/udhcpc", (char *)"-i", (char *)WLAN,
			                 (char *)"-S", (char *)"-t", (char *)"5",
			                 (char *)"-T", (char *)"7", (char *)"-b",
			                 (char *)"-q", NULL };
			int null = open("/dev/null", O_RDWR);

			if (null >= 0) { dup2(null, 1); dup2(null, 2); }
			execv(argv[0], argv);
			_exit(127);
		}
		_exit(0);
	}
	waitpid(pid, NULL, 0);          /* the middle one, which exits at once */
}

wifi_state wifi_status(char *ssid, int ssid_cap, char *ip, int ip_cap)
{
	char out[2048], state[64];

	if (ssid && ssid_cap) ssid[0] = '\0';
	if (ip && ip_cap) ip[0] = '\0';

	if (wpa(out, sizeof out, "status", NULL, NULL, NULL) != 0) return WIFI_OFF;
	if (!kv(out, "wpa_state", state, sizeof state)) return WIFI_OFF;

	if (ssid && ssid_cap) kv(out, "ssid", ssid, ssid_cap);
	if (ip && ip_cap) kv(out, "ip_address", ip, ip_cap);

	/* COMPLETED is associated, which is not the same as usable.
	 *
	 * wpa_supplicant rejoins a saved network on its own at startup, so the
	 * state reaches COMPLETED with no lease, no route and no udhcpc ever
	 * having run - and reporting that as CONNECTED put the SSID in the menu
	 * and lit up every row that needs the network. Over The Hare then opened
	 * and said "Wi-Fi went away", which is the one thing that had not
	 * happened. An association with no address is still connecting.
	 *
	 * And it will stay that way unless something asks: the only udhcpc call
	 * used to be inside wifi_connect, the path taken by choosing an SSID by
	 * hand. Asking here is what makes an auto-rejoin finish by itself. */
	if (!strcmp(state, "COMPLETED")) {
		char addr[64];

		if (kv(out, "ip_address", addr, sizeof addr) && addr[0]) {
			g_dhcp_asked = false;  /* armed again for the next association */
			return WIFI_CONNECTED;
		}
		/* Once per association, not once per poll: this is called from
		 * menu_build, and a udhcpc per frame would be a fork bomb wearing a
		 * status query's clothes. */
		if (!g_dhcp_asked) { g_dhcp_asked = true; dhcp_start(); }
		return WIFI_CONNECTING;
	}
	g_dhcp_asked = false;              /* not associated: arm for the next one */
	if (!strcmp(state, "SCANNING") || !strcmp(state, "DISCONNECTED") ||
	    !strcmp(state, "INACTIVE"))   return WIFI_IDLE;
	return WIFI_CONNECTING;
}

/* The id wpa_supplicant already has for this SSID, or -1. */
static int saved_id(const char *ssid, char *out_id, size_t cap)
{
	char out[4096];
	char *line, *save;

	if (wpa(out, sizeof out, "list_networks", NULL, NULL, NULL) != 0) return -1;
	for (line = strtok_r(out, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char *tab = strchr(line, '\t'), *end;
		if (!tab || !strncmp(line, "network id", 10)) continue;
		*tab = '\0';
		end = strchr(++tab, '\t');
		if (end) *end = '\0';
		if (strcmp(tab, ssid)) continue;
		snprintf(out_id, cap, "%.15s", line);
		return 0;
	}
	return -1;
}

bool wifi_connect(const char *ssid, const char *psk)
{
	char out[256], id[16], quoted[WIFI_SSID_MAX + 4], qpsk[80];
	bool reused = false;
	int i;

	if (!ssid || !*ssid) return false;
	if (!wifi_up()) return false;

	/* An already-saved network is joined by its id. Creating a second entry
	 * for it and handing that entry no passphrase produced a key_mgmt=NONE
	 * config - an OPEN network - pointed at a WPA2 access point, which cannot
	 * associate. That is what "connect to a saved network" did until
	 * 2026-08-29, and it failed every time. */
	if (psk && *psk) {
		/* A new passphrase for a known SSID replaces the old entry rather
		 * than sitting beside it, or the stale one competes to associate. */
		if (saved_id(ssid, id, sizeof id) == 0)
			wpa(out, sizeof out, "remove_network", id, NULL, NULL);
	} else if (saved_id(ssid, id, sizeof id) == 0) {
		reused = true;
	}

	if (!reused && wpa(out, sizeof out, "add_network", NULL, NULL, NULL) != 0)
		return false;
	if (!reused)
	{	/* the id is the last line, and there may be a status line above it */
		char *nl = strrchr(out, '\n');
		char *v = out;
		while (nl && (nl == out || nl[1] == '\0')) { *nl = '\0'; nl = strrchr(out, '\n'); }
		if (nl) v = nl + 1;
		snprintf(id, sizeof id, "%.15s", v);
	}
	if (!id[0] || id[0] < '0' || id[0] > '9') return false;

	/* wpa_supplicant wants the value quoted INSIDE the argument: the quotes
	 * are part of the config syntax, not shell quoting, and execv would never
	 * have stripped them anyway. */
	if (!reused) {
		snprintf(quoted, sizeof quoted, "\"%s\"", ssid);
		if (!wpa_ok("set_network", id, "ssid", quoted)) goto fail;

		if (psk && *psk) {
			snprintf(qpsk, sizeof qpsk, "\"%s\"", psk);
			if (!wpa_ok("set_network", id, "psk", qpsk)) goto fail;
		} else {
			/* Genuinely open, and only because the scan said so. */
			if (!wpa_ok("set_network", id, "key_mgmt", "NONE")) goto fail;
		}
	}

	if (!wpa_ok("enable_network", id, NULL, NULL)) goto fail;
	if (!wpa_ok("select_network", id, NULL, NULL)) goto fail;

	/* Association is not instant and a wrong passphrase looks exactly like a
	 * slow one until the supplicant gives up, so this waits rather than
	 * reporting a success the moment the command was accepted. */
	for (i = 0; i < 20; i++) {
		sleep(1);
		if (wifi_status(NULL, 0, NULL, 0) == WIFI_CONNECTED) break;
	}
	if (wifi_status(NULL, 0, NULL, 0) != WIFI_CONNECTED) goto fail;

	/* Only now is the credential worth keeping. Saving before association
	 * would fill the config with passwords that never worked. */
	wpa(out, sizeof out, "save_config", NULL, NULL, NULL);

	dhcp_start();
	return true;

fail:
	/* select_network disables every OTHER network, so a failed attempt used
	 * to leave the whole saved list switched off - one bad try and the device
	 * never rejoined anything again, silently, with wpa_state sitting at
	 * INACTIVE and the saved entry marked [DISABLED]. Put them back. */
	if (!reused) wpa(out, sizeof out, "remove_network", id, NULL, NULL);
	wpa(out, sizeof out, "enable_network", "all", NULL, NULL);
	return false;
}

bool wifi_forget(const char *ssid)
{
	char out[4096];
	char *line, *save;
	bool hit = false;

	if (wpa(out, sizeof out, "list_networks", NULL, NULL, NULL) != 0) return false;
	for (line = strtok_r(out, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char *tab = strchr(line, '\t');
		char *end, id[16];
		if (!tab || !strncmp(line, "network id", 10)) continue;
		*tab = '\0';
		snprintf(id, sizeof id, "%.15s", line);
		end = strchr(++tab, '\t');
		if (end) *end = '\0';
		if (strcmp(tab, ssid)) continue;
		if (wpa_ok("remove_network", id, NULL, NULL)) hit = true;
	}
	if (hit) {
		char tmp[128];
		wpa(tmp, sizeof tmp, "save_config", NULL, NULL, NULL);
	}
	return hit;
}

#else  /* PLATFORM_GKD: ROCKNIX's ConnMan, driven the way ROCKNIX drives it */

/* ROCKNIX keeps the network in system.cfg, not in ConnMan. At every boot, and
 * after every resume, its scripts run `wifictl enable|disable` from
 * wifi.enabled, and `wifictl enable` rewrites ConnMan's profile from wifi.ssid
 * and wifi.key and deletes every other network ConnMan had saved. Anything
 * set in ConnMan alone is undone by the next wake. So this writes those keys
 * and calls wifictl, exactly as the stock menu does, and asks ConnMan only
 * what it can see. The cost is ROCKNIX's: one remembered network.
 *
 * The keys are written here in C rather than through set_setting, a shell
 * function that pushes its value through sed. An SSID comes off the air, and
 * the file header says what that means for a shell. */
#include <signal.h>

#include "atomic.h"

#define CONNMANCTL "/usr/bin/connmanctl"
#define WIFICTL    "/usr/bin/wifictl"
#define CFG        "/storage/.config/system/configs/system.cfg"
#define CFG_STOCK  "/usr/config/system/configs/system.cfg"   /* before the first write */
#define CM_PROFILE "/storage/.cache/connman/wifi.config"
#define SYSTEMCTL  "/usr/bin/systemctl"
#define SSHD_MARK  "/storage/.cache/services/sshd.conf"  /* sshd.service's condition */
#define SAVES_DIR  "/storage/games-external/Saves"

static int cm(char *out, size_t cap, const char *a, const char *b, const char *c)
{
	char *argv[] = { (char *)CONNMANCTL, (char *)a, (char *)b, (char *)c, NULL };
	return run(argv, out, cap);
}

static void wifictl(const char *verb)
{
	char *argv[] = { (char *)WIFICTL, (char *)verb, NULL };
	run(argv, NULL, 0);
}

/* The first `key=` line, as ROCKNIX's own get_setting reads it. */
static bool cfg_get(const char *key, char *out, int cap)
{
	char line[512];
	size_t klen = strlen(key);
	FILE *f = fopen(CFG, "r");
	bool hit = false;

	out[0] = '\0';
	if (!f) return false;
	while (fgets(line, sizeof line, f)) {
		if (strncmp(line, key, klen) || line[klen] != '=') continue;
		line[strcspn(line, "\r\n")] = '\0';
		snprintf(out, (size_t)cap, "%s", line + klen + 1);
		hit = true;
		break;
	}
	fclose(f);
	return hit;
}

/* Replace the first `key=` line, or append one. A newline in the value would
 * forge a second key, so it is refused rather than escaped: the file has no
 * escaping. */
static bool cfg_set(const char *key, const char *val)
{
	char line[512];
	size_t klen = strlen(key);
	bool done = false;
	FILE *in, *out;

	if (strpbrk(val, "\r\n")) return false;
	in = fopen(CFG, "r");
	if (!in) in = fopen(CFG_STOCK, "r");
	out = atomic_open(CFG, 0644);
	if (!out) { if (in) fclose(in); return false; }
	while (in && fgets(line, sizeof line, in)) {
		if (!done && !strncmp(line, key, klen) && line[klen] == '=') {
			fprintf(out, "%s=%s\n", key, val);
			done = true;
		} else {
			fputs(line, out);
		}
	}
	if (in) fclose(in);
	if (!done) fprintf(out, "%s=%s\n", key, val);
	return atomic_commit(out, CFG);
}

/* The radio is on when ConnMan's wifi technology is powered. */
static bool powered(void)
{
	char out[2048];
	const char *p, *next;

	if (cm(out, sizeof out, "technologies", NULL, NULL) != 0) return false;
	p = strstr(out, "/net/connman/technology/wifi");
	if (!p) return false;
	next = strstr(p + 1, "/net/connman/");
	p = strstr(p, "Powered = True");
	return p && (!next || p < next);
}

/* One `connmanctl services` line: three state flags, a space, the name padded
 * to a column, then the service id. The name may hold spaces, the id never
 * does, so the id is the last word and the name is everything before it.
 * Destroys `line`. False for anything that is not a named wifi service. */
static bool cm_line(char *line, char *state, char **name, char **id)
{
	char *sp, *end;

	if (strlen(line) < 5) return false;
	sp = strrchr(line, ' ');
	if (!sp || strncmp(sp + 1, "wifi_", 5)) return false;
	*state = line[2];
	*id = sp + 1;
	for (end = sp; end > line + 4 && end[-1] == ' '; end--) ;
	*end = '\0';
	*name = line + 4;
	return (*name)[0] != '\0';              /* hidden: no name to show */
}

/* One field of `connmanctl services <id>`, up to the end of its line. */
static bool cm_field(const char *id, const char *field, char *out, int cap)
{
	char buf[2048];
	const char *p, *e;
	int n;

	if (cm(buf, sizeof buf, "services", id, NULL) != 0) return false;
	p = strstr(buf, field);
	if (!p) return false;
	p += strlen(field);
	e = strchr(p, '\n');
	n = e ? (int)(e - p) : (int)strlen(p);
	if (n >= cap) n = cap - 1;
	memcpy(out, p, (size_t)n);
	out[n] = '\0';
	return true;
}

bool wifi_up(void)
{
	int i;

	if (powered()) return true;
	wifictl("enable");                      /* also sets wifi.enabled=1 */
	for (i = 0; i < 20; i++) {
		if (powered()) return true;
		sleep(1);
	}
	return false;
}

void wifi_down(void)
{
	wifictl("disable");                     /* also sets wifi.enabled=0 */
}

int wifi_known(wifi_net *out, int max)
{
	char ssid[WIFI_SSID_MAX], key[80];

	if (max < 1 || !cfg_get("wifi.ssid", ssid, sizeof ssid) || !ssid[0]) return 0;
	cfg_get("wifi.key", key, sizeof key);
	memset(&out[0], 0, sizeof out[0]);
	snprintf(out[0].ssid, sizeof out[0].ssid, "%s", ssid);
	out[0].known = true;
	out[0].secured = key[0] != '\0';
	return 1;
}

/* `connmanctl scan wifi` returns when the scan is complete (about 2 s here),
 * so the scan runs as a child and the poll only asks whether it has exited.
 * The cap is for a scan that never answers. */
static pid_t  g_scan_pid;
static time_t g_scan_deadline;

bool wifi_scan_start(void)
{
	pid_t pid;

	if (g_scan_pid > 0) return true;        /* one already running */
	if (!powered()) return false;
	pid = fork();
	if (pid < 0) return false;
	if (pid == 0) {
		char *argv[] = { (char *)CONNMANCTL, (char *)"scan", (char *)"wifi", NULL };
		int null = open("/dev/null", O_RDWR);
		if (null >= 0) { dup2(null, 1); dup2(null, 2); }
		execv(argv[0], argv);
		_exit(127);
	}
	g_scan_pid = pid;
	g_scan_deadline = time(NULL) + 15;
	return true;
}

int wifi_scan_poll(void)
{
	if (g_scan_pid <= 0) return -1;
	if (waitpid(g_scan_pid, NULL, WNOHANG) == 0) {
		if (time(NULL) < g_scan_deadline) return 0;
		kill(g_scan_pid, SIGKILL);
		waitpid(g_scan_pid, NULL, 0);
	}
	g_scan_pid = 0;
	return 1;
}

int wifi_scan_take(wifi_net *out, int max)
{
	char buf[8192], saved[WIFI_SSID_MAX];
	char *line, *save;
	int n = 0, i;

	if (cm(buf, sizeof buf, "services", NULL, NULL) != 0) return 0;
	cfg_get("wifi.ssid", saved, sizeof saved);
	for (line = strtok_r(buf, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char state, *name, *id, str[16];
		wifi_net e;

		if (!cm_line(line, &state, &name, &id)) continue;
		memset(&e, 0, sizeof e);
		snprintf(e.ssid, sizeof e.ssid, "%s", name);
		/* ConnMan's Strength is 0-100, which is its own 2 x (dBm + 100). */
		e.signal = cm_field(id, "Strength = ", str, sizeof str)
		           ? atoi(str) / 2 - 100 : -100;
		e.secured = !strstr(id, "_none");
		e.known = saved[0] && !strcmp(saved, e.ssid);

		/* One row per SSID, strongest wins: see the TrimUI parse_results. */
		for (i = 0; i < n; i++)
			if (!strcmp(out[i].ssid, e.ssid)) break;
		if (i < n) {
			if (e.signal > out[i].signal) out[i].signal = e.signal;
			continue;
		}
		if (n < max) out[n++] = e;
	}
	for (i = 1; i < n; i++) {                 /* strongest first */
		wifi_net t = out[i];
		int j = i - 1;
		while (j >= 0 && out[j].signal < t.signal) { out[j + 1] = out[j]; j--; }
		out[j + 1] = t;
	}
	return n;
}

int wifi_scan(wifi_net *out, int max)
{
	if (!wifi_scan_start()) return -1;
	while (wifi_scan_poll() == 0) sleep(1);
	return wifi_scan_take(out, max);
}

/* ConnMan's state flag: O online and R ready have an address; a association
 * and c configuration are on the way. */
wifi_state wifi_status(char *ssid, int ssid_cap, char *ip, int ip_cap)
{
	char buf[8192];
	char *line, *save;
	wifi_state st = WIFI_IDLE;

	if (ssid && ssid_cap) ssid[0] = '\0';
	if (ip && ip_cap) ip[0] = '\0';
	if (!powered()) return WIFI_OFF;
	if (cm(buf, sizeof buf, "services", NULL, NULL) != 0) return WIFI_IDLE;
	for (line = strtok_r(buf, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char state, *name, *id;

		if (!cm_line(line, &state, &name, &id)) continue;
		if (state == 'O' || state == 'R') {
			if (ssid && ssid_cap) snprintf(ssid, (size_t)ssid_cap, "%s", name);
			if (ip && ip_cap) {
				char v4[256], *a;
				if (cm_field(id, "IPv4 = [ ", v4, sizeof v4) &&
				    (a = strstr(v4, "Address="))) {
					a += 8;
					a[strcspn(a, ", ]")] = '\0';
					snprintf(ip, (size_t)ip_cap, "%s", a);
				}
			}
			return WIFI_CONNECTED;
		}
		if (state == 'a' || state == 'c') st = WIFI_CONNECTING;
	}
	return st;
}

/* Joining is wifictl's: it writes ConnMan's profile from the keys and
 * restarts it. The keys are only kept if the join works; otherwise the old
 * ones go back and the old network is rejoined. A saved network chosen again
 * with no passphrase keeps the one it has. */
bool wifi_connect(const char *ssid, const char *psk)
{
	char old_ssid[WIFI_SSID_MAX], old_key[80], cur[WIFI_SSID_MAX];
	const char *key;
	int i;

	if (!ssid || !*ssid) return false;
	cfg_get("wifi.ssid", old_ssid, sizeof old_ssid);
	cfg_get("wifi.key", old_key, sizeof old_key);
	key = (psk && *psk) ? psk : !strcmp(old_ssid, ssid) ? old_key : "";
	if (!cfg_set("wifi.ssid", ssid) || !cfg_set("wifi.key", key)) goto fail;

	wifictl("enable");
	for (i = 0; i < 20; i++) {
		if (wifi_status(cur, sizeof cur, NULL, 0) == WIFI_CONNECTED &&
		    !strcmp(cur, ssid)) return true;
		sleep(1);
	}
fail:
	cfg_set("wifi.ssid", old_ssid);
	cfg_set("wifi.key", old_key);
	if (old_ssid[0]) wifictl("enable");
	else unlink(CM_PROFILE);                 /* nothing to go back to: see wifi_forget */
	return false;
}

/* Not through wifictl: with no SSID, its connect step matches whatever service
 * is listed first and could join a stranger's open network. Clearing the keys
 * and deleting the profile ConnMan was provisioned from drops the network and
 * leaves the radio on; the next boot's wifictl then writes an empty profile. */
bool wifi_forget(const char *ssid)
{
	char saved[WIFI_SSID_MAX];

	if (!cfg_get("wifi.ssid", saved, sizeof saved) || strcmp(saved, ssid))
		return false;
	if (!cfg_set("wifi.ssid", "") || !cfg_set("wifi.key", "")) return false;
	unlink(CM_PROFILE);
	return true;
}

static const char *const svc_key[WIFI_NSVC] = {
	"ssh.enabled", "samba.enabled", "syncthing.enabled"
};

bool wifi_svc_on(wifi_svc s)
{
	char v[8];
	return cfg_get(svc_key[s], v, sizeof v) && !strcmp(v, "1");
}

/* Syncthing shares Saves and nothing else (plorpos-gkd.42): its own "Default
 * Folder" (/storage/Sync) goes, and Saves comes in under a fixed id, so a
 * second switch-on finds it there and changes nothing. Which other devices get
 * it is chosen in Syncthing's web UI - the GKD cannot know them. Each step
 * runs only when needed, since a refused one shows as a warning in the web UI.
 * Automatic upgrades go off: the binary is in ROCKNIX's read-only image, so one
 * can only fail. (The web UI still offers one; only STNOUPGRADE in syncthing's
 * environment hides that.) All of this goes through the running instance, which
 * takes seconds to come up, so it waits for it in the background, detached
 * from the menu. */
#define ST_CLI "/usr/bin/syncthing cli --home=/storage/.config/syncthing config"
static void syncthing_share_saves(void)
{
	char *argv[] = { (char *)"/bin/sh", (char *)"-c", (char *)
		"( i=0; while [ $i -lt 30 ] && ! L=$(" ST_CLI " folders list); do i=$((i+1)); sleep 1; done; "
		"echo \"$L\" | grep -qx default && " ST_CLI " folders default delete; "
		"echo \"$L\" | grep -qx plorpos-saves || "
		ST_CLI " folders add --id plorpos-saves --label Saves --path " SAVES_DIR "; "
		ST_CLI " options auto-upgrade-intervalh set 0"
		" ) >/dev/null 2>&1 &", NULL };
	run(argv, NULL, 0);
}

/* The setting, then what ROCKNIX's 099-networkservices would do with it at the
 * next boot, done now. sshd.service only starts while its marker exists. */
bool wifi_svc_set(wifi_svc s, bool on)
{
	char *verb = on ? (char *)"start" : (char *)"stop";
	char *ssh[]   = { (char *)SYSTEMCTL, verb, (char *)"sshd", NULL };
	char *samba[] = { (char *)SYSTEMCTL, verb, (char *)"nmbd", (char *)"smbd", NULL };
	char *st[]    = { (char *)SYSTEMCTL, verb, (char *)"syncthing", NULL };

	if (!cfg_set(svc_key[s], on ? "1" : "0")) return false;
	switch (s) {
	case WIFI_SSH:
		if (on) {
			FILE *f = fopen(SSHD_MARK, "a");
			if (f) fclose(f);
		} else {
			unlink(SSHD_MARK);
		}
		return run(ssh, NULL, 0) == 0;
	case WIFI_SYNCTHING:
		if (run(st, NULL, 0) != 0) return false;
		if (on) syncthing_share_saves();
		return true;
	default:
		return run(samba, NULL, 0) == 0;
	}
}

#endif /* PLATFORM_GKD */

#else  /* host build: no radio, and the shelf still has to run */

bool wifi_up(void) { return false; }
void wifi_down(void) { }
int wifi_scan(wifi_net *out, int max) { (void)out; (void)max; return -1; }
int wifi_known(wifi_net *out, int max) { (void)out; (void)max; return 0; }
bool wifi_scan_start(void) { return false; }
int wifi_scan_poll(void) { return -1; }
int wifi_scan_take(wifi_net *out, int max) { (void)out; (void)max; return 0; }
bool wifi_connect(const char *ssid, const char *psk)
{
	(void)ssid; (void)psk; return false;
}
wifi_state wifi_status(char *ssid, int ssid_cap, char *ip, int ip_cap)
{
	if (ssid && ssid_cap) ssid[0] = '\0';
	if (ip && ip_cap) ip[0] = '\0';
	return WIFI_OFF;
}
bool wifi_forget(const char *ssid) { (void)ssid; return false; }

#endif

#if !defined(PLATFORM_GKD)   /* no services to switch: WIFI_SVC_ROWS is 0 */
bool wifi_svc_on(wifi_svc s) { (void)s; return false; }
bool wifi_svc_set(wifi_svc s, bool on) { (void)s; (void)on; return false; }
#endif
