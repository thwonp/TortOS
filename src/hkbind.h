/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The in-game Hotkeys screen's binding parser/serializer, split out of
 * main.c under ADR-0001 so a check can drive the real thing with no SDL -
 * tools/hkbind-check.c links it without one. Not NextUI-derived: unlike
 * Diatom's own hotkeys.c (its sibling ADR-0035), this side is a plain
 * left/right cycling menu, the same shape Display Mode already is.
 *
 * Four fixed rows (fast-forward, rewind, quicksave, quickload), each bound
 * to at most one of a short candidate button list. The candidates are
 * exactly what Diatom's display_chord does NOT already claim under SELECT
 * (l1/r1/a) - see diatom's ADR-0035 - so a binding made here can never be
 * one Diatom would refuse. */
#ifndef TORTOS_HKBIND_H
#define TORTOS_HKBIND_H

#include <stddef.h>

extern const char *const HK_BTN_NAME[];      /* "None", "L2", "R2", "X", "Y" */
#define HK_BTN_COUNT 5
extern const char *const HK_ACTION_LABEL[];  /* "Fast-Forward", "Rewind", ... */
#define HK_ROW_COUNT 4

/* Parses a stored or live spec ("l2:ff,x:savestate") into one button index
 * (0..HK_BTN_COUNT-1, 0 = None) per action row. A fragment that names an
 * unknown button or action is simply not represented - the same "ignore
 * what does not parse" the wire format's own parser (Diatom's hotkeys_set)
 * uses one layer up. */
void hk_parse(const char *spec, int btn_for_row[HK_ROW_COUNT]);

/* The reverse of hk_parse: one entry per row with a real button, skipping
 * "None" rows entirely - an unbound action is simply absent from the spec,
 * which is what an empty binding means on the wire. */
void hk_serialize(const int btn_for_row[HK_ROW_COUNT], char *out, size_t n);

#endif
