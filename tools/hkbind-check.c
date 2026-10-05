/* SPDX-License-Identifier: MIT */
/* Does the in-game Hotkeys screen's binding parser keep its promises?
 *
 * Links src/hkbind.c and NOT SDL - same split db-check.c relies on, so the
 * screen's own parse/serialize logic can be driven with no display and no
 * game running. Diatom's own hotkeys.c (the wire-format side of this same
 * feature) has its own equivalent test, in that repo.
 */
#include "../src/hkbind.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void ck(int cond, const char *what)
{
	if (!cond) { printf("  FAIL %s\n", what); fails++; }
}

int main(void)
{
	int b[HK_ROW_COUNT];
	char out[128];

	printf("a full valid spec:\n");
	hk_parse("l2:ff,r2:rewind,x:savestate,y:loadstate", b);
	ck(!strcmp(HK_TRIG_NAME[b[0]], "L2"), "ff row -> L2");
	ck(!strcmp(HK_TRIG_NAME[b[1]], "R2"), "rewind row -> R2");
	ck(!strcmp(HK_TRIG_NAME[b[2]], "X"),  "savestate row -> X");
	ck(!strcmp(HK_TRIG_NAME[b[3]], "Y"),  "loadstate row -> Y");

	printf("round-trips through serialize:\n");
	hk_serialize(b, out, sizeof out);
	ck(!strcmp(out, "l2:ff,r2:rewind,x:savestate,y:loadstate"), "byte for byte");

	printf("the Screenshot row (plorpos-gkd.86.2):\n");
	hk_parse("l2:ff,r2:rewind,x:savestate,y:loadstate,d.l1:screenshot", b);
	ck(!strcmp(HK_TRIG_NAME[b[4]], "L1") && !hk_trig_mod(b[4]), "screenshot row -> L1, direct");
	ck(!strcmp(HK_ACTION_LABEL[4], "Screenshot"), "labelled Screenshot");
	hk_serialize(b, out, sizeof out);
	ck(!strcmp(out, "l2:ff,r2:rewind,x:savestate,y:loadstate,d.l1:screenshot"),
	   "a full set of five round-trips byte for byte");
	hk_parse("l2:ff", b);
	ck(b[4] == 0, "unbound by default");

	printf("the L1/R1/A/B buttons (plorpos-gkd.22):\n");
	hk_parse("l1:ff,r1:rewind,a:savestate,b:loadstate", b);
	ck(!strcmp(HK_TRIG_NAME[b[0]], "L1"), "ff row -> L1");
	ck(!strcmp(HK_TRIG_NAME[b[1]], "R1"), "rewind row -> R1");
	ck(!strcmp(HK_TRIG_NAME[b[2]], "A"),  "savestate row -> A");
	ck(!strcmp(HK_TRIG_NAME[b[3]], "B"),  "loadstate row -> B");

	printf("a spec saved with display/filter keeps the rest (plorpos-gkd.73):\n");
	hk_parse("sright:ff,sleft:rewind,x:display,a:filter", b);
	ck(!strcmp(HK_TRIG_NAME[b[0]], "Stick Right"), "ff row -> Stick Right");
	ck(!strcmp(HK_TRIG_NAME[b[1]], "Stick Left"),  "rewind row -> Stick Left");
	ck(!b[2] && !b[3], "X and A bound nothing - neither is an action now");
	hk_serialize(b, out, sizeof out);
	ck(!strcmp(out, "sright:ff,sleft:rewind"), "and a save drops them");

	printf("layers and directions (plorpos-gkd.43.2):\n");
	hk_parse("d.x:ff,x:rewind,up:savestate,sright:loadstate", b);
	ck(!strcmp(HK_TRIG_NAME[b[0]], "X") && !hk_trig_mod(b[0]), "ff row -> X, direct");
	ck(!strcmp(HK_TRIG_NAME[b[1]], "X") && hk_trig_mod(b[1]),  "rewind row -> X, with the modifier");
	ck(b[0] != b[1], "X and d.X are two triggers");
	ck(!strcmp(HK_TRIG_NAME[b[2]], "Up") && hk_trig_mod(b[2]) && !hk_trig_stick(b[2]),
	   "savestate row -> d-pad Up, with the modifier");
	ck(!strcmp(HK_TRIG_NAME[b[3]], "Stick Right") && hk_trig_mod(b[3]) && hk_trig_stick(b[3]),
	   "loadstate row -> Stick Right, with the modifier");
	hk_serialize(b, out, sizeof out);
	ck(!strcmp(out, "d.x:ff,x:rewind,up:savestate,sright:loadstate"),
	   "round-trips byte for byte");
	ck(!hk_trig_mod(0) && !hk_trig_stick(0), "None is neither");

	printf("a stored pre-43.2 spec keeps its meaning (the modifier layer):\n");
	hk_parse("r2:ff,l1:loadstate", b);
	ck(!strcmp(HK_TRIG_NAME[b[0]], "R2") && hk_trig_mod(b[0]), "ff row -> R2, with the modifier");
	ck(!strcmp(HK_TRIG_NAME[b[3]], "L1") && hk_trig_mod(b[3]), "loadstate row -> L1, with the modifier");

	printf("press-to-bind: what each press makes (plorpos-gkd.43.3):\n");
	{
		int in;
		for (in = 0; in < HK_IN_COUNT; in++) {
			int m = hk_trig_from(in, 1), d = hk_trig_from(in, 0);
			int alone = in < HK_IN_UP || in >= HK_IN_RSUP;
			ck(m > 0 && hk_trig_mod(m), "every input makes a modifier trigger");
			ck(alone ? d > 0 && !hk_trig_mod(d) && d != m : d == 0,
			   "buttons and the right stick alone are direct; other directions alone are nothing");
			ck(!hk_trig_stick(m) == (in < HK_IN_SUP || in >= HK_IN_RSUP),
			   "only the left stick makes (left) stick triggers");
		}
	}
	hk_parse("x:ff,d.x:rewind,up:savestate,sright:loadstate", b);
	ck(b[0] == hk_trig_from(HK_IN_X, 1),      "Mod + X is x");
	ck(b[1] == hk_trig_from(HK_IN_X, 0),      "X alone is d.x");
	ck(b[2] == hk_trig_from(HK_IN_UP, 1),     "Mod + Up is up");
	ck(b[3] == hk_trig_from(HK_IN_SRIGHT, 1), "Mod + Stick Right is sright");
	ck(hk_trig_from(HK_IN_COUNT, 1) == 0 && hk_trig_from(-1, 0) == 0, "out of range is None");

	printf("the Brick Pro's right stick, both layers (plorpos-pky.17):\n");
	hk_parse("rsup:ff,d.rsup:rewind,d.rsleft:savestate,rsright:loadstate", b);
	ck(!strcmp(HK_TRIG_NAME[b[0]], "R Stick Up") && hk_trig_mod(b[0]) && !hk_trig_stick(b[0]),
	   "ff row -> R Stick Up, with the modifier");
	ck(!strcmp(HK_TRIG_NAME[b[1]], "R Stick Up") && !hk_trig_mod(b[1]),
	   "rewind row -> R Stick Up, direct");
	ck(b[1] == hk_trig_from(HK_IN_RSUP, 0) && b[0] == hk_trig_from(HK_IN_RSUP, 1),
	   "press-to-bind makes the same two");
	ck(b[2] == hk_trig_from(HK_IN_RSLEFT, 0) && b[3] == hk_trig_from(HK_IN_RSRIGHT, 1),
	   "R Stick Left alone is d.rsleft; Mod + R Stick Right is rsright");
	hk_serialize(b, out, sizeof out);
	ck(!strcmp(out, "rsup:ff,d.rsup:rewind,d.rsleft:savestate,rsright:loadstate"),
	   "round-trips byte for byte");

	printf("empty spec means every row is None:\n");
	hk_parse("", b);
	ck(!strcmp(HK_TRIG_NAME[b[0]], "None"), "ff row -> None");
	ck(!strcmp(HK_TRIG_NAME[b[1]], "None"), "rewind row -> None");
	hk_serialize(b, out, sizeof out);
	ck(!strcmp(out, ""), "serializes to an empty string");

	printf("NULL spec is treated the same as empty:\n");
	hk_parse(NULL, b);
	ck(!strcmp(HK_TRIG_NAME[b[0]], "None"), "ff row -> None");

	printf("an unknown button name is ignored, not a crash:\n");
	hk_parse("select:ff,x:savestate", b);
	ck(!strcmp(HK_TRIG_NAME[b[0]], "None"), "ff row untouched - select is not a candidate");
	ck(!strcmp(HK_TRIG_NAME[b[2]], "X"), "the other, valid fragment still parsed");

	printf("an unknown action name is ignored, not a crash:\n");
	hk_parse("l2:turbo,x:savestate", b);
	ck(!strcmp(HK_TRIG_NAME[b[2]], "X"), "savestate row still parsed");
	{
		int i, any = 0;
		for (i = 0; i < HK_ROW_COUNT; i++) if (b[i] == 3 /* L2 */) any = 1;
		ck(!any, "l2:turbo bound nothing - turbo is not a hotkey action");
	}

	printf("serialize skips None rows entirely, no dangling comma:\n");
	hk_parse("x:savestate", b);
	hk_serialize(b, out, sizeof out);
	ck(!strcmp(out, "x:savestate"), "one entry, no leading or trailing comma");

	printf("a too-small output buffer truncates rather than overflows:\n");
	hk_parse("l2:ff,r2:rewind,x:savestate,y:loadstate", b);
	{
		char tiny[6];
		hk_serialize(b, tiny, sizeof tiny);
		ck(strlen(tiny) < sizeof tiny, "null-terminated within bounds");
	}

	if (fails) { printf("FAILED: %d\n", fails); return 1; }
	printf("ok: the hotkey screen's binding parser keeps its promises\n");
	return 0;
}
