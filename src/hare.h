/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef TORTOS_HARE_H
#define TORTOS_HARE_H

#include <stdbool.h>
#include <stddef.h>

#include "httpd.h"

/* Over The Hare: the routes, the PIN, and the file operations.
 *
 * httpd.c moves bytes and knows nothing about what they mean. xfer.c decides
 * what a path is allowed to be. This is the part in between: what a URL does,
 * who is allowed to do it, and what happens to the card when they do.
 *
 * THE PIN IS FOUR DIGITS, which is ten thousand guesses. On its own that is a
 * doorbell rather than a lock - a script exhausts it in seconds. What makes it
 * a lock is the lockout: a handful of wrong answers and the door stops
 * answering for a while, which turns ten thousand guesses into more hours than
 * anyone will spend on somebody else's ROM folder. The PIN is generated fresh
 * every time the screen opens and dies with it, so there is nothing to reuse
 * and nothing to leak.
 *
 * The threat this defends against is someone else on the same Wi-Fi, which is
 * the realistic one for a handheld on a home or cafe network. It is not
 * defense against someone who can watch the traffic: this is plain HTTP, the
 * PIN crosses the network in the clear, and so does everything else. TLS on a
 * LAN address is a certificate nobody can issue and a warning everybody clicks
 * through, which buys less than it costs. The mitigation that actually applies
 * is that the server exists only while the screen is open.
 */

/* Start and stop with the screen. Generates the PIN, clears any session, and
 * sweeps part-files left by a transfer that died.
 *
 * The three directories are arguments rather than the P_ globals so this whole
 * subsystem - routes, transport, path safety - links into a check without SDL
 * behind it. The launcher passes P_ROMS, P_CARD and its own res/web. */
bool hare_start(const char *roms_dir, const char *card_dir,
                const char *shared_dir, const char *web_dir);
void hare_stop(void);

/* What the screen puts on the panel. `ip` is the LAN address, from wifi. */
const char *hare_pin(void);
int         hare_port(void);

/* Drive it from the screen's frame loop. Returns non-zero when anything moved,
 * which is what keeps Auto Off from powering the device off mid-upload. */
int hare_poll(void);

/* For the screen: what is going on, in words a person can read. */
typedef struct {
	int           clients;      /* browsers currently connected */
	unsigned long in, out;      /* bytes since the last call */
	int           uploads;      /* transfers in flight */
	char          last[128];    /* the most recent thing that happened */
} hare_stats;

void hare_status(hare_stats *out);

/* Whether anything under the ROM or Music root was written, renamed or deleted
 * since the screen opened. The shelf is scanned once at startup and nothing
 * watches the card, so a ROM or an album arriving over the network is
 * invisible until something rescans - which is what the screen uses this to
 * decide on the way out. */
bool hare_shelf_changed(void);

#endif
