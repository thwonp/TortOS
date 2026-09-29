/* The Wi-Fi screen's state and its row building, with no SDL and no renderer.
 *
 * Split from main.c so `make check` can ask what the menu contains for a given
 * state - the requirement ADR-0001 rests on. The half that needs a screen
 * (wifi_key, wifi_screen) stays in main.c; this half is answerable offline.
 */
#ifndef TORTOS_WIFI_MENU_H
#define TORTOS_WIFI_MENU_H

#include "menu.h"
#include "wifi.h"

typedef struct {
	wifi_net nets[WIFI_MAX_NETS];
	char     vals[WIFI_MAX_NETS][32];
	int      n;
	bool     on;
	bool     scanning;   /* a scan is running; the list is provisional */
	bool     scanned;    /* a scan has finished at least once this visit */
	char     ip[80];     /* "IP a.b.c.d" while connected, else empty */
	bool     svc[WIFI_NSVC];   /* SSH, Samba; read on entry, only where WIFI_SVC_ROWS */
} wifi_ui;

/* Signal as a word. dBm is the honest number and it is also jargon; the list is
 * sorted strongest first anyway, so the word only has to separate "this will
 * work" from "this will not".
 *
 * The -72 boundary is NOT validated. Measured throughput exists at two points
 * only - unusable at -73, fine at -58 - and the cliff is somewhere in that 15
 * dB. Left where it is rather than moved on a hunch; see BACKLOG. */
const char *wifi_strength(int dbm);

void wifi_label(wifi_ui *w);
void wifi_begin_scan(wifi_ui *w);
/* Fills `rows` and names the screen. The heading is a function of the same
 * state the rows are, so it is produced here rather than being handed to the
 * runner separately and then having to be kept in step with what is below it. */
int  wifi_build(void *ctx, menu_row *rows, int max, const char **heading);

#endif
