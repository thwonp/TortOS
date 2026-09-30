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

	printf("display/filter rows and the L1/R1/A/B buttons (plorpos-gkd.22):\n");
	hk_parse("l1:ff,r1:rewind,a:savestate,b:loadstate,x:display,y:filter", b);
	ck(!strcmp(HK_TRIG_NAME[b[0]], "L1"), "ff row -> L1");
	ck(!strcmp(HK_TRIG_NAME[b[1]], "R1"), "rewind row -> R1");
	ck(!strcmp(HK_TRIG_NAME[b[2]], "A"),  "savestate row -> A");
	ck(!strcmp(HK_TRIG_NAME[b[3]], "B"),  "loadstate row -> B");
	ck(!strcmp(HK_TRIG_NAME[b[4]], "X"),  "display row -> X");
	ck(!strcmp(HK_TRIG_NAME[b[5]], "Y"),  "filter row -> Y");
	hk_serialize(b, out, sizeof out);
	ck(!strcmp(out, "l1:ff,r1:rewind,a:savestate,b:loadstate,x:display,y:filter"),
	   "all six round-trip byte for byte");

	printf("layers and directions (plorpos-gkd.43.2):\n");
	hk_parse("d.x:ff,x:rewind,up:savestate,sright:loadstate,d.l2:display", b);
	ck(!strcmp(HK_TRIG_NAME[b[0]], "X") && !hk_trig_mod(b[0]), "ff row -> X, direct");
	ck(!strcmp(HK_TRIG_NAME[b[1]], "X") && hk_trig_mod(b[1]),  "rewind row -> X, with the modifier");
	ck(b[0] != b[1], "X and d.X are two triggers");
	ck(!strcmp(HK_TRIG_NAME[b[2]], "Up") && hk_trig_mod(b[2]) && !hk_trig_stick(b[2]),
	   "savestate row -> d-pad Up, with the modifier");
	ck(!strcmp(HK_TRIG_NAME[b[3]], "Stick Right") && hk_trig_mod(b[3]) && hk_trig_stick(b[3]),
	   "loadstate row -> Stick Right, with the modifier");
	ck(!strcmp(HK_TRIG_NAME[b[4]], "L2") && !hk_trig_mod(b[4]), "display row -> L2, direct");
	ck(!strcmp(HK_TRIG_NAME[b[5]], "None"), "filter row -> None");
	hk_serialize(b, out, sizeof out);
	ck(!strcmp(out, "d.x:ff,x:rewind,up:savestate,sright:loadstate,d.l2:display"),
	   "round-trips byte for byte");
	ck(!hk_trig_mod(0) && !hk_trig_stick(0), "None is neither");

	printf("a stored pre-43.2 spec keeps its meaning (the modifier layer):\n");
	hk_parse("r2:ff,l1:display", b);
	ck(!strcmp(HK_TRIG_NAME[b[0]], "R2") && hk_trig_mod(b[0]), "ff row -> R2, with the modifier");
	ck(!strcmp(HK_TRIG_NAME[b[4]], "L1") && hk_trig_mod(b[4]), "display row -> L1, with the modifier");

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
