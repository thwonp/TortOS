/* SPDX-License-Identifier: MIT */
/* Does the in-game Palette list send what mgba takes?
 *
 * Links src/gbpal.c and NOT SDL. A value mgba does not declare is not an
 * error anywhere - the core keeps its own default and the row looks broken -
 * so every value is checked against the list the shipped core printed:
 * `diatom --list-options` with mgba_libretro.so (mGBA 0.11.0) on the GKD,
 * 2026-10-03.
 */
#include "../src/gbpal.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void ck(int cond, const char *what)
{
	printf("  %s %s\n", cond ? "ok  " : "FAIL", what);
	if (!cond) fails++;
}

static const char *const MGBA[] = {
	"Grayscale", "DMG Green", "GB Pocket", "GB Light",
	"GBC Brown ↑", "GBC Red ↑A", "GBC Dark Brown ↑B", "GBC Pale Yellow ↓",
	"GBC Orange ↓A", "GBC Yellow ↓B", "GBC Blue ←", "GBC Dark Blue ←A",
	"GBC Gray ←B", "GBC Green →", "GBC Dark Green →A", "GBC Reverse →B",
};

static int declared(const char *v)
{
	size_t i;

	for (i = 0; i < sizeof MGBA / sizeof MGBA[0]; i++)
		if (!strcmp(MGBA[i], v)) return 1;
	return 0;
}

int main(void)
{
	char p[64], c[64], what[128];
	int i, j, dup = 0;

	printf("lookup:\n");
	ck(gbpal_find(NULL) == 0 && gbpal_find("") == 0, "nothing is Auto");
	ck(gbpal_find("SGB 2-F") == 0, "a label not listed is Auto");
	ck(!strcmp(gbpal_label(0), "Auto"), "Auto leads");
	ck(!strcmp(gbpal_label(GBPAL_COUNT), "Auto"), "out of range reads Auto");

	printf("options:\n");
	ck(gbpal_opts(0, p, sizeof p, c, sizeof c) &&
	   !strcmp(p, "mgba_gb_colors_preset=1") &&
	   !strcmp(c, "mgba_gb_colors=GBC Dark Green →A"),
	   "Auto: the GBC table, Dark Green where a game is not in it");
	ck(gbpal_opts(3, p, sizeof p, c, sizeof c) &&
	   !strcmp(p, "mgba_gb_colors_preset=0") &&
	   !strcmp(c, "mgba_gb_colors=GB Pocket"),
	   "a fixed palette turns the table off");
	ck(!gbpal_opts(1, p, 8, c, sizeof c), "too small does not fit");

	printf("every entry:\n");
	for (i = 0; i < GBPAL_COUNT; i++) {
		gbpal_opts(i, p, sizeof p, c, sizeof c);
		snprintf(what, sizeof what, "%-16s %s", gbpal_label(i), c + strlen("mgba_gb_colors="));
		ck(declared(c + strlen("mgba_gb_colors=")) && gbpal_find(gbpal_label(i)) == i, what);
		for (j = 0; j < i; j++)
			if (!strcmp(gbpal_label(i), gbpal_label(j))) dup++;
	}
	ck(dup == 0, "no two labels alike");

	printf(fails ? "%d FAILED\n" : "ALL PASS\n", fails);
	return fails != 0;
}
