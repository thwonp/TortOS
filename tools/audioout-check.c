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
	s.policy = p; s.wired = wired; s.bt_sink = bt; s.usb_sink = NULL; s.codec = NULL;
	return s;
}

#define USB "plughw:CARD=KA13,DEV=0"

static aout_state su(aout_policy p, bool wired, const char *bt, const char *usb)
{
	aout_state s = st(p, wired, bt);
	s.usb_sink = usb;
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
	int p, w, b, u, n = 0;

	printf("all sixteen combinations resolve, in the order usb > wired > bt > speaker:\n");
	for (p = 0; p < AOUT_POLICY_COUNT; p++)
		for (w = 0; w < 2; w++)
			for (b = 0; b < 2; b++)
				for (u = 0; u < 2; u++) {
					aout_state s = su((aout_policy)p, w != 0, b ? BT : NULL,
					                  u ? USB : NULL);
					aout_dest  d = aout_resolve(&s);

					n++;
					ck(d == AOUT_SPK || d == AOUT_WIRED || d == AOUT_BT ||
					   d == AOUT_USB, "resolves to a real destination");
					if (u && p == AOUT_AUTO) ck(d == AOUT_USB, "a DAC on Auto always wins");
					else if (w) ck(d == AOUT_WIRED, "otherwise wired wins");
					if (d == AOUT_BT) ck(!w && !u && b && p == AOUT_AUTO,
					                     "bluetooth only with no cable or DAC, on Auto");
					if (d == AOUT_USB) ck(u && p == AOUT_AUTO, "a DAC only on Auto");
					ck(aout_dest_of(&s, aout_device(&s)) == d,
					   "the device string reads back as the same destination");
				}
	ck(n == 16, "sixteen combinations were actually tried");
}

/* The DAC: above the jack and the headset, refused by Speaker like a headset. */
static void usb_dac(void)
{
	aout_state s;

	printf("a USB DAC:\n");
	s = su(AOUT_AUTO, true, BT, USB);
	ck(aout_resolve(&s) == AOUT_USB, "beats a cable and a headset together");
	ck(!strcmp(aout_device(&s), USB), "and sends its own device string");
	s = su(AOUT_SPEAKER, false, BT, USB);
	ck(aout_resolve(&s) == AOUT_SPK, "Speaker refuses it");
	s = su(AOUT_SPEAKER, true, NULL, USB);
	ck(aout_resolve(&s) == AOUT_WIRED, "but Speaker still hears the jack");
	s = su(AOUT_AUTO, false, BT, "");
	ck(aout_resolve(&s) == AOUT_BT, "an unplugged DAC (\"\") is not a destination");
	s = su(AOUT_AUTO, false, BT, USB);
	ck(aout_dest_of(&s, BT) == AOUT_BT, "a player on the headset reads as bluetooth");
	ck(aout_dest_of(&s, "") == AOUT_SPK, "and one on the codec as the speaker");
}

/* The Bricks: with a DAC in, "default" IS the DAC, so the codec is named. */
static void named_codec(void)
{
	aout_state s;

	printf("the codec, named:\n");
	s = su(AOUT_SPEAKER, false, BT, USB);
	s.codec = "Playback";
	ck(!strcmp(aout_device(&s), "Playback"), "Speaker with a DAC in sends the codec by name");
	ck(aout_dest_of(&s, "Playback") == AOUT_SPK, "which reads back as the speaker");
	ck(!aout_named(&s), "and is not a sink to hand over");
	s.wired = true;
	ck(aout_dest_of(&s, "Playback") == AOUT_WIRED, "or the jack, with a cable in");
	s = su(AOUT_AUTO, false, BT, USB);
	s.codec = "Playback";
	ck(aout_named(&s) && !strcmp(aout_device(&s), USB), "Auto still sends the DAC");
	s = st(AOUT_AUTO, false, NULL);
	ck(!strcmp(aout_device(&s), "") && !aout_named(&s), "no name, no DAC: the default, as before");
}

/* /proc/asound/cards as each device printed it, 2026-10-07, plus DACs that
 * are not the KA13 - the parse must not know any one DAC. */
static void cards(void)
{
	const char *sp =
		" 0 [audiocodec     ]: audiocodec - audiocodec\n"
		"                      audiocodec\n"
		" 1 [ahubdam        ]: ahubdam - ahubdam\n"
		"                      ahubdam\n"
		" 2 [ahubhdmi       ]: ahubhdmi - ahubhdmi\n"
		"                      ahubhdmi\n"
		" 3 [KA13           ]: USB-Audio - FIIO KA13\n"
		"                      FIIO FIIO KA13 at usb-sunxi-ehci-1, high speed\n";
	const char *brick =
		" 0 [audiocodec     ]: audiocodec - audiocodec\n"
		"                      audiocodec\n";
	const char *other =
		" 0 [rockchipes9018 ]: rockchip-es9018 - rockchip-es9018\n"
		"                      rockchip-es9018\n"
		" 1 [Dongle_X2      ]: USB-Audio - Some Dongle X2\n"
		"                      Vendor Some Dongle X2 at usb-xhci-hcd.5.auto-1, full speed\n"
		" 2 [KA13           ]: USB-Audio - FIIO KA13\n"
		"                      FIIO FIIO KA13 at usb-xhci-hcd.5.auto-2, high speed\n";
	/* A description line that happens to mention USB-Audio is not a card. */
	const char *tricky =
		" 0 [audiocodec     ]: audiocodec - audiocodec\n"
		"                      1 [x]: USB-Audio lookalike\n";
	const char *longid =
		" 4 [ABCDEFGHIJKLMNOP]: USB-Audio - Long\n"
		"                      Long\n";
	/* The RG Nano's own, read 2026-10-08 (plorpos-ggv.8), and the same with
	 * a DAC as card 1: its kernel has USB audio (FunKey rg_nano branch). */
	const char *nano =
		" 0 [Codec          ]: V3s_Audio_Codec - V3s Audio Codec\n"
		"                      V3s Audio Codec\n";
	const char *nano_dac =
		" 0 [Codec          ]: V3s_Audio_Codec - V3s Audio Codec\n"
		"                      V3s Audio Codec\n"
		" 1 [KA13           ]: USB-Audio - FIIO KA13\n"
		"                      FIIO FIIO KA13 at usb-musb-hdrc.1.auto-1, high speed\n";
	char dev[64];
	int  card = -1;

	printf("finding the DAC in /proc/asound/cards:\n");
	ck(!aout_usb_card(nano, dev, sizeof dev, &card) && dev[0] == '\0',
	   "the RG Nano's codec alone is no DAC");
	ck(aout_usb_card(nano_dac, dev, sizeof dev, &card) &&
	   !strcmp(dev, "plughw:CARD=KA13,DEV=0") && card == 1,
	   "a DAC on the RG Nano, as card 1");
	ck(aout_usb_card(sp, dev, sizeof dev, &card) && !strcmp(dev, USB) && card == 3,
	   "the RG SP's, as card 3 by its id");
	ck(!aout_usb_card(brick, dev, sizeof dev, &card) && dev[0] == '\0',
	   "a Brick with nothing plugged in has none");
	ck(aout_usb_card(other, dev, sizeof dev, &card) &&
	   !strcmp(dev, "plughw:CARD=Dongle_X2,DEV=0") && card == 1,
	   "any DAC, by whatever id the kernel gave it - the first one");
	ck(!aout_usb_card(tricky, dev, sizeof dev, &card), "a description line is not a card");
	ck(aout_usb_card(longid, dev, sizeof dev, &card) &&
	   !strcmp(dev, "plughw:CARD=ABCDEFGHIJKLMNOP,DEV=0") && card == 4,
	   "an id that fills the whole column");
	ck(!aout_usb_card(sp, dev, 10, &card) && dev[0] == '\0',
	   "a buffer too small refuses rather than truncating");
	ck(!aout_usb_card("", dev, sizeof dev, NULL), "empty text has none");
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
	ck(!strcmp(aout_dest_name(AOUT_USB), "USB DAC"), "a DAC reads as USB DAC");
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
	usb_dac();
	named_codec();
	cards();
	names();
	reapply_rule();
	the_measured_case();
	ladder();
	if (fails) { printf("\n%d audio output check(s) failed\n", fails); return 1; }
	printf("\nok: sound goes where it should\n");
	return 0;
}
