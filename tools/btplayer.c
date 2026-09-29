/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* btplayer: a media player registered with BlueZ that plays nothing, so that
 * a headset's volume reaches the Brick.
 *
 * BlueZ 5.54 negotiates AVRCP absolute volume with a headset and then drops
 * what the headset reports: avrcp_volume_changed() hands the value to the
 * target's player, and returns early when there is none. On a phone the music
 * app is that player. On the Brick nothing registered one, bluealsa included,
 * so the A2DP transport's Volume stayed at its initial -1, the property stayed
 * hidden, and bluealsa's --a2dp-volume failed with "No such property 'Volume'".
 * A headset with no buttons of its own had no volume control anywhere.
 *
 * Measured 2026-09-25 with the OpenFit: registering a player with an empty
 * property dict is the whole fix. The transport gains Volume, setting bluealsa's
 * `<name> - A2DP` control sends SetAbsoluteVolume, and the change is heard.
 *
 * It has to stay alive: BlueZ unregisters a player whose owner leaves the bus.
 * launch.sh's bt_on starts it after bluetoothd and bt_off stops it, because a
 * bluetoothd that restarts has forgotten it.
 *
 * libdbus is the device's own, reached with dlopen: the sysroot carries no
 * headers for it, so the handful of types used are declared here. Every call
 * the headset makes on the player - play, pause, from its buttons - is answered
 * UnknownMethod by libdbus, which BlueZ passes on as a failure and nothing more.
 *
 * It also mends a volume BlueZ lost. BlueZ 5.54 hands a headset's report only
 * to the A2DP transports that exist at that moment, and keeps the value in the
 * player so that a report of the same level again is dropped too
 * (media.c set_volume). A transport created AFTER the report - a reconnect
 * from the Bluetooth screen, 2026-09-26 - has no Volume, the keys stop
 * reaching the headset, and nothing says so. Re-reporting alone did not bring
 * it back, and a fresh player alone did not; the two together did, measured
 * the same day: Volume 70, the headset's own level. So each new transport is
 * looked at two seconds after it appears, and one without a Volume gets
 * exactly that - once, so a headset that cannot do absolute volume is tried
 * once and not forever.
 *
 * And it passes a headset's OWN volume changes on. BlueZ announces the
 * transport's Volume (PropertiesChanged) whenever the headset reports one:
 * the OpenRun announced every press of its buttons, measured 2026-09-26 -
 * and every value the Brick sent it as well, which the launcher tells apart
 * by time (bt_volume_follow). Written to /tmp/tortos_btvol as
 * `<PCM name> <0..127> <stamp>`, the stamp being this clock's milliseconds,
 * which only go up - so a new report never reads as the last one, as a
 * reused inode number could make it. Nothing for four seconds after a
 * transport appears or is mended: that is the headset reporting where it
 * already was, and at connect the Brick's level is the one that wins.
 *
 * And a headset's play, pause, next and previous. Not as calls on this player
 * - none came, from either headset - but as keys: BlueZ gives every AVRCP
 * headset a virtual keyboard named `<headset> (AVRCP)` and presses its keys.
 * The OpenRun sent KEY_PLAYCD for every press, never pause; the OpenFit sent
 * KEY_PAUSECD and KEY_PLAYCD as it saw fit, and KEY_NEXTSONG for a hold -
 * measured 2026-09-26. Gestures are the headset's own business and cannot be
 * seen from here; only what they mean arrives. Any `(AVRCP)` keyboard is
 * watched, and a press is written to /tmp/tortos_btkey as `<action> <stamp>`
 * for the launcher to hand to Muse: toggle, pause, next or prev.
 */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct { const char *name, *message; unsigned bits; void *pad; } dbus_error;
typedef struct { void *pad[16]; } dbus_iter;      /* at least DBusMessageIter's size */
typedef struct {
	void (*unregister_fn)(void *, void *);
	int  (*message_fn)(void *, void *, void *);
	void (*pad[4])(void *);
} dbus_vtable;

#define BUS_SYSTEM      1
#define TYPE_OBJECT     ((int)'o')
#define TYPE_ARRAY      ((int)'a')
#define TYPE_STRING     ((int)'s')
#define TYPE_DICT_ENTRY ((int)'e')
#define TYPE_UINT16     ((int)'q')
#define NOT_HANDLED     1
#define PLAYER_PATH     "/org/tortos/player"

static void *(*bus_get)(int, dbus_error *);
static void  (*error_init)(dbus_error *);
static void  (*error_free)(dbus_error *);
static void *(*new_call)(const char *, const char *, const char *, const char *);
static void  (*message_unref)(void *);
static void  (*iter_init_append)(void *, dbus_iter *);
static int   (*append_basic)(dbus_iter *, int, const void *);
static int   (*open_container)(dbus_iter *, int, const char *, dbus_iter *);
static int   (*close_container)(dbus_iter *, dbus_iter *);
static void *(*send_block)(void *, void *, int, dbus_error *);
static int   (*register_path)(void *, const char *, const dbus_vtable *, void *);
static int   (*dispatch)(void *, int);
static void  (*add_match)(void *, const char *, dbus_error *);
static int   (*add_filter)(void *, int (*)(void *, void *, void *), void *, void (*)(void *));
static int   (*is_signal)(void *, const char *, const char *);
static int   (*iter_init)(void *, dbus_iter *);
static int   (*iter_type)(dbus_iter *);
static void  (*iter_recurse)(dbus_iter *, dbus_iter *);
static int   (*iter_next)(dbus_iter *);
static void  (*iter_get)(dbus_iter *, void *);
static const char *(*message_path)(void *);

static int on_message(void *conn, void *msg, void *data)
{
	(void)conn; (void)msg; (void)data;
	return NOT_HANDLED;
}

static bool bind_libdbus(void)
{
	void *lib = dlopen("libdbus-1.so.3", RTLD_NOW);

	if (!lib) return false;
#define SYM(v, n) if (!(*(void **)&v = dlsym(lib, n))) return false
	SYM(bus_get,          "dbus_bus_get");
	SYM(error_init,       "dbus_error_init");
	SYM(error_free,       "dbus_error_free");
	SYM(new_call,         "dbus_message_new_method_call");
	SYM(message_unref,    "dbus_message_unref");
	SYM(iter_init_append, "dbus_message_iter_init_append");
	SYM(append_basic,     "dbus_message_iter_append_basic");
	SYM(open_container,   "dbus_message_iter_open_container");
	SYM(close_container,  "dbus_message_iter_close_container");
	SYM(send_block,       "dbus_connection_send_with_reply_and_block");
	SYM(register_path,    "dbus_connection_register_object_path");
	SYM(dispatch,         "dbus_connection_read_write_dispatch");
	SYM(add_match,        "dbus_bus_add_match");
	SYM(add_filter,       "dbus_connection_add_filter");
	SYM(is_signal,        "dbus_message_is_signal");
	SYM(iter_init,        "dbus_message_iter_init");
	SYM(iter_type,        "dbus_message_iter_get_arg_type");
	SYM(iter_recurse,     "dbus_message_iter_recurse");
	SYM(iter_next,        "dbus_message_iter_next");
	SYM(iter_get,         "dbus_message_iter_get_basic");
	SYM(message_path,     "dbus_message_get_path");
#undef SYM
	return true;
}

/* RegisterPlayer(o path, a{sv} {}). An empty dict is valid: BlueZ reads the
 * properties it is given and needs none. */
static bool register_player(void *conn)
{
	const char *path = PLAYER_PATH;
	void *msg, *reply;
	dbus_iter args, dict;
	dbus_error err;

	error_init(&err);
	msg = new_call("org.bluez", "/org/bluez/hci0", "org.bluez.Media1", "RegisterPlayer");
	if (!msg) return false;
	iter_init_append(msg, &args);
	append_basic(&args, TYPE_OBJECT, &path);
	open_container(&args, TYPE_ARRAY, "{sv}", &dict);
	close_container(&args, &dict);
	reply = send_block(conn, msg, 5000, &err);
	message_unref(msg);
	if (!reply) {
		fprintf(stderr, "btplayer: RegisterPlayer: %s\n",
		        err.message ? err.message : "no reply");
		error_free(&err);
		return false;
	}
	message_unref(reply);
	return true;
}

static long now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* A method call with up to two string-ish arguments, waiting for the reply.
 * The reply for the caller to read and unref, or NULL on any error. */
static void *call(void *conn, const char *path, const char *iface, const char *method,
                  int t1, const char *a1, int t2, const char *a2)
{
	void *msg = new_call("org.bluez", path, iface, method), *reply;
	dbus_iter args;
	dbus_error err;

	if (!msg) return NULL;
	iter_init_append(msg, &args);
	if (a1) append_basic(&args, t1, &a1);
	if (a2) append_basic(&args, t2, &a2);
	error_init(&err);
	reply = send_block(conn, msg, 5000, &err);
	message_unref(msg);
	if (!reply) error_free(&err);
	return reply;
}

static bool has_volume(void *conn, const char *transport)
{
	void *r = call(conn, transport, "org.freedesktop.DBus.Properties", "Get",
	               TYPE_STRING, "org.bluez.MediaTransport1", TYPE_STRING, "Volume");

	if (!r) return false;
	message_unref(r);
	return true;
}

/* The transport's device, e.g. /org/bluez/hci0/dev_C0_86_B3_A7_48_7F. */
static bool device_of(void *conn, const char *transport, char *out, size_t n)
{
	void *r = call(conn, transport, "org.freedesktop.DBus.Properties", "Get",
	               TYPE_STRING, "org.bluez.MediaTransport1", TYPE_STRING, "Device");
	dbus_iter it, var;
	const char *dev = NULL;

	if (!r) return false;
	if (iter_init(r, &it) && iter_type(&it) == (int)'v') {
		iter_recurse(&it, &var);
		if (iter_type(&var) == TYPE_OBJECT) iter_get(&var, &dev);
	}
	if (dev) snprintf(out, n, "%s", dev);
	message_unref(r);
	return dev != NULL;
}

/* A fresh player, so BlueZ has no stored level to call the report a repeat
 * of, then AVRCP again on the device, so the headset reports once more - now
 * that the transport exists to take it. AVRCP is only the control channel:
 * the audio does not stop, and the transport is not recreated by it. */
static void mend(void *conn, const char *transport)
{
	static const char *avrcp = "0000110e-0000-1000-8000-00805f9b34fb";
	char dev[160];
	void *r;

	if (!device_of(conn, transport, dev, sizeof dev)) return;
	if ((r = call(conn, "/org/bluez/hci0", "org.bluez.Media1", "UnregisterPlayer",
	              TYPE_OBJECT, PLAYER_PATH, 0, NULL)))
		message_unref(r);
	if (!register_player(conn)) return;
	if ((r = call(conn, dev, "org.bluez.Device1", "DisconnectProfile",
	              TYPE_STRING, avrcp, 0, NULL)))
		message_unref(r);
	sleep(1);
	if ((r = call(conn, dev, "org.bluez.Device1", "ConnectProfile",
	              TYPE_STRING, avrcp, 0, NULL)))
		message_unref(r);
}

/* Transports to look at, and when. Stage 0 is the first look, two seconds
 * after it appears - a headset whose AVRCP comes up after the transport
 * reports into it by itself; stage 1 is after a mend, and the last. */
#define WATCHED 4
static struct { char path[160]; long due; int stage; } g_watch[WATCHED];

static long g_quiet_until;   /* no headset volumes passed on before this */

/* /org/bluez/hci0/dev_A8_F5_E1_4A_93_71/sep1/fd8 -> bt_A8_F5_E1_4A_93_71, the
 * name launch.sh publishes (bt_pcm_name in bt-alsa.sh). */
static void publish_volume(const char *transport, unsigned volume)
{
	const char *dev = strstr(transport, "/dev_");
	FILE *f;

	if (!dev || strlen(dev) < 5 + 17) return;
	if (!(f = fopen("/tmp/tortos_btvol.tmp", "w"))) return;
	fprintf(f, "bt_%.17s %u %ld\n", dev + 5, volume, now_ms());
	fclose(f);
	rename("/tmp/tortos_btvol.tmp", "/tmp/tortos_btvol");
}

/* PropertiesChanged(s interface, a{sv} changed, as invalidated), on a
 * transport: its Volume, if that is what changed. */
static void on_properties(void *msg)
{
	dbus_iter args, dict, entry, var;
	const char *iface, *key, *path = message_path(msg);

	if (!path || !iter_init(msg, &args) || iter_type(&args) != TYPE_STRING) return;
	iter_get(&args, &iface);
	if (strcmp(iface, "org.bluez.MediaTransport1")) return;
	if (!iter_next(&args) || iter_type(&args) != TYPE_ARRAY) return;
	for (iter_recurse(&args, &dict); iter_type(&dict) == TYPE_DICT_ENTRY; iter_next(&dict)) {
		unsigned short volume;

		iter_recurse(&dict, &entry);
		iter_get(&entry, &key);
		if (strcmp(key, "Volume") || !iter_next(&entry)) continue;
		iter_recurse(&entry, &var);
		if (iter_type(&var) != TYPE_UINT16) continue;
		iter_get(&var, &volume);
		if (now_ms() < g_quiet_until) continue;
		publish_volume(path, volume);
	}
}

static int on_signal(void *conn, void *msg, void *data)
{
	dbus_iter args, dict, entry;
	const char *path = NULL, *iface;
	int i;

	(void)conn; (void)data;
	if (is_signal(msg, "org.freedesktop.DBus.Properties", "PropertiesChanged")) {
		on_properties(msg);
		return NOT_HANDLED;
	}
	if (!is_signal(msg, "org.freedesktop.DBus.ObjectManager", "InterfacesAdded"))
		return NOT_HANDLED;
	if (!iter_init(msg, &args) || iter_type(&args) != TYPE_OBJECT) return NOT_HANDLED;
	iter_get(&args, &path);
	if (!iter_next(&args) || iter_type(&args) != TYPE_ARRAY) return NOT_HANDLED;
	for (iter_recurse(&args, &dict); iter_type(&dict) == TYPE_DICT_ENTRY; iter_next(&dict)) {
		iter_recurse(&dict, &entry);
		iter_get(&entry, &iface);
		if (strcmp(iface, "org.bluez.MediaTransport1")) continue;
		for (i = 0; i < WATCHED && g_watch[i].path[0]; i++) { }
		if (i == WATCHED) break;
		snprintf(g_watch[i].path, sizeof g_watch[i].path, "%s", path);
		g_watch[i].due = now_ms() + 2000;
		g_watch[i].stage = 0;
		g_quiet_until = now_ms() + 4000;
		break;
	}
	return NOT_HANDLED;
}

static void look(void *conn)
{
	int i;

	for (i = 0; i < WATCHED; i++) {
		char *p = g_watch[i].path;

		if (!p[0] || now_ms() < g_watch[i].due) continue;
		if (has_volume(conn, p)) {
			if (g_watch[i].stage) fprintf(stderr, "btplayer: %s has its volume back\n", p);
			p[0] = '\0';
		} else if (g_watch[i].stage == 0) {
			fprintf(stderr, "btplayer: %s came up without a volume; asking again\n", p);
			mend(conn, p);
			g_quiet_until = now_ms() + 4000;
			g_watch[i].stage = 1;
			g_watch[i].due = now_ms() + 3000;
		} else {
			fprintf(stderr, "btplayer: %s still has no volume; leaving it\n", p);
			p[0] = '\0';
		}
	}
}

/* The headsets' AVRCP keyboards. BlueZ makes one per connection and they come
 * and go with it, so the list is read again every two seconds; a node that
 * has gone reads as ENODEV and is closed. */
#define KEYBOARDS 4
static struct { int fd; char node[24]; } g_kbd[KEYBOARDS] = {
	{ -1, "" }, { -1, "" }, { -1, "" }, { -1, "" }
};

static void find_keyboards(void)
{
	FILE *f = fopen("/proc/bus/input/devices", "r");
	char line[256];
	bool avrcp = false;

	if (!f) return;
	while (fgets(line, sizeof line, f)) {
		char *ev, node[24];
		int i, free_slot = -1, have = 0;

		if (!strncmp(line, "N: Name=", 8)) {
			avrcp = strstr(line, " (AVRCP)\"") != NULL;
			continue;
		}
		if (!avrcp || strncmp(line, "H: Handlers=", 12) || !(ev = strstr(line, "event")))
			continue;
		avrcp = false;
		snprintf(node, sizeof node, "/dev/input/%.*s", (int)strspn(ev, "event0123456789"), ev);
		for (i = 0; i < KEYBOARDS; i++) {
			if (g_kbd[i].fd >= 0 && !strcmp(g_kbd[i].node, node)) have = 1;
			if (g_kbd[i].fd < 0 && free_slot < 0) free_slot = i;
		}
		if (have || free_slot < 0) continue;
		if ((g_kbd[free_slot].fd = open(node, O_RDONLY | O_NONBLOCK)) >= 0) {
			snprintf(g_kbd[free_slot].node, sizeof g_kbd[free_slot].node, "%s", node);
			fprintf(stderr, "btplayer: watching %s for a headset's keys\n", node);
		}
	}
	fclose(f);
}

/* Which of Muse's actions a key is. PLAYCD toggles, because the OpenRun sends
 * nothing else for play and for pause; PAUSECD and STOPCD only pause, so the
 * OpenFit, which says which it means, never starts music it meant to stop.
 * FASTFORWARD and REWIND are next and previous, for a headset that sends a
 * hold that way. */
static const char *key_action(unsigned code)
{
	switch (code) {
	case KEY_PLAYCD: case KEY_PLAYPAUSE: case KEY_PLAY: return "toggle";
	case KEY_PAUSECD: case KEY_STOPCD: case KEY_PAUSE:  return "pause";
	case KEY_NEXTSONG: case KEY_FASTFORWARD:           return "next";
	case KEY_PREVIOUSSONG: case KEY_REWIND:            return "prev";
	}
	return NULL;
}

static void read_keyboards(void)
{
	int i;

	for (i = 0; i < KEYBOARDS; i++) {
		struct input_event ev;
		ssize_t n;

		if (g_kbd[i].fd < 0) continue;
		while ((n = read(g_kbd[i].fd, &ev, sizeof ev)) == (ssize_t)sizeof ev) {
			const char *act = ev.type == EV_KEY && ev.value == 1 ? key_action(ev.code) : NULL;
			FILE *f;

			if (!act || !(f = fopen("/tmp/tortos_btkey.tmp", "w"))) continue;
			fprintf(f, "%s %ld\n", act, now_ms());
			fclose(f);
			rename("/tmp/tortos_btkey.tmp", "/tmp/tortos_btkey");
		}
		if (n < 0 && errno != EAGAIN) {          /* the headset went */
			close(g_kbd[i].fd);
			g_kbd[i].fd = -1;
			g_kbd[i].node[0] = '\0';
		}
	}
}

int main(void)
{
	static const dbus_vtable vt = { NULL, on_message, { NULL } };
	void *conn;
	dbus_error err;
	int tries;

	if (!bind_libdbus()) {
		fprintf(stderr, "btplayer: libdbus-1.so.3 is missing or incomplete\n");
		return 1;
	}
	error_init(&err);
	conn = bus_get(BUS_SYSTEM, &err);
	if (!conn) {
		fprintf(stderr, "btplayer: system bus: %s\n", err.message ? err.message : "?");
		return 1;
	}
	if (!register_path(conn, PLAYER_PATH, &vt, NULL)) {
		fprintf(stderr, "btplayer: cannot export %s\n", PLAYER_PATH);
		return 1;
	}
	/* bluetoothd's media interface can come up a moment after bluetoothd
	 * itself, so a first refusal is not the last word. */
	for (tries = 0; tries < 10 && !register_player(conn); tries++)
		sleep(1);
	if (tries == 10) return 1;
	fprintf(stderr, "btplayer: registered %s\n", PLAYER_PATH);

	add_match(conn, "type='signal',sender='org.bluez',"
	          "interface='org.freedesktop.DBus.ObjectManager',member='InterfacesAdded'", &err);
	add_filter(conn, on_signal, NULL, NULL);
	add_match(conn, "type='signal',sender='org.bluez',"
	          "interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',"
	          "arg0='org.bluez.MediaTransport1'", &err);
	/* A tenth of a second, so a headset's key is seen as soon as it is
	 * pressed; the rest of the time this sleeps in the dispatch. */
	{
		long next_find = 0;

		while (dispatch(conn, 100)) {
			look(conn);
			if (now_ms() >= next_find) {
				find_keyboards();
				next_find = now_ms() + 2000;
			}
			read_keyboards();
		}
	}
	return 0;
}
