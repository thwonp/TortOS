/* SPDX-License-Identifier: MIT */
/* Where does the sound go, given what is plugged in and what was asked for?
 *
 * Eight combinations of three facts, and the whole rule is five lines - which
 * is exactly the kind of thing that is obviously right until a headset connects
 * during a game and it is not. It is a pure function precisely so this can
 * enumerate every case rather than sample the ones somebody thought of.
 *
 * Links src/audioout.c and NOT SDL. If it ever needs SDL, the split has failed.
 */
#include "../src/audioout.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void ck(int cond, const char *what)
{
	if (!cond) { printf("  FAIL %s\n", what); fails++; }
}

#define BT "bluealsa:DEV=AA:BB:CC:DD:EE:FF,PROFILE=a2dp"

static aout_state st(aout_policy p, bool wired, const char *bt)
{
	aout_state s;
	s.policy = p; s.wired = wired; s.bt_sink = bt;
	return s;
}

/* The rule the whole feature is: wired > bluetooth > speaker. */
static void priority(void)
{
	aout_state s;

	printf("wired beats bluetooth beats speaker:\n");

	s = st(AOUT_AUTO, true, BT);
	ck(aout_resolve(&s) == AOUT_WIRED, "a cable wins over a connected headset");
	s = st(AOUT_AUTO, false, BT);
	ck(aout_resolve(&s) == AOUT_BT, "the headset wins over the speaker");
	s = st(AOUT_AUTO, false, NULL);
	ck(aout_resolve(&s) == AOUT_SPK, "with neither, the speaker");

	/* The cable wins in EVERY policy, which is the part a setting could
	 * plausibly have been allowed to override and must not. */
	s = st(AOUT_SPEAKER, true, BT);
	ck(aout_resolve(&s) == AOUT_WIRED, "a cable wins even when pinned to Speaker");
}

/* Speaker means "never Bluetooth", and nothing else. */
static void the_override(void)
{
	aout_state s;

	printf("the Speaker override:\n");

	s = st(AOUT_SPEAKER, false, BT);
	ck(aout_resolve(&s) == AOUT_SPK, "refuses a connected headset");
	ck(!strcmp(aout_device(&s), ""), "and sends the codec, not the sink");

	s = st(AOUT_AUTO, false, BT);
	ck(!strcmp(aout_device(&s), BT), "Auto sends the sink's own device string");
}

/* Two destinations share one device string, and that is not a bug: the jack
 * switch that separates them is in the codec, below ALSA. */
static void device_strings(void)
{
	aout_state w = st(AOUT_AUTO, true, NULL);
	aout_state k = st(AOUT_AUTO, false, NULL);
	aout_state b = st(AOUT_AUTO, false, BT);

	printf("device strings:\n");
	ck(!strcmp(aout_device(&w), ""), "wired is the default device");
	ck(!strcmp(aout_device(&k), ""), "so is the speaker");
	ck(aout_resolve(&w) != aout_resolve(&k),
	   "but they are still told apart, for the menu to name");
	ck(aout_device(&b)[0] != '\0', "bluetooth is not the default device");
	ck(aout_device(&w) != NULL && aout_device(&b) != NULL,
	   "never NULL, so a caller can always print it");
}

/* An empty sink string is what a disconnect leaves behind, and must read as
 * "no headset" rather than as a device named "". */
static void empty_sink(void)
{
	aout_state s = st(AOUT_AUTO, false, "");

	printf("a sink that went away:\n");
	ck(aout_resolve(&s) == AOUT_SPK, "an empty sink is not a destination");
	ck(!strcmp(aout_device(&s), ""), "and resolves to the codec");
}

/* Every combination, so nothing is left to a case nobody thought of. */
static void every_case(void)
{
	int p, w, b, n = 0;

	printf("all eight combinations resolve, and only wired outranks a sink:\n");
	for (p = 0; p < AOUT_POLICY_COUNT; p++)
		for (w = 0; w < 2; w++)
			for (b = 0; b < 2; b++) {
				aout_state s = st((aout_policy)p, w != 0, b ? BT : NULL);
				aout_dest  d = aout_resolve(&s);

				n++;
				ck(d == AOUT_SPK || d == AOUT_WIRED || d == AOUT_BT,
				   "resolves to a real destination");
				if (w) ck(d == AOUT_WIRED, "wired always wins");
				if (d == AOUT_BT) ck(!w && b && p == AOUT_AUTO,
				                     "bluetooth only unplugged, connected, on Auto");
			}
	ck(n == 8, "eight combinations were actually tried");
}

static void names(void)
{
	printf("what the menu says:\n");
	ck(!strcmp(aout_policy_name(AOUT_AUTO), "Auto"), "Auto");
	ck(!strcmp(aout_policy_name(AOUT_SPEAKER), "Speaker"), "Speaker");
	ck(strcmp(aout_dest_name(AOUT_WIRED), aout_dest_name(AOUT_SPK)) != 0,
	   "wired and speaker read differently");
	ck(strcmp(aout_dest_name(AOUT_BT), aout_dest_name(AOUT_SPK)) != 0,
	   "so do bluetooth and speaker");
}


/* ---- the jack, and the handover that made this invisible ----------------- */

/* The two windows, from src/platform.c. Duplicated here on purpose: if someone
 * retunes the ladder and does not touch this file, the numbers below stop
 * describing the device and the mismatch is the point of noticing. */
#define SPK_TOP 0
#define SPK_BOT 39
#define HP_TOP  8
#define HP_BOT  61
#define VMAX    20

/* The defect, stated as a rule. Every one of these was true before the fix
 * except the first, and the first is the whole bug. */
static void reapply_rule(void)
{
	printf("when the volume must be written again:\n");

	ck(aout_should_reapply(AOUT_JACK_UNKNOWN, false, true),
	   "after a handover, yes - even with no transition to point at");
	ck(aout_should_reapply(AOUT_JACK_UNKNOWN, true, true),
	   "after a handover with a cable in, the same");

	ck(aout_should_reapply(0, true, true), "out then in");
	ck(aout_should_reapply(1, false, true), "in then out");

	ck(!aout_should_reapply(0, false, true), "unchanged, out: no");
	ck(!aout_should_reapply(1, true, true), "unchanged, in: no");

	ck(!aout_should_reapply(AOUT_JACK_UNKNOWN, false, false),
	   "with no level known there is nothing to write");
}

/* The failure that was audible: a headphone value left in a speaker window.
 *
 * These are the exact numbers measured on the device on 2026-09-05, and they
 * are why "silent" was the symptom rather than "slightly wrong". */
static void the_measured_case(void)
{
	int hp12 = aout_level_to_raw(12, VMAX, HP_TOP, HP_BOT);
	int spk12 = aout_level_to_raw(12, VMAX, SPK_TOP, SPK_BOT);

	printf("the case that was heard as a dead speaker:\n");
	ck(hp12 == 29, "level 12 on headphones is raw 29");
	ck(spk12 == 16, "the same level on the speaker is raw 16");
	/* 29 against a window whose quiet end is 39 is nearly all the way down,
	 * which is why it read as broken rather than as quiet. */
	ck(hp12 > (SPK_BOT * 2) / 3,
	   "so leaving 29 in the speaker window is near its quiet end");
}

/* The mapping itself: inverted, clamped, and different per window. */
static void ladder(void)
{
	printf("level to raw:\n");

	ck(aout_level_to_raw(VMAX, VMAX, SPK_TOP, SPK_BOT) == SPK_TOP,
	   "full volume is the top of the window");
	ck(aout_level_to_raw(0, VMAX, SPK_TOP, SPK_BOT) == SPK_BOT,
	   "zero is the bottom");
	ck(aout_level_to_raw(VMAX, VMAX, HP_TOP, HP_BOT) == HP_TOP,
	   "and the headphone window has its own top");
	ck(aout_level_to_raw(0, VMAX, HP_TOP, HP_BOT) == HP_BOT,
	   "and its own bottom");

	/* Inverted: the control is attenuation, so louder is a SMALLER number. */
	ck(aout_level_to_raw(15, VMAX, SPK_TOP, SPK_BOT) <
	   aout_level_to_raw(5, VMAX, SPK_TOP, SPK_BOT),
	   "louder is a smaller register value");

	/* Monotonic, with no rung repeating a raw it should not. */
	{
		int i, prev = aout_level_to_raw(0, VMAX, SPK_TOP, SPK_BOT);
		int ok = 1;
		for (i = 1; i <= VMAX; i++) {
			int r = aout_level_to_raw(i, VMAX, SPK_TOP, SPK_BOT);
			if (r > prev) ok = 0;
			prev = r;
		}
		ck(ok, "every rung up is at least as loud as the one below");
	}

	/* Out of range cannot escape the window. Above the top would be louder
	 * than the ladder was ever calibrated against, which on headphones is the
	 * difference between loud and painful. */
	ck(aout_level_to_raw(999, VMAX, HP_TOP, HP_BOT) == HP_TOP,
	   "over the top clamps");
	ck(aout_level_to_raw(-5, VMAX, HP_TOP, HP_BOT) == HP_BOT,
	   "under the bottom clamps");
	ck(aout_level_to_raw(10, 0, SPK_TOP, SPK_BOT) == SPK_TOP,
	   "a zero-length ladder does not divide");
}

int main(void)
{
	priority();
	the_override();
	device_strings();
	empty_sink();
	every_case();
	names();
	reapply_rule();
	the_measured_case();
	ladder();
	if (fails) { printf("\n%d audio output check(s) failed\n", fails); return 1; }
	printf("\nok: sound goes where it should\n");
	return 0;
}
