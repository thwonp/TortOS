/* SPDX-License-Identifier: MIT */
/* See hkbind.h. */
#include <stdio.h>
#include <string.h>

#include "hkbind.h"

const char *const HK_BTN_NAME[] = { "None", "L2", "R2", "X", "Y" };
static const char *const HK_BTN_WIRE[] = { NULL, "l2", "r2", "x", "y" };
const char *const HK_ACTION_LABEL[] = {
	"Fast-Forward", "Rewind", "Quick Save", "Quick Load"
};
static const char *const HK_ACTION_WIRE[] = { "ff", "rewind", "savestate", "loadstate" };

/* hkbind.h's HK_BTN_COUNT/HK_ROW_COUNT are plain numbers, not derived from
 * these arrays' own sizes - an extern array is an incomplete type in the
 * header that declares it, so sizeof cannot cross that boundary the way it
 * could when everything lived in one file (see BRIGHT_MAX in platform.c for
 * that in-file version of the same idiom). Checked here instead, at the one
 * place both the header's numbers and these arrays are in scope together, so
 * a mismatch fails the build rather than corrupting a caller's stack array. */
_Static_assert(sizeof HK_BTN_NAME / sizeof *HK_BTN_NAME == HK_BTN_COUNT,
               "HK_BTN_COUNT must match HK_BTN_NAME's length");
_Static_assert(sizeof HK_BTN_WIRE / sizeof *HK_BTN_WIRE == HK_BTN_COUNT,
               "HK_BTN_COUNT must match HK_BTN_WIRE's length");
_Static_assert(sizeof HK_ACTION_LABEL / sizeof *HK_ACTION_LABEL == HK_ROW_COUNT,
               "HK_ROW_COUNT must match HK_ACTION_LABEL's length");
_Static_assert(sizeof HK_ACTION_WIRE / sizeof *HK_ACTION_WIRE == HK_ROW_COUNT,
               "HK_ROW_COUNT must match HK_ACTION_WIRE's length");

void hk_parse(const char *spec, int btn_for_row[HK_ROW_COUNT])
{
	char buf[128], *save = NULL, *tok;
	int i;

	for (i = 0; i < HK_ROW_COUNT; i++) btn_for_row[i] = 0;
	if (!spec || !*spec) return;
	snprintf(buf, sizeof buf, "%s", spec);
	for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		char *colon = strchr(tok, ':');
		int b, r;

		if (!colon) continue;
		*colon = '\0';
		for (b = 1; b < HK_BTN_COUNT; b++)
			if (!strcmp(tok, HK_BTN_WIRE[b])) break;
		if (b == HK_BTN_COUNT) continue;
		for (r = 0; r < HK_ROW_COUNT; r++)
			if (!strcmp(colon + 1, HK_ACTION_WIRE[r])) break;
		if (r == HK_ROW_COUNT) continue;
		btn_for_row[r] = b;
	}
}

void hk_serialize(const int btn_for_row[HK_ROW_COUNT], char *out, size_t n)
{
	char *p = out;
	size_t left = n;
	int r;

	*p = '\0';
	for (r = 0; r < HK_ROW_COUNT; r++) {
		int written;

		if (!btn_for_row[r]) continue;
		written = snprintf(p, left, "%s%s:%s", p == out ? "" : ",",
		                   HK_BTN_WIRE[btn_for_row[r]], HK_ACTION_WIRE[r]);
		if (written < 0 || (size_t)written >= left) break;
		p += written;
		left -= (size_t)written;
	}
}
