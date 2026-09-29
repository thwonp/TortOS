/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The Bluetooth screen, minus the screen.
 *
 * State and row building only. This file must never include SDL: ADR-0001 says
 * a build function has to be callable with no renderer and no device, and
 * tools/menu-check.c links it directly to prove it. The first version of this
 * screen built its rows inside main.c, which meant nothing could check them.
 *
 * The half that needs a screen - bt_screen - stays in main.c, the same split
 * wifi_menu.h describes.
 */
#ifndef TORTOS_BT_MENU_H
#define TORTOS_BT_MENU_H

#include "bt.h"
#include "menu.h"

typedef struct {
	bt_device dev[BT_MAX];
	int       n;
	int       cursor;      /* index into dev, not into the built rows */
	int       top;         /* first device shown, for a list that scrolls */
	bt_state  state;
	bool      scanning;
	char      note[96];    /* what just happened; empty for the key legend */
	char      vals[BT_MAX][32];
} bt_ui;

/* Rows are: the on/off toggle, the devices, a rule, then a footer. `max` is
 * the size of `out`; the device list is truncated rather than overrunning it. */
int bt_menu_build(const bt_ui *u, menu_row *out, int max, int visible);

/* The footer text for the current cursor: what just happened if anything did,
 * otherwise what the buttons do HERE. A pairs a device that is only in range
 * and connects one already bonded, and X has nothing to forget until it is -
 * so a fixed legend would be wrong two rows out of three. */
const char *bt_menu_footer(const bt_ui *u);

#endif
