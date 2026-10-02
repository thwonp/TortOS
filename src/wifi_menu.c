/* The Wi-Fi screen, minus the screen.
 *
 * State and row building only. This file must never include SDL: ADR-0001 says
 * a build function has to be callable with no renderer and no device, and
 * tools/menu-check.c links it directly to prove it. If that stops being true
 * the decision has failed and should be reopened rather than the requirement
 * quietly dropped.
 *
 * The half that needs a screen - wifi_key, wifi_screen - stays in main.c.
 */
#include "wifi_menu.h"

#include <stdio.h>
#include <string.h>

/* Signal as a word. dBm is the honest number and it is also jargon; the list
 * is sorted strongest first anyway, so the word only has to separate "this
 * will work" from "this will not". */
const char *wifi_strength(int dbm)
{
	if (dbm >= -60) return "strong";
	if (dbm >= -72) return "good";
	return "weak";
}


/* The right-hand column for every row, rebuilt whenever the list changes.
 *
 * "connected" outranks "saved": after a successful connect the list was redrawn
 * saying only that the network was known, which is what it said before the
 * connect too, so the screen gave no sign anything had happened. */
void wifi_label(wifi_ui *w)
{
	char cur[WIFI_SSID_MAX], ip[64];
	int i;

	w->conn[0] = w->st_ui[0] = '\0';
	w->connected = wifi_status(cur, sizeof cur, ip, sizeof ip) == WIFI_CONNECTED;
	if (!w->connected) cur[0] = '\0';
	else if (ip[0]) {
		snprintf(w->conn, sizeof w->conn, "%s · %s", cur, ip);
		snprintf(w->st_ui, sizeof w->st_ui, "%s:8384", ip);
	}
	for (i = 0; i < w->n; i++) {
		const char *state = (cur[0] && !strcmp(cur, w->nets[i].ssid))
		                    ? " - connected"
		                    : w->nets[i].known ? " - saved"
		                    : w->nets[i].secured ? "" : " - open";

		/* A saved network listed before the scan has no signal reading, and
		 * printing "weak" for one that has simply not been heard from yet
		 * would be a measurement we do not have. */
		if (!w->scanned && w->nets[i].known)
			snprintf(w->vals[i], sizeof w->vals[i], "saved");
		else
			snprintf(w->vals[i], sizeof w->vals[i], "%s%s",
			         wifi_strength(w->nets[i].signal), state);
	}
}

/* Start a scan and show what we already know while it runs.
 *
 * Entering this screen used to stand still behind a "Scanning..." panel for up
 * to nine seconds, because a scan settles slowly on purpose. The saved networks
 * need no scan at all - the supplicant is holding them - so they go up
 * immediately and the scan fills in around them. */
void wifi_begin_scan(wifi_ui *w)
{
	w->scanned = false;
	w->n = wifi_known(w->nets, WIFI_MAX_NETS);
	wifi_label(w);
	w->scanning = wifi_scan_start();
}

/* Every row goes through here, so no state - a long list, a small buffer -
 * writes past what the caller gave. */
#define ADD(...) do { if (nrows < max) rows[nrows++] = (__VA_ARGS__); } while (0)

int wifi_build(void *ctx, menu_row *rows, int max, const char **heading)
{
	wifi_ui *w = ctx;
	int nrows = 0, i;
	bool any_saved = false;

	*heading = "Wi-Fi Services";

	/* The scan is driven from here because this is the function that already
	 * runs every frame. wifi_scan_poll() does nothing until its next second is
	 * due, so calling it per frame costs a comparison. When results land the
	 * provisional list of saved networks is replaced by what the radio heard.
	 *
	 * This is still the only place rows are built, and it still touches no
	 * renderer - ADR-0001's requirement holds. */
	if (w->scanning && wifi_scan_poll() == 1) {
		int got = wifi_scan_take(w->nets, WIFI_MAX_NETS);

		w->scanning = false;
		w->scanned = true;
		if (got >= 0) w->n = got;
		wifi_label(w);
	}

	/* The switch is row 0, so the state of the radio is the first thing read
	 * and the first thing reachable. */
	ADD((menu_row){ "Wi-Fi", w->on ? "on" : "off", true });
	/* Under the switch and above the list, so they stay put while a scan
	 * changes the list's length (WIFI_TOP_ROWS). An account, reading "sign
	 * in" until there is one, and the file server, dead until there is a
	 * network to serve it on - the same words the plorpOS menu used for both
	 * before they moved here (plorpos-z0d.1). */
	ADD((menu_row){ "Cheevos", w->ra_name ? w->ra_name : "sign in", true });
	/* Not tied to the radio: they serve whatever network the device is on. */
	if (WIFI_SVC_ROWS) {
		ADD((menu_row){ "SSH",   w->svc[WIFI_SSH]   ? "on" : "off", true });
		ADD((menu_row){ "Samba", w->svc[WIFI_SAMBA] ? "on" : "off", true });
		/* On, it reads as where to manage it: the web UI, signed into as
		 * root with ROCKNIX's root password. */
		ADD((menu_row){ "Syncthing", !w->svc[WIFI_SYNCTHING] ? "off"
		                : w->st_ui[0] ? w->st_ui : "on", true });
	}
	ADD((menu_row){ "Over The Hare",
	                            w->connected ? NULL : "needs Wi-Fi", w->connected });
	for (i = 0; i < w->n && nrows < max - 3; i++)
		ADD((menu_row){ w->nets[i].ssid, w->vals[i], true });

	/* These are footers, not list items - they say something about the list
	 * rather than offering anything - so they sit under a rule like the key
	 * legend does. */
	if (!w->on) {
		ADD(MENU_RULE);
		ADD(MENU_NOTE("Turn Wi-Fi on to scan"));
		return nrows;
	}
	if (w->n == 0 && !w->scanning) {
		ADD(MENU_RULE);
		ADD(MENU_NOTE("No networks found"));
		return nrows;
	}
	for (i = 0; i < w->n; i++)
		if (w->nets[i].known) { any_saved = true; break; }
	ADD(MENU_RULE);
	/* Where the device is on the network: what Over The Hare, SSH and a file
	 * share need typed into the other machine. */
	if (w->conn[0]) ADD(MENU_NOTE(w->conn));
	/* What is happening, then what you can do. The footer is the honest place
	 * for progress: it is already outside the list, and swapping its text
	 * means the screen never has to put a modal in front of you. */
	ADD(w->scanning
	              ? MENU_NOTE("Scanning...")
	              : MENU_NOTE(any_saved ? "Y: rescan   X: forget" : "Y: rescan"));
	return nrows;
}

