/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* Bluetooth audio, over the stock BlueZ. Like wifi.h, none of this is TortOS's
 * own stack - it is a client of firmware that already works, and the four
 * things that were not obvious about getting it working are in the backlog.
 *
 * Three of them matter to this file:
 *
 *   THE AGENT IS THE WHOLE BALLGAME. A headset will not pair without one, and
 *   bluetoothctl only registers a default agent in interactive mode. Every
 *   pair here passes --agent NoInputNoOutput explicitly.
 *
 *   JUDGE BY `info`, NEVER BY THE RETURN OF `connect`. bluetoothctl reports
 *   Failed for a2dp even when the link came up. Cost an evening.
 *
 *   BONDS LIVE WHERE bluetoothd'S BUILD SAYS: /etc/lib/bluetooth/<adapter>/
 *   <device>/ on the Brick (5.54, --localstatedir=/etc), where the
 *   /etc/bluetooth/keys/ the init wrapper symlinks into being is a decoy; but
 *   /var/lib/bluetooth -> /etc/bluetooth/keys/ on the Brick Pro (5.78).
 *   bt-alsa.sh asks the binary which, as $TORTOS_BT_BONDS. Reading the bonds directly is also how the paired list is built
 *   without forking anything.
 *
 * FORKING IS THE COST HERE. This process is ~119 MB, every query is a fork,
 * and the menu loop runs every frame - so nothing in here is called per frame.
 * The screen refreshes on an interval, the same shape menu_wifi uses.
 */
#ifndef TORTOS_BT_H
#define TORTOS_BT_H

#include <stdbool.h>
#include <stddef.h>

#define BT_MAC_MAX   18      /* AA:BB:CC:DD:EE:FF plus NUL */
#define BT_NAME_MAX  64
#define BT_MAX       24

typedef struct {
	char mac[BT_MAC_MAX];
	char name[BT_NAME_MAX];
	bool bonded;         /* trusted and keyed, so it reconnects on its own at boot */
	bool connected;
} bt_device;

typedef enum { BT_NO_ADAPTER, BT_POWERED_OFF, BT_READY } bt_state;

/* --- pure, and checked on the host ---------------------------------------
 * An address is the one thing here that is ever handed back to bluetoothctl as
 * an argument, so it is validated rather than trusted. A device NAME is
 * arbitrary bytes chosen by whoever owns the headset, arriving over the air
 * into a process running as root - it is never passed to anything. */
bool bt_mac_valid(const char *mac);

/* "Device AA:BB:CC:DD:EE:FF Some Name", one per line, as `bluetoothctl
 * devices` prints it. Returns how many were understood; anything else on a
 * line is skipped rather than guessed at. */
int bt_parse_devices(const char *text, bt_device *out, int max);

/* Mark every device in `list` that appears in `hcitool con` output. Returns
 * how many were marked.
 *
 * ONE call for ALL of them, which is the whole point. This started as
 * /tmp/tortos_btsink, which launch.sh writes on a twenty-second poll and so
 * knows nothing about a connect this screen just made; then as an `info` per
 * row, which was a fork each and which I limited to the row under the cursor
 * to keep the cost down. That made the label depend on where the CURSOR was -
 * move off the headset and it went back to saying "paired". Connection state
 * is a property of the device, not of the selection. Seen on the device
 * 2026-09-06. */
int bt_mark_connected(const char *hcitool_con, bt_device *list, int n);

/* The right-hand column for one device. Pure, so the wording is checkable:
 * "connected" has to outrank "paired", or a successful connect redraws the
 * list saying exactly what it said before and the screen gives no sign
 * anything happened. wifi_label learned that the hard way. */
void bt_label(const bt_device *d, char *out, size_t n);

/* Run hcitool once and mark `list`. The one place that knows how the answer is
 * obtained, so the menu row and the screen cannot disagree about it - which
 * they did, when each had its own source. */
int bt_mark_connected_now(bt_device *list, int n);

bt_state bt_status(void);

/* Bonded devices, read from the bond directories - no fork. */
int bt_bonded(bt_device *out, int max);

/* Everything BlueZ currently knows about, bonded or merely seen. One fork. */
int bt_visible(bt_device *out, int max);

/* Scan in the background for `secs`, so the UI never blocks on it. Results
 * arrive in bt_visible as they are found. */
bool bt_scan(int secs);

/* Each of these forks once and blocks. `err` takes something worth showing. */
bool bt_pair(const char *mac, char *err, size_t n);
bool bt_connect(const char *mac, char *err, size_t n);
/* Is this one connected, asked of BlueZ rather than inferred. One fork, so
 * the caller decides how often - the screen asks only about the row under the
 * cursor.
 *
 * bt_visible marks connections from /tmp/tortos_btsink, which launch.sh writes
 * on a twenty-second poll. That is fine for a headset that reconnected on its
 * own and useless right after the UI connects one: the file has not caught up,
 * so the row redraws saying "paired" and the screen looks like it did nothing.
 * Seen on the device 2026-09-06. */
/* Power the adapter. This is the immediate half of the toggle and it only
 * works when the stack is already up - hciattach, bluetoothd and bluealsa are
 * started by launch.sh from the stored preference, so turning Bluetooth on
 * from a cold boot that had it off takes effect at the next boot. */
bool bt_power(bool on);

bool bt_connected(const char *mac);

bool bt_disconnect(const char *mac);
/* Disconnect every connected bonded device but `keep`, and return how many.
 * For the Bluetooth screen, where connecting a headset means choosing it. */
int  bt_disconnect_others(const char *keep);
bool bt_forget(const char *mac);

/* Rewrite .asoundrc from the current bonds, so a newly paired headset has a
 * PCM waiting for it.
 *
 * By running bt_write_asoundrc from bt-alsa.sh in `tortos_dir` - the same
 * function launch.sh runs at boot, not a copy of it. There used to be a second
 * implementation here, held to the shell one by a check on the names alone;
 * bt-alsa.sh says why there is one now. Waits for the shell to finish, and its
 * errors go to the log.
 *
 * A running emulator or Muse picks it up at its next open, because launch.sh
 * names .asoundrc in ALSA_CONFIG_PATH. Without that it arrived too late:
 * alsa-lib 1.1.8 re-reads only the top-level files on that list and never
 * one loaded through alsa.conf's @hooks, which is how .asoundrc was reached,
 * so a new headset was `Unknown PCM` to both until they restarted. Measured
 * 2026-09-05 and again, with the cause, 2026-09-25. */
bool bt_asoundrc(const char *tortos_dir, const char *userdata_dir);

/* Delete every cached device that is not bonded, and return how many went.
 *
 * BlueZ writes a cache entry - name and A2DP endpoints - for every device it
 * SCANS, not only the ones you pair with. So the search button leaves a
 * permanent record on the card of every named device that was in range when it
 * was pressed, accumulating, with nothing to clear it. Two entries appeared
 * after one scan on 2026-09-06 and only one of them was the headset.
 *
 * Bonded devices keep theirs: that is the cache doing its job, for a device
 * the player chose. Forgetting one removes it, which bt_forget already does.
 *
 * `root` is the bonds directory, a parameter rather than a constant because
 * this deletes files and a check should be able to point it somewhere safe.
 * Pass NULL for the real one. */
int bt_sweep_cache(const char *root);

#endif
