/* SPDX-License-Identifier: MIT */
/* The in-game Hotkeys screen's binding parser/serializer, split out of
 * main.c under ADR-0001 so a check can drive the real thing with no SDL -
 * tools/hkbind-check.c links it without one. Not NextUI-derived: unlike
 * Diatom's own hotkeys.c (its sibling ADR-0035), this side is a plain
 * left/right cycling menu, the same shape Display Mode already is.
 *
 * Six fixed rows (fast-forward, rewind, quicksave, quickload, display mode,
 * screen filter), each bound to at most one trigger - exactly the set
 * Diatom's hotkeys_set accepts (its ADR-0035 and ADR-0039), so a binding made
 * here can never be one Diatom would refuse. A trigger is a face button or
 * shoulder with the modifier held (`x`) or alone (`d.x`), or a d-pad or stick
 * direction with the modifier held (`up`, `sright`). */
#ifndef TORTOS_HKBIND_H
#define TORTOS_HKBIND_H

#include <stddef.h>

/* Trigger 0 is None. The name is the input alone ("X", "Stick Up"); the
 * screen puts the modifier's own label in front where hk_trig_mod says so. */
extern const char *const HK_TRIG_NAME[];
#define HK_TRIG_COUNT 25
int hk_trig_mod(int t);     /* needs the modifier held */
int hk_trig_stick(int t);   /* a stick direction - absent without a stick */

/* Press-to-bind (plorpos-gkd.43.3): the trigger an input makes, pressed with
 * the modifier held (`mod`) or alone - 0 (None) for a direction alone, which
 * is never a trigger. Inputs are named here, not as the launcher's in_button,
 * so this stays SDL-free and checkable. */
enum {
	HK_IN_L1, HK_IN_R1, HK_IN_L2, HK_IN_R2, HK_IN_A, HK_IN_B, HK_IN_X, HK_IN_Y,
	HK_IN_UP, HK_IN_DOWN, HK_IN_LEFT, HK_IN_RIGHT,
	HK_IN_SUP, HK_IN_SDOWN, HK_IN_SLEFT, HK_IN_SRIGHT,
	HK_IN_COUNT
};
int hk_trig_from(int input, int mod);
extern const char *const HK_ACTION_LABEL[];  /* "Fast-Forward", "Rewind", ... */
#define HK_ROW_COUNT 4

/* Parses a stored or live spec ("l2:ff,d.x:savestate") into one trigger index
 * (0..HK_TRIG_COUNT-1, 0 = None) per action row. A fragment that names an
 * unknown button or action is simply not represented - the same "ignore
 * what does not parse" the wire format's own parser (Diatom's hotkeys_set)
 * uses one layer up. */
void hk_parse(const char *spec, int trig_for_row[HK_ROW_COUNT]);

/* The reverse of hk_parse: one entry per row with a real trigger, skipping
 * "None" rows entirely - an unbound action is simply absent from the spec,
 * which is what an empty binding means on the wire. */
void hk_serialize(const int trig_for_row[HK_ROW_COUNT], char *out, size_t n);

#endif
