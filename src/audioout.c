/* SPDX-License-Identifier: MIT */
/* See src/audioout.h for why this is a file of its own. */
#include "audioout.h"

#include <stdio.h>
#include <string.h>

aout_dest aout_resolve(const aout_state *s)
{
	/* A cable wins outright, in every policy. Someone who physically plugged
	 * something in has said what they want more plainly than any setting, and
	 * a headset that is merely connected has not said anything at all. A DAC
	 * is the same act at the other port, and the later one, so it comes first
	 * - except when pinned to Speaker, which refuses everything software
	 * routes. */
	if (s->policy == AOUT_AUTO && s->usb_sink && s->usb_sink[0]) return AOUT_USB;
	if (s->wired) return AOUT_WIRED;
	if (s->policy == AOUT_AUTO && s->bt_sink && s->bt_sink[0]) return AOUT_BT;
	return AOUT_SPK;
}

const char *aout_device(const aout_state *s)
{
	/* "" is the port's default device, which on this hardware is the codec
	 * through dmix - and therefore both the speaker AND the wired jack. */
	switch (aout_resolve(s)) {
	case AOUT_USB: return s->usb_sink;
	case AOUT_BT:  return s->bt_sink;
	default:       return "";
	}
}

aout_dest aout_dest_of(const aout_state *s, const char *dev)
{
	if (!dev || !dev[0]) return s->wired ? AOUT_WIRED : AOUT_SPK;
	if (s->usb_sink && s->usb_sink[0] && !strcmp(dev, s->usb_sink)) return AOUT_USB;
	return AOUT_BT;
}

bool aout_usb_card(const char *cards, char *dev, int cap, int *card)
{
	const char *l;

	if (cap > 0) dev[0] = '\0';
	/* Each card is two lines, and the first reads
	 *     " 3 [KA13           ]: USB-Audio - FIIO KA13"
	 * - index, id padded to fifteen, then the driver. snd-usb-audio names
	 * itself USB-Audio for every device it binds, which is what makes this
	 * any DAC rather than one. */
	for (l = cards; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
		const char *p = l, *id, *end;
		int idx = 0, len;

		/* The index is printed "%2i", so a card's line has it in the first two
		 * columns; the description under it is indented past them. */
		while (*p == ' ' && p - l < 2) p++;
		if (*p < '0' || *p > '9') continue;
		while (*p >= '0' && *p <= '9') idx = idx * 10 + (*p++ - '0');
		while (*p == ' ') p++;
		if (*p++ != '[') continue;
		id = p;
		if (!(end = strchr(id, ']'))) continue;
		p = end + 1;
		while (end > id && end[-1] == ' ') end--;
		len = (int)(end - id);
		if (len <= 0 || strncmp(p, ": USB-Audio", 11)) continue;
		if (snprintf(dev, (size_t)cap, "plughw:CARD=%.*s,DEV=0", len, id) >= cap) {
			dev[0] = '\0';
			return false;
		}
		if (card) *card = idx;
		return true;
	}
	return false;
}

const char *aout_policy_name(aout_policy p)
{
	return p == AOUT_SPEAKER ? "Speaker" : "Auto";
}

const char *aout_dest_name(aout_dest d)
{
	return d == AOUT_USB ? "USB DAC" : d == AOUT_BT ? "bluetooth"
	     : d == AOUT_WIRED ? "wired" : "speaker";
}

bool aout_should_reapply(int remembered, bool wired_now, bool have_level)
{
	if (!have_level) return false;
	/* Unknown is not "unchanged". This is the line the defect was. */
	if (remembered == AOUT_JACK_UNKNOWN) return true;
	return remembered != (wired_now ? 1 : 0);
}

int aout_level_to_raw(int level, int level_max, int win_top, int win_bottom)
{
	long span = (long)win_bottom - win_top;
	long raw;

	if (level_max <= 0) return win_top;
	if (level < 0) level = 0;
	if (level > level_max) level = level_max;
	/* Rounded, not truncated: a half-rung lost at every step walks the whole
	 * ladder away from the ends it was calibrated against. */
	raw = win_top + ((long)(level_max - level) * span + level_max / 2) / level_max;
	if (raw < win_top) raw = win_top;
	if (raw > win_bottom) raw = win_bottom;
	return (int)raw;
}
