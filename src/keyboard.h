/* SPDX-License-Identifier: MIT */
#ifndef TORTOS_KEYBOARD_H
#define TORTOS_KEYBOARD_H

#include <SDL.h>
#include <stdbool.h>

#include "platform.h"

/* On-screen text entry.
 *
 * General on purpose. WiFi needs a network password, and the moment WiFi
 * works RetroAchievements needs a username and a password too, so this takes
 * a title and a buffer and returns a string rather than knowing anything
 * about what it is being typed into. A keyboard welded into the WiFi flow
 * gets rewritten the first time something else needs to type.
 *
 * Modal and blocking, in the same shape as the menus: poll, act, draw,
 * present, delay. The caller supplies a backdrop callback because this module
 * knows nothing about the shelf, and something has to be drawn behind a panel
 * that does not cover the screen.
 */

typedef enum {
	KB_CANCEL,   /* MENU, or B held on an empty field */
	KB_ACCEPT,   /* START */
	KB_POWER     /* the power button, handed back for the caller to act on */
} kb_result;

/* Drawn behind the keyboard panel every frame. May be NULL for a bare
 * background. */
typedef void (*kb_backdrop)(void *ctx);

/* Asked every frame, after input is polled: must the device power off now?
 * The caller's whole power policy - the button's tap and hold, the idle
 * clock, sleeping in between - and true is reported as KB_POWER, which every
 * caller already knows what to do with. A callback rather than a timeout
 * value, for the same reason as the backdrop: this module is general and
 * holds no policy. NULL: any power press is KB_POWER. */
typedef bool (*kb_power)(void *ctx);

/* `buf` is both the seed and the result: pass an empty string for a fresh
 * entry, or existing text to edit. `cap` is the size of buf including the
 * terminator. `accent` is 0xRRGGBB for the panel edge and the cursor.
 *
 * The text is always shown in clear. A password typed a character at a time
 * on a d-pad is going to be typed wrong, and a masked field turns that into a
 * failed association with no way to tell a typo from a weak signal or a
 * supplicant that never came up. Hiding it would protect against a shoulder,
 * at the cost of the one thing the user needs to see. */
kb_result kb_prompt(SDL_Renderer *r, in_state *in, const char *title,
                    char *buf, int cap, unsigned accent,
                    kb_backdrop backdrop, kb_power power, void *ctx);

/* One frame, no loop, for the --keyboard shot harness. The panel is dense
 * enough that laying it out against a screenshot beats laying it out against
 * a device, and the same argument already justifies --menu and --slots.
 * `layer` is 0 lower, 1 upper, 2 symbols. */
void kb_preview(SDL_Renderer *r, const char *title, const char *text,
                int layer, int row, int col, unsigned accent);

#endif
