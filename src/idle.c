/* SPDX-License-Identifier: MIT */
/* See idle.h for why this is a file of its own. */
#include "idle.h"

bool idle_check(idle_clock *c, unsigned now_ms, bool input, bool charging)
{
	if (c->seconds <= 0) {
		/* Still stamped. Turning Auto Off back on after an hour switched off
		 * would otherwise read as an hour-long gap, which restarts the
		 * countdown - harmless, but only by accident, and a reader should not
		 * have to work that out. */
		c->started = true;
		c->last_ms = now_ms;
		return false;
	}

	/* Before anything else: was anybody even asking?
	 *
	 * `started` rather than a zero last_ms. Zero is a time like any other -
	 * the one the counter takes as it wraps - and using it as "never called"
	 * made the tick after the wrap restart the countdown. Caught by the wrap
	 * case in tools/idle-check.c on the first run of that check. */
	if (!c->started || now_ms - c->last_ms > IDLE_GAP_MS)
		c->since_ms = now_ms;
	c->started = true;
	c->last_ms = now_ms;

	if (input || charging) {
		c->since_ms = now_ms;
		return false;
	}
	return now_ms - c->since_ms > (unsigned)c->seconds * 1000u;
}
