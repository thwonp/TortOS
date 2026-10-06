/* SPDX-License-Identifier: MIT */
/* See bt.h. fork/execv rather than popen, for the reason wifi.c gives at
 * length: a device NAME is arbitrary bytes chosen by whoever owns the headset
 * and it arrives here over the air, into a process running as root. execv
 * passes a vector and never parses it, which removes the class rather than
 * escaping around it. A MAC is validated besides, because it is the one thing
 * here that is ever passed back out as an argument.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bt.h"

#define BLUETOOTHCTL "/usr/bin/bluetoothctl"

/* Where bluetoothd keeps its bonds, which is compiled into it: /etc/lib/bluetooth
 * on the Brick, /etc/bluetooth/keys on the Brick Pro (TortOS-pky.10). bt-alsa.sh
 * asks the binary once at boot and exports the answer; without it (a tool run
 * over adb) this is the Brick's path, as it always was. */
static const char *bonds(void)
{
	const char *b = getenv("TORTOS_BT_BONDS");
#if defined(PLATFORM_GKD)
	return b && *b ? b : "/storage/.cache/bluetooth";   /* ROCKNIX's build */
#else
	return b && *b ? b : "/etc/lib/bluetooth";
#endif
}

/* AA:BB:CC:DD:EE:FF and nothing else. Everything that reaches bluetoothctl as
 * an argument goes through this first: a name can be arbitrary, an address
 * cannot, and refusing what is not an address is cheaper than trusting the
 * source of one. */
/* The first 17 bytes only, so a line like "Device AA:.. Some Name" can be
 * validated where it sits without being copied out first. */
static bool mac_ok_prefix(const char *m)
{
	int i;

	if (!m) return false;
	for (i = 0; i < 17; i++) {
		if (!m[i]) return false;
		if (i % 3 == 2) { if (m[i] != ':') return false; continue; }
		if (!((m[i] >= '0' && m[i] <= '9') ||
		      (m[i] >= 'A' && m[i] <= 'F') ||
		      (m[i] >= 'a' && m[i] <= 'f'))) return false;
	}
	return true;
}

/* Exactly an address and nothing after it. Everything that reaches
 * bluetoothctl as an argument goes through this: a name can be arbitrary, an
 * address cannot, and refusing what is not one is cheaper than trusting where
 * it came from. */
bool bt_mac_valid(const char *m)
{
	return m && strlen(m) == 17 && mac_ok_prefix(m);
}

/* BlueZ prints an address as the name when it has none, in either punctuation.
 * Returns false when the "name" is just the address again. */
static bool name_is_address(const char *name, const char *mac)
{
	int i;

	if (!name || !*name) return false;
	for (i = 0; i < 17; i++) {
		char a = name[i], b = mac[i];
		if (!a) return true;                          /* shorter: a real name */
		if (a == '-' && b == ':') continue;           /* AA-BB-.. for AA:BB:.. */
		if (a >= 'a' && a <= 'z') a = (char)(a - 32);
		if (b >= 'a' && b <= 'z') b = (char)(b - 32);
		if (a != b) return true;
	}
	return name[17] != '\0';                          /* trailing text: a name */
}

/* "Device AA:BB:CC:DD:EE:FF Some Name" - one per line. */
int bt_parse_devices(const char *text, bt_device *out, int max)
{
	const char *p = text;
	int n = 0;

	while (p && *p && n < max) {
		const char *eol = strchr(p, '\n');
		size_t len = eol ? (size_t)(eol - p) : strlen(p);
		char line[256];

		snprintf(line, sizeof line, "%.*s", (int)(len < sizeof line ? len : sizeof line - 1), p);
		p = eol ? eol + 1 : NULL;

		if (strncmp(line, "Device ", 7)) continue;
		if (!mac_ok_prefix(line + 7)) continue;
		snprintf(out[n].mac, BT_MAC_MAX, "%.17s", line + 7);

		/* NAMED ONLY. A scan in an ordinary room finds a dozen BLE beacons,
		 * a television and somebody's phone, and BlueZ prints an address for
		 * every one it has no name for - so the list filled with rows nobody
		 * could identify or use, and the headset was somewhere off the bottom.
		 *
		 * "No name" has two spellings: the field is absent, or BlueZ has
		 * substituted the address with dashes. Both are skipped. A device
		 * that is BONDED is added back by bt_visible whatever its name, so
		 * one that was paired before it had a name can still be forgotten. */
		if (!line[24]) continue;
		if (!name_is_address(line + 25, out[n].mac)) continue;

		/* Truncated deliberately, and bounded so the device compiler can see
		 * it is: a headset may advertise a name far longer than a menu row. */
		snprintf(out[n].name, BT_NAME_MAX, "%.*s", BT_NAME_MAX - 1, line + 25);
		out[n].bonded = false;
		out[n].connected = false;
		n++;
	}
	return n;
}

void bt_label(const bt_device *d, char *out, size_t n)
{
	if (!out || !n) return;
	if (!d)           { out[0] = '\0'; return; }
	if (d->connected)   snprintf(out, n, "connected");
	else if (d->bonded) snprintf(out, n, "paired");
	else                snprintf(out, n, "in range");
}

/* "\t> ACL A8:F5:E1:4A:93:71 handle 128 state 1 lm MASTER AUTH ENCRYPT" */
int bt_mark_connected(const char *text, bt_device *list, int n)
{
	const char *p = text;
	int marked = 0, i;

	if (!text || !list) return 0;
	for (i = 0; i < n; i++) list[i].connected = false;

	while (p && *p) {
		const char *eol = strchr(p, '\n');
		size_t len = eol ? (size_t)(eol - p) : strlen(p);
		char line[256];
		const char *acl;

		snprintf(line, sizeof line, "%.*s",
		         (int)(len < sizeof line ? len : sizeof line - 1), p);
		p = eol ? eol + 1 : NULL;

		/* ACL only. A SCO link is the headset's microphone channel and says
		 * nothing about whether audio is going out to it. */
		if (!(acl = strstr(line, "ACL "))) continue;
		if (!mac_ok_prefix(acl + 4)) continue;

		/* AND ESTABLISHED. A link being set up is listed too, and reads
		 * exactly like a working one to anything that only looks for the
		 * address:
		 *
		 *   working    > ACL <mac> handle 128 state 1 lm MASTER AUTH ENCRYPT
		 *   half-open  < ACL <mac> handle 0   state 5 lm MASTER
		 *
		 * Seen on the device 2026-09-06 with the headset still held by a
		 * phone: bluetoothctl said Connected: no while this said yes. Which
		 * is the same wrong-status bug as the sink file and the per-cursor
		 * query, in a third disguise - so state is checked rather than
		 * assumed. */
		{
			const char *st = strstr(line, "state ");
			/* The NUMBER, not a substring. Matching "state 1 " with a
			 * trailing space looked fine and broke on a line where state was
			 * the last field - which the check caught immediately. */
			if (!st || atoi(st + 6) != 1) continue;
		}
		for (i = 0; i < n; i++)
			if (!strncasecmp(list[i].mac, acl + 4, 17)) {
				list[i].connected = true;
				marked++;
				break;
			}
	}
	return marked;
}

/* Outside the __linux__ block on purpose: this is dirent, stat and unlink,
 * which are POSIX, and it is the one function here that DELETES FILES. Inside
 * it, the check ran against a stub returning 0 and passed while proving
 * nothing - which is what happened the first time it was written. */
int bt_sweep_cache(const char *root)
{
	DIR *ad;
	struct dirent *a;
	int gone = 0;

	if (!root) root = bonds();
	if (!(ad = opendir(root))) return 0;
	while ((a = readdir(ad))) {
		char cdir[600];
		DIR *cd;
		struct dirent *c;

		if (a->d_name[0] == '.') continue;
		snprintf(cdir, sizeof cdir, "%s/%s/cache", root, a->d_name);
		if (!(cd = opendir(cdir))) continue;
		while ((c = readdir(cd))) {
			char bond[700], path[700];
			struct stat sb;

			if (!bt_mac_valid(c->d_name)) continue;
			/* The bond directory sits beside the cache one, named the same
			 * way. Present means the player chose this device and the cache
			 * is doing its job; absent means a scan put it there. */
			snprintf(bond, sizeof bond, "%s/%s/%s", root, a->d_name, c->d_name);
			if (stat(bond, &sb) == 0 && S_ISDIR(sb.st_mode)) continue;
			snprintf(path, sizeof path, "%s/%s", cdir, c->d_name);
			if (unlink(path) == 0) gone++;
		}
		closedir(cd);
	}
	closedir(ad);
	return gone;
}

/* The TrimUI radio, and the H700's under BaseOS (plorpos-7ny.26), which has
 * the same BlueZ tools but no hcitool. */
#if defined(__linux__)

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <poll.h>
#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

/* Run a command with a DEADLINE, capture stdout, return its exit status - or
 * -1 if it could not be run, and -2 if it had to be killed.
 *
 * The deadline is not defensive programming, it is a bug fix. On a fresh card
 * Bluetooth is off, so launch.sh never runs hciattach and there is no adapter;
 * `bluetoothctl power on` in that state does not fail and does not exit, it
 * WAITS for a controller that is never coming. The launcher sat in waitpid
 * behind it and the screen froze on "Turning on...", needing the process
 * killed over adb. Seen on the device 2026-09-06.
 *
 * bluetoothctl has a --timeout of its own and it is used for scanning, but
 * relying on the tool to bound itself is what produced the hang. This bounds
 * it from outside, where a tool that ignores its own timeout cannot reach. */
static int run(char *const argv[], char *out, size_t cap, int timeout_s)
{
	int fd[2], status = -1;
	pid_t pid;
	size_t used = 0;
	struct timespec t0, now;
	bool killed = false;

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
	clock_gettime(CLOCK_MONOTONIC, &t0);

	for (;;) {
		struct pollfd pfd = { fd[0], POLLIN, 0 };
		ssize_t n;
		char scratch[256];
		char *dst;
		size_t room;
		long elapsed;

		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed = now.tv_sec - t0.tv_sec;
		if (elapsed >= timeout_s) {
			/* SIGKILL rather than SIGTERM: a bluetoothctl waiting on a
			 * controller has already shown it is not minded to leave. */
			kill(pid, SIGKILL);
			killed = true;
			break;
		}
		if (poll(&pfd, 1, 200) <= 0) continue;

		dst  = (out && used + 1 < cap) ? out + used : scratch;
		room = (out && used + 1 < cap) ? cap - used - 1 : sizeof scratch;
		n = read(fd[0], dst, room);
		if (n <= 0) break;
		if (dst != scratch) used += (size_t)n;
	}
	if (out && cap) out[used] = '\0';
	close(fd[0]);
	if (waitpid(pid, &status, 0) < 0) return -1;
	if (killed) return -2;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* bluetoothctl with the agent always supplied. A headset will not pair without
 * one, and bluetoothctl registers a default agent only in interactive mode -
 * which is the single fact that made pairing work at all. */
/* Asking something costs a second or two; pairing legitimately takes longer,
 * because the headset has to answer. Neither may take forever. */
#define BT_ASK_S   6
#define BT_ACT_S  20

static int btctl(char *out, size_t cap, int timeout_s, const char *a, const char *b)
{
	char *argv[8];
	int n = 0;

	argv[n++] = (char *)BLUETOOTHCTL;
	argv[n++] = (char *)"--agent";
	argv[n++] = (char *)"NoInputNoOutput";
	if (a) argv[n++] = (char *)a;
	if (b) argv[n++] = (char *)b;
	argv[n] = NULL;
	return run(argv, out, cap, timeout_s);
}

#if defined(PLATFORM_GKD)
/* ROCKNIX's own switch, so the stack is on or off the way ROCKNIX leaves it:
 * `enable` starts bluetoothd and its agent and records the setting its
 * autostart reads at boot, `disable` the reverse, `save` tars the bonds to
 * the card, which `restore` unpacks at every start - so a bond not saved
 * before a power loss would be gone. */
static bool rocknix_bt(const char *verb)
{
	char out[512];
	char *argv[] = { (char *)"/usr/bin/rocknix-bluetooth", (char *)verb, NULL };
	return run(argv, out, sizeof out, BT_ACT_S) == 0;
}

/* Off on the GKD means no bluetoothd, and bluetoothctl then waits for one
 * rather than failing - so ask /proc first, which forks nothing. */
static bool bluetoothd_running(void)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	bool found = false;

	if (!d) return false;
	while (!found && (e = readdir(d))) {
		char path[300], comm[32] = "";
		FILE *f;

		if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
		snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
		if (!(f = fopen(path, "r"))) continue;
		found = fgets(comm, sizeof comm, f) && !strcmp(comm, "bluetoothd\n");
		fclose(f);
	}
	closedir(d);
	return found;
}
#endif

/* One call for all of them. See bt.h for the two cheaper-looking sources this
 * replaced and why each was wrong. */
int bt_mark_connected_now(bt_device *list, int n)
{
#if defined(PLATFORM_H700)
	/* No hcitool on BaseOS. BlueZ 5.66's own list, which holds a device only
	 * once its link is up - not the half-open one hcitool also showed.
	 * Matched on `Device <mac>` at a line's start, so neither an event line
	 * (`[CHG] Device ...`) nor a device with no name is misread. */
	char out[2048];
	int i, marked = 0;

	if (btctl(out, sizeof out, BT_ASK_S, "devices", "Connected") != 0) return 0;
	for (i = 0; i < n; i++) {
		const char *p;

		list[i].connected = false;
		for (p = out; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL)
			if (!strncmp(p, "Device ", 7) && !strncasecmp(p + 7, list[i].mac, 17)) {
				list[i].connected = true;
				marked++;
				break;
			}
	}
	return marked;
#else
	char con[2048];
	char *argv[] = { (char *)"/usr/bin/hcitool", (char *)"con", NULL };

	if (run(argv, con, sizeof con, BT_ASK_S) != 0) return 0;
	return bt_mark_connected(con, list, n);
#endif
}

bt_state bt_status(void)
{
	char out[512];

	if (access("/sys/class/bluetooth/hci0", F_OK) != 0) return BT_NO_ADAPTER;
#if defined(PLATFORM_GKD)
	if (!bluetoothd_running()) return BT_POWERED_OFF;
#endif
	if (btctl(out, sizeof out, BT_ASK_S, "show", NULL) != 0) return BT_NO_ADAPTER;
	return strstr(out, "Powered: yes") ? BT_READY : BT_POWERED_OFF;
}

/* Read Name= out of a bond's info file. */
static bool bond_name(const char *dir, const char *mac, char *out, size_t n)
{
	char path[600], line[256];
	FILE *f;
	bool trusted = false, named = false, keyed = false;

	snprintf(path, sizeof path, "%s/%s/info", dir, mac);
	if (!(f = fopen(path, "r"))) return false;
	snprintf(out, n, "%s", mac);
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (!strncmp(line, "Name=", 5) && line[5]) {
			/* Truncated on purpose: a headset can advertise a name far
			 * longer than a menu row, and a short one is what the screen
			 * wants anyway. Bounded explicitly so it is not a warning. */
			snprintf(out, n, "%.*s", (int)n - 1, line + 5);
			named = true;
		} else if (!strcmp(line, "Trusted=true")) {
			trusted = true;
		} else if (!strcmp(line, "[LinkKey]")) {
			keyed = true;
		}
	}
	fclose(f);
	(void)named;
	/* Trusted AND keyed. Trust alone is what a memory-only pairing leaves on
	 * the card after bluetoothd restarts: a record that connects and drops
	 * every 25 seconds, which this listed as paired and bt_reconnect kept
	 * dialing. Without the key it is a device to pair, not one that is. */
	return trusted && keyed;
}

int bt_bonded(bt_device *out, int max)
{
	DIR *ad;
	struct dirent *a;
	int n = 0;

	if (!out || max <= 0) return 0;
	if (!(ad = opendir(bonds()))) return 0;
	while ((a = readdir(ad)) && n < max) {
		char adapter[512];
		DIR *dd;
		struct dirent *d;

		if (a->d_name[0] == '.') continue;
		snprintf(adapter, sizeof adapter, "%s/%s", bonds(), a->d_name);
		if (!(dd = opendir(adapter))) continue;
		while ((d = readdir(dd)) && n < max) {
			if (!bt_mac_valid(d->d_name)) continue;
			if (!bond_name(adapter, d->d_name, out[n].name, BT_NAME_MAX))
				continue;                     /* bonded but not trusted */
			snprintf(out[n].mac, BT_MAC_MAX, "%s", d->d_name);
			out[n].bonded = true;
			out[n].connected = false;
			n++;
		}
		closedir(dd);
	}
	closedir(ad);
	return n;
}

int bt_visible(bt_device *out, int max)
{
	char text[4096];
	int n, i, j, nb;
	bt_device bond[BT_MAX];

	if (!out || max <= 0) return 0;
	if (btctl(text, sizeof text, BT_ASK_S, "devices", NULL) != 0) return bt_bonded(out, max);
	n = bt_parse_devices(text, out, max);

	/* Mark the ones that are bonded, so the screen can say which will come
	 * back on their own and which are merely in range. */
	nb = bt_bonded(bond, BT_MAX);
	for (i = 0; i < n; i++)
		for (j = 0; j < nb; j++)
			if (!strcasecmp(out[i].mac, bond[j].mac)) {
				out[i].bonded = true;
				if (bond[j].name[0]) snprintf(out[i].name, BT_NAME_MAX, "%s", bond[j].name);
				break;
			}

	bt_mark_connected_now(out, n);

	/* A bonded device out of range does not appear in `devices` at all, and
	 * leaving it out would make forgetting one impossible. */
	for (j = 0; j < nb && n < max; j++) {
		for (i = 0; i < n; i++)
			if (!strcasecmp(out[i].mac, bond[j].mac)) break;
		if (i == n) out[n++] = bond[j];
	}
	return n;
}

bool bt_scan(int secs)
{
	pid_t pid;
	char timeout[16];

	if (secs < 1) secs = 1;
	snprintf(timeout, sizeof timeout, "%d", secs);

	/* Double-forked and never waited on: `scan on` runs for the timeout and
	 * the UI must not stop for it. The same shape wifi.c uses for its own
	 * slow calls. */
	pid = fork();
	if (pid < 0) return false;
	if (pid == 0) {
		if (fork() == 0) {
			char *argv[] = { (char *)BLUETOOTHCTL, (char *)"--timeout",
			                 timeout, (char *)"scan", (char *)"on", NULL };
			int null = open("/dev/null", O_RDWR);
			if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); }
			execv(argv[0], argv);
			_exit(127);
		}
		_exit(0);
	}
	waitpid(pid, NULL, 0);
	return true;
}

/* `connect` reports Failed for a2dp even when the link came up, so success is
 * read from `info` rather than from the exit status. That is the single most
 * expensive thing anyone learned about this stack. */
static bool info_says(const char *mac, const char *needle)
{
	char out[1024];

	if (!bt_mac_valid(mac)) return false;
	if (btctl(out, sizeof out, BT_ASK_S, "info", mac) < 0) return false;
	return strstr(out, needle) != NULL;
}

/* Whether BlueZ wrote a link key for this device, which is the only thing
 * that makes a bond survive bluetoothd restarting. `Paired: yes` does not say
 * so: it is true of a key held in memory too. */
static bool bond_has_key(const char *mac)
{
	DIR *ad;
	struct dirent *a;
	bool found = false;

	if (!(ad = opendir(bonds()))) return false;
	while (!found && (a = readdir(ad))) {
		char path[700], line[64];
		FILE *f;

		if (a->d_name[0] == '.') continue;
		snprintf(path, sizeof path, "%s/%s/%.17s/info", bonds(), a->d_name, mac);
		if (!(f = fopen(path, "r"))) continue;
		while (fgets(line, sizeof line, f))
			if (!strncmp(line, "[LinkKey]", 9)) { found = true; break; }
		fclose(f);
	}
	closedir(ad);
	return found;
}

bool bt_pair(const char *mac, char *err, size_t n)
{
	char out[1024], scratch[256];

	if (err && n) err[0] = '\0';
	if (!bt_mac_valid(mac)) { if (err) snprintf(err, n, "not an address"); return false; }

	/* Bondable for the pairing and no longer. The adapter comes up
	 * non-bondable, and then the kernel negotiates no-bonding: the pairing
	 * succeeds, the link encrypts, `Paired: yes`, and the key arrives with
	 * store_hint 0, so BlueZ never writes it. Everything works until
	 * bluetoothd restarts, and then the headset is a trust record with no key
	 * that connects and drops every 25 seconds. Measured 2026-09-24; every
	 * pairing made from this screen before then was memory-only. Left on
	 * permanently, anything that knows the address could bond at any time. */
	btctl(scratch, sizeof scratch, BT_ASK_S, "pairable", "on");
	btctl(out, sizeof out, BT_ACT_S, "pair", mac);
	btctl(scratch, sizeof scratch, BT_ASK_S, "pairable", "off");

	if (!info_says(mac, "Paired: yes")) {
		if (err) snprintf(err, n, "%s", strstr(out, "AuthenticationFailed")
		                  ? "the headset refused the pairing"
		                  : "pairing did not complete");
		return false;
	}
	/* Judged by the key on disk, not by `Paired: yes`, which was the check
	 * that let a memory-only pairing through. Removed rather than left, or a
	 * retry finds it already paired and never gets a new key. */
	if (!bond_has_key(mac)) {
		btctl(scratch, sizeof scratch, BT_ASK_S, "remove", mac);
		if (err) snprintf(err, n, "the pairing was not saved");
		return false;
	}
	/* Trusted, or it will not reconnect on its own at the next boot - which
	 * is the whole reason the bond is worth having. */
	btctl(out, sizeof out, BT_ASK_S, "trust", mac);
#if defined(PLATFORM_GKD)
	rocknix_bt("save");
#endif
	return true;
}

/* Ask launch.sh's bt_reconnect for a pass now rather than at the end of its
 * twenty seconds, so the published sink follows what this screen just did.
 * A file it polls once a second; see bt_reconnect. */
static void publish_soon(void)
{
	int fd = open("/tmp/tortos_btpass", O_WRONLY | O_CREAT, 0644);

	if (fd >= 0) close(fd);
}

bool bt_connect(const char *mac, char *err, size_t n)
{
	char out[1024];

	if (err && n) err[0] = '\0';
	if (!bt_mac_valid(mac)) { if (err) snprintf(err, n, "not an address"); return false; }
	btctl(out, sizeof out, BT_ACT_S, "connect", mac);
	if (info_says(mac, "Connected: yes")) { publish_soon(); return true; }
	if (err) snprintf(err, n, "it did not connect");
	return false;
}

bool bt_power(bool on)
{
#if defined(PLATFORM_GKD)
	return rocknix_bt(on ? "enable" : "disable");
#else
	char out[512];
	return btctl(out, sizeof out, BT_ASK_S, "power", on ? "on" : "off") == 0;
#endif
}

bool bt_disconnect(const char *mac)
{
	char out[512];
	if (!bt_mac_valid(mac)) return false;
	btctl(out, sizeof out, BT_ACT_S, "disconnect", mac);
	publish_soon();
	return !info_says(mac, "Connected: yes");
}

int bt_disconnect_others(const char *keep)
{
	bt_device bonded[BT_MAX];
	int n = bt_bonded(bonded, BT_MAX), i, gone = 0;

	bt_mark_connected_now(bonded, n);
	for (i = 0; i < n; i++)
		if (bonded[i].connected && strcasecmp(bonded[i].mac, keep) &&
		    bt_disconnect(bonded[i].mac))
			gone++;
	return gone;
}

/* BlueZ keeps a cache entry per device beside the bonds, named for the address
 * and holding the friendly name and the A2DP endpoint capabilities. `remove`
 * deletes the bond and leaves this - observed on 5.54; whether that is
 * deliberate is not something I know.
 *
 * It carries no key material, so leaving it cannot let a forgotten headset
 * reconnect. What it does leave is a record on the card that the device was
 * here, with its name and address, and the row is called Forget rather than
 * Unpair. Deleting it costs a slower first reconnect next time - the name is
 * re-fetched and the endpoints renegotiated - and cannot break pairing,
 * because nothing in it is needed to pair. */
static void forget_cache(const char *mac)
{
	DIR *ad;
	struct dirent *a;

	if (!(ad = opendir(bonds()))) return;
	while ((a = readdir(ad))) {
		char path[700];

		if (a->d_name[0] == '.') continue;
		snprintf(path, sizeof path, "%s/%s/cache/%.17s", bonds(), a->d_name, mac);
		unlink(path);
	}
	closedir(ad);
}


bool bt_forget(const char *mac)
{
	char out[512];

	if (!bt_mac_valid(mac)) return false;
	btctl(out, sizeof out, BT_ASK_S, "remove", mac);
	if (info_says(mac, "Paired: yes")) return false;
	forget_cache(mac);
	publish_soon();
#if defined(PLATFORM_GKD)
	rocknix_bt("save");
#endif
	return true;
}

bool bt_asoundrc(const char *tortos_dir, const char *userdata_dir)
{
#if defined(PLATFORM_GKD)
	/* No bluealsa and no .asoundrc: PipeWire makes the sink itself. */
	(void)tortos_dir; (void)userdata_dir;
	return true;
#else
	char script[512];
	char *argv[6];
	pid_t pid;
	int st;

	if (!tortos_dir || !*tortos_dir || !userdata_dir || !*userdata_dir) return false;
	if (snprintf(script, sizeof script, "%s/bt-alsa.sh", tortos_dir)
	    >= (int)sizeof script)
		return false;

	/* The paths go in as $0 and $1, never pasted into the command, so no path
	 * is ever read as shell. The same reason the rest of this file uses execv
	 * and not popen. */
	argv[0] = (char *)"/bin/sh";
	argv[1] = (char *)"-c";
	argv[2] = (char *)". \"$0\" && USERDATA_PATH=$1 && bt_write_asoundrc";
	argv[3] = script;
	argv[4] = (char *)userdata_dir;
	argv[5] = NULL;

	pid = fork();
	if (pid < 0) return false;
	if (pid == 0) {
		/* Nothing between fork and exec that is not async-signal-safe: the
		 * launcher has threads. stderr stays, so a failure lands in the log. */
		int null = open("/dev/null", O_RDONLY);
		if (null >= 0) { dup2(null, 0); close(null); }
		execv(argv[0], argv);
		_exit(127);
	}
	if (waitpid(pid, &st, 0) != pid) return false;
	return WIFEXITED(st) && WEXITSTATUS(st) == 0;
#endif
}

#else   /* not __linux__ */

bt_state bt_status(void) { return BT_NO_ADAPTER; }
int  bt_bonded(bt_device *out, int max) { (void)out; (void)max; return 0; }
int  bt_visible(bt_device *out, int max) { (void)out; (void)max; return 0; }
bool bt_scan(int secs) { (void)secs; return false; }
bool bt_pair(const char *m, char *e, size_t n) { (void)m; if (e && n) e[0] = 0; return false; }
bool bt_connect(const char *m, char *e, size_t n) { (void)m; if (e && n) e[0] = 0; return false; }
int  bt_mark_connected_now(bt_device *l, int n) { (void)l; (void)n; return 0; }
bool bt_power(bool on) { (void)on; return false; }
bool bt_disconnect(const char *m) { (void)m; return false; }
int  bt_disconnect_others(const char *k) { (void)k; return 0; }
bool bt_forget(const char *m) { (void)m; return false; }
bool bt_asoundrc(const char *t, const char *d) { (void)t; (void)d; return false; }

#endif
