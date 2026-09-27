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
	ck(!strcmp(HK_BTN_NAME[b[0]], "L2"), "ff row -> L2");
	ck(!strcmp(HK_BTN_NAME[b[1]], "R2"), "rewind row -> R2");
	ck(!strcmp(HK_BTN_NAME[b[2]], "X"),  "savestate row -> X");
	ck(!strcmp(HK_BTN_NAME[b[3]], "Y"),  "loadstate row -> Y");

	printf("round-trips through serialize:\n");
	hk_serialize(b, out, sizeof out);
	ck(!strcmp(out, "l2:ff,r2:rewind,x:savestate,y:loadstate"), "byte for byte");

	printf("empty spec means every row is None:\n");
	hk_parse("", b);
	ck(!strcmp(HK_BTN_NAME[b[0]], "None"), "ff row -> None");
	ck(!strcmp(HK_BTN_NAME[b[1]], "None"), "rewind row -> None");
	hk_serialize(b, out, sizeof out);
	ck(!strcmp(out, ""), "serializes to an empty string");

	printf("NULL spec is treated the same as empty:\n");
	hk_parse(NULL, b);
	ck(!strcmp(HK_BTN_NAME[b[0]], "None"), "ff row -> None");

	printf("an unknown button name is ignored, not a crash:\n");
	hk_parse("select:ff,x:savestate", b);
	ck(!strcmp(HK_BTN_NAME[b[0]], "None"), "ff row untouched - select is not a candidate");
	ck(!strcmp(HK_BTN_NAME[b[2]], "X"), "the other, valid fragment still parsed");

	printf("an unknown action name is ignored, not a crash:\n");
	hk_parse("l2:turbo,x:savestate", b);
	ck(!strcmp(HK_BTN_NAME[b[2]], "X"), "savestate row still parsed");
	{
		int i, any = 0;
		for (i = 0; i < HK_ROW_COUNT; i++) if (b[i] == 1 /* L2 */) any = 1;
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
