/* SPDX-License-Identifier: MIT */
#include "gbpal.h"

#include <stdio.h>
#include <string.h>

/* Values exactly as mgba 0.11 declares mgba_gb_colors - arrows and all, they
 * are the buttons held at a Game Boy Color's boot - and labels without the
 * arrows, which the menu font may not have.
 *
 * Auto is what a Game Boy Color does with nothing held: mgba_gb_colors_preset
 * 1 looks the game up in the GBC boot ROM's table (144 games - Pokemon Red
 * comes out red) and anything not in it gets Dark Green, the GBC's own
 * default. The preset is read at load, so switching TO Auto mid-game shows the
 * fallback until the next launch; every fixed palette applies at once. Checked
 * on the GKD 2026-10-03 with one-shots: Pokemon Red under Auto red and green,
 * a homebrew game under Auto identical to Dark Green, and a resume state made
 * under Auto loaded grey under Grayscale - a state does not carry the palette,
 * so the choice holds on a game already played. */
static const struct { const char *label, *value; } PAL[GBPAL_COUNT] = {
	{ "Auto",            "GBC Dark Green →A" },
	{ "Grayscale",       "Grayscale" },
	{ "DMG Green",       "DMG Green" },
	{ "GB Pocket",       "GB Pocket" },
	{ "GB Light",        "GB Light" },
	{ "GBC Brown",       "GBC Brown ↑" },
	{ "GBC Red",         "GBC Red ↑A" },
	{ "GBC Dark Brown",  "GBC Dark Brown ↑B" },
	{ "GBC Pale Yellow", "GBC Pale Yellow ↓" },
	{ "GBC Orange",      "GBC Orange ↓A" },
	{ "GBC Yellow",      "GBC Yellow ↓B" },
	{ "GBC Blue",        "GBC Blue ←" },
	{ "GBC Dark Blue",   "GBC Dark Blue ←A" },
	{ "GBC Gray",        "GBC Gray ←B" },
	{ "GBC Green",       "GBC Green →" },
	{ "GBC Dark Green",  "GBC Dark Green →A" },
	{ "GBC Reverse",     "GBC Reverse →B" },
};

const char *gbpal_label(int i)
{
	return i >= 0 && i < GBPAL_COUNT ? PAL[i].label : PAL[0].label;
}

int gbpal_find(const char *label)
{
	int i;

	if (!label) return 0;
	for (i = 0; i < GBPAL_COUNT; i++)
		if (!strcmp(PAL[i].label, label)) return i;
	return 0;
}

int gbpal_opts(int i, char *preset, size_t pcap, char *colors, size_t ccap)
{
	int a, b;

	if (i < 0 || i >= GBPAL_COUNT) i = 0;
	a = snprintf(preset, pcap, "mgba_gb_colors_preset=%d", i == 0 ? 1 : 0);
	b = snprintf(colors, ccap, "mgba_gb_colors=%s", PAL[i].value);
	return a > 0 && (size_t)a < pcap && b > 0 && (size_t)b < ccap;
}
