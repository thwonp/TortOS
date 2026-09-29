/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef TORTOS_IDLE_H
#define TORTOS_IDLE_H

#include <stdbool.h>

/* Auto Off's clock, and only the clock.
 *
 * It lives in its own file because it has broken five times, and every break
 * was invisible until somebody set the shortest timeout and watched a device
 * for half a minute. Four of the five were wiring - a screen that never asked,
 * a message sent from the wrong branch - and no unit test would have caught
 * those. The fifth was this arithmetic, and it is the reason the charger case
 * shipped wrong: plugged in, the countdown ran to zero anyway and unplugging
 * fired it instantly.
 *
 * So: policy here, where it can be checked without a device (see
 * tools/idle-check.c). Gathering the three inputs stays in the launcher, which
 * is the part that needs a screen and a battery.
 */

typedef struct {
	int      seconds;    /* the setting; 0 or less means never */
	unsigned since_ms;   /* when the current countdown started */
	unsigned last_ms;    /* when idle_check was last called */
	bool     started;    /* has it been called at all? */
} idle_clock;

/* True when the device should switch itself off.
 *
 * `input` is any button at all, held or tapped, including ones the current
 * screen ignores - somebody mashing a key with no function is still somebody
 * there. `charging` HOLDS the clock rather than blocking the answer, so
 * unplugging starts a whole fresh countdown; gating only the answer is what
 * made an hour on the cable power the device off the moment it came out.
 *
 * A gap between calls longer than IDLE_GAP_MS restarts the countdown. Callers
 * are frame loops running at roughly 120 Hz, so a gap means nobody was reading
 * the pad in between - a game running, a Wi-Fi scan, a request - and time
 * nobody was watching is not evidence that the player left. This is what
 * stops the feature from being a list of places to remember: it holds even
 * for a screen that has not been written yet.
 *
 * All times are milliseconds from any monotonic source; comparisons are
 * unsigned differences, so the source may wrap.
 */
#define IDLE_GAP_MS 1000u

bool idle_check(idle_clock *c, unsigned now_ms, bool input, bool charging);

#endif
