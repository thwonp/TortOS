/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The in-game Palette list for Game Boy games (plorpos-gkd.76): a choice per
 * game, turned into the two mgba core options that make it. Split out of
 * main.c under ADR-0001 so tools/gbpal-check.c drives it with no SDL. */
#ifndef TORTOS_GBPAL_H
#define TORTOS_GBPAL_H

#include <stddef.h>

/* Auto first, then mgba's four plain palettes and the twelve a Game Boy Color
 * offers at boot. Its 32 Super Game Boy palettes are left out: "SGB 2-F"
 * tells a player nothing. */
#define GBPAL_COUNT 17

/* The menu's label, and what palette.GB.<file> stores. */
const char *gbpal_label(int i);

/* The entry labelled `label`; 0 (Auto) for NULL, "" or a label not listed. */
int gbpal_find(const char *label);

/* "mgba_gb_colors_preset=N" and "mgba_gb_colors=<value>" for entry i, the
 * shape plat_coreopt hands out. False if either does not fit. */
int gbpal_opts(int i, char *preset, size_t pcap, char *colors, size_t ccap);

#endif
