/* SPDX-License-Identifier: MIT */
/* See hkbind.h. */
#include <stdio.h>
#include <string.h>

#include "hkbind.h"

/* Cycling order: the modifier layer first (what every binding was before
 * plorpos-gkd.43.2, so indexes 1-8 kept their meaning), then the same buttons
 * direct, then the directions, which only exist with the modifier - except
 * the Brick Pro's right stick, last, on both layers (diatom's ADR-0044). */
#define HK_BUTTONS(p) p "l1", p "r1", p "l2", p "r2", p "a", p "b", p "x", p "y"
const char *const HK_TRIG_NAME[] = {
	"None",
	"L1", "R1", "L2", "R2", "A", "B", "X", "Y",
	"L1", "R1", "L2", "R2", "A", "B", "X", "Y",
	"Up", "Down", "Left", "Right",
	"Stick Up", "Stick Down", "Stick Left", "Stick Right",
	"R Stick Up", "R Stick Down", "R Stick Left", "R Stick Right",
	"R Stick Up", "R Stick Down", "R Stick Left", "R Stick Right",
};
static const char *const HK_TRIG_WIRE[] = {
	NULL,
	HK_BUTTONS(""),
	HK_BUTTONS("d."),
	"up", "down", "left", "right",
	"sup", "sdown", "sleft", "sright",
	"rsup", "rsdown", "rsleft", "rsright",
	"d.rsup", "d.rsdown", "d.rsleft", "d.rsright",
};
enum { HK_FIRST_DIRECT = 9, HK_FIRST_DPAD = 17, HK_FIRST_STICK = 21,
       HK_FIRST_RSTICK = 25, HK_FIRST_RSTICK_DIRECT = 29 };

int hk_trig_mod(int t)
{
	return t > 0 && (t < HK_FIRST_DIRECT
	                 || (t >= HK_FIRST_DPAD && t < HK_FIRST_RSTICK_DIRECT));
}
int hk_trig_stick(int t) { return t >= HK_FIRST_STICK && t < HK_FIRST_RSTICK; }

int hk_trig_from(int input, int mod)
{
	if (input < 0 || input >= HK_IN_COUNT) return 0;
	if (input < HK_IN_UP) return (mod ? 1 : HK_FIRST_DIRECT) + input;
	if (input >= HK_IN_RSUP)
		return (mod ? HK_FIRST_RSTICK : HK_FIRST_RSTICK_DIRECT) + (input - HK_IN_RSUP);
	return mod ? HK_FIRST_DPAD + (input - HK_IN_UP) : 0;
}
const char *const HK_ACTION_LABEL[] = {
	"Fast-Forward", "Rewind", "Quick Save", "Quick Load", "Screenshot"
};
static const char *const HK_ACTION_WIRE[] = {
	"ff", "rewind", "savestate", "loadstate", "screenshot"
};

/* hkbind.h's HK_TRIG_COUNT/HK_ROW_COUNT are plain numbers, not derived from
 * these arrays' own sizes - an extern array is an incomplete type in the
 * header that declares it, so sizeof cannot cross that boundary the way it
 * could when everything lived in one file (see BRIGHT_MAX in platform.c for
 * that in-file version of the same idiom). Checked here instead, at the one
 * place both the header's numbers and these arrays are in scope together, so
 * a mismatch fails the build rather than corrupting a caller's stack array. */
_Static_assert(sizeof HK_TRIG_NAME / sizeof *HK_TRIG_NAME == HK_TRIG_COUNT,
               "HK_TRIG_COUNT must match HK_TRIG_NAME's length");
_Static_assert(sizeof HK_TRIG_WIRE / sizeof *HK_TRIG_WIRE == HK_TRIG_COUNT,
               "HK_TRIG_COUNT must match HK_TRIG_WIRE's length");
_Static_assert(sizeof HK_ACTION_LABEL / sizeof *HK_ACTION_LABEL == HK_ROW_COUNT,
               "HK_ROW_COUNT must match HK_ACTION_LABEL's length");
_Static_assert(sizeof HK_ACTION_WIRE / sizeof *HK_ACTION_WIRE == HK_ROW_COUNT,
               "HK_ROW_COUNT must match HK_ACTION_WIRE's length");

void hk_parse(const char *spec, int trig_for_row[HK_ROW_COUNT])
{
	char buf[128], *save = NULL, *tok;
	int i;

	for (i = 0; i < HK_ROW_COUNT; i++) trig_for_row[i] = 0;
	if (!spec || !*spec) return;
	snprintf(buf, sizeof buf, "%s", spec);
	for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		char *colon = strchr(tok, ':');
		int b, r;

		if (!colon) continue;
		*colon = '\0';
		for (b = 1; b < HK_TRIG_COUNT; b++)
			if (!strcmp(tok, HK_TRIG_WIRE[b])) break;
		if (b == HK_TRIG_COUNT) continue;
		for (r = 0; r < HK_ROW_COUNT; r++)
			if (!strcmp(colon + 1, HK_ACTION_WIRE[r])) break;
		if (r == HK_ROW_COUNT) continue;
		trig_for_row[r] = b;
	}
}

void hk_serialize(const int trig_for_row[HK_ROW_COUNT], char *out, size_t n)
{
	char *p = out;
	size_t left = n;
	int r;

	*p = '\0';
	for (r = 0; r < HK_ROW_COUNT; r++) {
		int written;

		if (!trig_for_row[r]) continue;
		written = snprintf(p, left, "%s%s:%s", p == out ? "" : ",",
		                   HK_TRIG_WIRE[trig_for_row[r]], HK_ACTION_WIRE[r]);
		if (written < 0 || (size_t)written >= left) break;
		p += written;
		left -= (size_t)written;
	}
}
