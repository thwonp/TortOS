/* SPDX-License-Identifier: MIT */
#ifndef TORTOS_WIFI_H
#define TORTOS_WIFI_H

#include <stdbool.h>

/* WiFi, over the stock wpa_supplicant (on the GKD, ROCKNIX's ConnMan).
 *
 * TortOS ships no supplicant and no DHCP client of its own. The device has
 * wpa_supplicant, wpa_cli, udhcpc and iw already, procd supervises the
 * supplicant, and /usr/share/udhcpc/default.script already knows how to put
 * an address on an interface. All of that is firmware, and the whole of this
 * file is talking to it rather than replacing it -- the same position taken
 * on volume and brightness, and for the same reason.
 *
 * Everything here is a no-op returning failure on the host build. The shelf
 * has to keep running under `make native` with no radio anywhere near it.
 */

#define WIFI_SSID_MAX  33     /* 32 octets plus the terminator */
#define WIFI_MAX_NETS  24

typedef struct {
	char ssid[WIFI_SSID_MAX];
	int  signal;              /* dBm, less negative is stronger */
	bool secured;             /* needs a passphrase */
	bool known;               /* already saved in wpa_supplicant.conf */
} wifi_net;

typedef enum {
	WIFI_OFF,                 /* the supplicant is not running */
	WIFI_IDLE,                /* running, not associated */
	WIFI_CONNECTING,
	WIFI_CONNECTED
} wifi_state;

/* Start or stop the stock service. `wifi_up` is idempotent and waits for the
 * control socket, so a caller can treat it as "make wifi usable". */
bool wifi_up(void);
void wifi_down(void);

/* Trigger a scan and read the results, strongest first, one entry per SSID.
 * Returns the count, or -1 if the supplicant is not reachable. Hidden
 * networks are dropped: without an SSID there is nothing to show or select. */
int wifi_scan(wifi_net *out, int max);

/* The networks the supplicant already holds, with no scan and no radio time.
 * Instant, so a screen can show something while a scan runs. `signal` is 0 and
 * meaningless: a saved network is not necessarily in range. */
int wifi_known(wifi_net *out, int max);

/* A scan the caller does not wait for. A full scan takes several seconds by
 * design - the weak networks arrive last, and cutting it short drops exactly
 * the ones somebody is looking for - so this splits the waiting out.
 *
 *   wifi_scan_start()  kick it off; false if the supplicant is not answering
 *   wifi_scan_poll()   0 running, 1 results ready, -1 no scan running.
 *                      Safe every frame: it does nothing until its next
 *                      second is due.
 *   wifi_scan_take()   fill the array from the results poll reported ready
 *
 * wifi_scan() above is these three with the waiting put back in. */
bool wifi_scan_start(void);
int  wifi_scan_poll(void);
int  wifi_scan_take(wifi_net *out, int max);

/* Associate, and on success write the credential to wpa_supplicant.conf and
 * take a DHCP lease. `psk` may be NULL or empty for an open network. Blocking,
 * with an internal timeout. */
bool wifi_connect(const char *ssid, const char *psk);

/* Current state. `ssid` and `ip` may be NULL. */
wifi_state wifi_status(char *ssid, int ssid_cap, char *ip, int ip_cap);

/* Drop a saved network and forget its credential. */
bool wifi_forget(const char *ssid);

/* The network services a device lets you switch: SSH, Samba and Syncthing on
 * the GKD, where ROCKNIX runs them, and none on the Brick. On/off is ROCKNIX's own
 * setting, so it survives a reboot and the stock menu agrees with it. */
typedef enum { WIFI_SSH, WIFI_SAMBA, WIFI_SYNCTHING, WIFI_NSVC } wifi_svc;
#ifdef PLATFORM_GKD
#define WIFI_SVC_ROWS WIFI_NSVC
#else
#define WIFI_SVC_ROWS 0
#endif
bool wifi_svc_on(wifi_svc s);
bool wifi_svc_set(wifi_svc s, bool on);

#endif
