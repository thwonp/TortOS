/* SPDX-License-Identifier: MIT */
/* Does Auto Off's clock behave?
 *
 *     make check-idle
 *
 * This feature has broken five times. Four were wiring - a screen that never
 * asked, a message sent from a branch a game without achievements never takes
 * - and nothing here would have caught those; `grep -n idle_due src/main.c` is
 * the check for those, and it should list every loop that draws.
 *
 * The fifth was this arithmetic, and it shipped: plugged in, the countdown ran
 * to zero anyway, and unplugging powered the device off in that instant. That
 * one is checkable, so it is checked, along with the gap rule that was added
 * to stop the wiring bugs recurring in screens nobody has written yet.
 *
 * No SDL, no device, no battery. Time is a number this file chooses.
 */
#include <stdio.h>

#include "../src/idle.h"

static int failures;

#define CHECK(cond, ...)                                                      \
	do {                                                                      \
		if (!(cond)) {                                                        \
			printf("  FAIL: ");                                               \
			printf(__VA_ARGS__);                                              \
			printf("\n        at %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
			failures++;                                                       \
		}                                                                     \
	} while (0)

/* A frame loop, run forward. The launcher's screens poll at about 120 Hz, so
 * 8 ms a step is what the real caller does. True if it fired, with when in
 * `*at` if `at` is given.
 *
 * The obvious `for (t = from; t < from + ms; t += 8)` is wrong here and was
 * written that way first: near the wrap, `from + ms` lands BELOW `from`, the
 * condition is false on the first test, and the loop never runs at all. It
 * reported "never fired across the wrap", which reads exactly like the bug
 * this case exists to catch. Count the elapsed time instead and let only `t`
 * wrap - which is the same discipline idle_check itself follows. */
static bool run(idle_clock *c, unsigned from, unsigned ms,
                bool input, bool charging, unsigned *at)
{
	unsigned t = from, elapsed;

	for (elapsed = 0; elapsed < ms; elapsed += 8, t += 8)
		if (idle_check(c, t, input, charging)) {
			if (at) *at = t;
			return true;
		}
	return false;
}

int main(void)
{
	printf("idle: Auto Off's clock\n");

	printf("  it fires, once the timeout has passed:\n");
	{
		idle_clock c = { .seconds = 30 };
		unsigned at;

		CHECK(!idle_check(&c, 1000, false, false), "fired on the first call");
		CHECK(!run(&c, 1008, 29000, false, false, NULL),
		      "fired before the timeout was up");
		CHECK(run(&c, 30008, 5000, false, false, &at), "never fired");
		CHECK(at >= 31000 && at <= 31100,
		      "fired at %u ms, expected about 31000", at);
	}

	printf("  any button at all resets it:\n");
	{
		idle_clock c = { .seconds = 30 };

		run(&c, 1000, 25000, false, false, NULL);   /* most of the way */
		CHECK(idle_check(&c, 26000, true, false) == false,
		      "a press was not enough to reset it");
		CHECK(!run(&c, 26008, 29000, false, false, NULL),
		      "the press did not buy a whole fresh countdown");
		CHECK(run(&c, 55008, 3000, false, false, NULL),
		      "never fired after the press");
	}

	/* The reported bug, 2026-08-30. The charger used to gate the ANSWER, so
	 * the countdown ran to zero on the cable and sat there expired; unplugging
	 * powered the device off in the same instant. */
	printf("  the charger holds the clock, and does not merely veto it:\n");
	{
		idle_clock c = { .seconds = 30 };

		CHECK(!run(&c, 1000, 600000, false, true, NULL),
		      "fired while charging");
		/* Unplugged after ten minutes on the cable. */
		CHECK(!run(&c, 601000, 29000, false, false, NULL),
		      "unplugging fired it immediately - the clock had been running "
		      "the whole time it was charging");
		CHECK(run(&c, 630000, 3000, false, false, NULL),
		      "never fired after unplugging");
	}

	/* The rule that keeps the wiring bugs from coming back in screens that do
	 * not exist yet: a stretch when nobody asked is not a stretch when nobody
	 * was there. A game, a Wi-Fi scan, a request. */
	printf("  a gap in the asking restarts it:\n");
	{
		idle_clock c = { .seconds = 30 };

		CHECK(!idle_check(&c, 1000, false, false), "fired on the first call");
		/* An hour inside a game, where Diatom owns the pad and none of this
		 * runs. The in-game menu is the next thing to ask. */
		CHECK(!idle_check(&c, 3601000, false, false),
		      "the menu opened after an hour of play and fired at once");
		CHECK(!run(&c, 3601008, 29000, false, false, NULL),
		      "the gap did not buy a whole fresh countdown");
		CHECK(run(&c, 3630008, 3000, false, false, NULL),
		      "never fired after the gap");
	}

	printf("  and a gap shorter than one frame budget does not:\n");
	{
		idle_clock c = { .seconds = 30 };

		run(&c, 1000, 29000, false, false, NULL);
		/* A slow frame - a card decode, a texture upload - must not be read
		 * as the launcher having stopped watching. */
		CHECK(idle_check(&c, 30500, false, false) == false ||
		      IDLE_GAP_MS < 500, "a 500 ms frame counted as a gap");
		CHECK(run(&c, 30508, 2000, false, false, NULL),
		      "a slow frame reset a countdown that was nearly up");
	}

	printf("  off means off:\n");
	{
		idle_clock c = { .seconds = 0 };

		CHECK(!run(&c, 1000, 3600000, false, false, NULL),
		      "fired with Auto Off disabled");
	}

	/* plat_now_ms is unsigned and wraps every 49 days. Nothing on this device
	 * stays up that long, but the comparison costs nothing to get right and
	 * the alternative is a deadline compare that silently stops working. */
	printf("  it survives the millisecond counter wrapping:\n");
	{
		idle_clock c = { .seconds = 30 };
		unsigned near = 0xFFFFF000u;

		CHECK(!idle_check(&c, near, false, false), "fired on the first call");
		CHECK(!run(&c, near + 8, 3000, false, false, NULL),
		      "fired early across the wrap");
		/* Straight through zero: `t` wraps inside run, which is the point. */
		CHECK(run(&c, near + 3008, 29000, false, false, NULL),
		      "never fired across the wrap");
	}

	if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
	printf("\nok: every check passed\n");
	return 0;
}
