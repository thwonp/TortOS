/* Does the in-game Shader list read its file, and say what diatom expects?
 *
 * Links src/shaderlist.c and NOT SDL, the same split hkbind-check relies on.
 * Two halves: fixtures for the parser's rules, then the shipped
 * res/shaders/shaders.cfg - every line taken, every file it names present.
 */
#include "../src/shaderlist.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fails;

static void ck(int cond, const char *what)
{
	printf("  %s %s\n", cond ? "ok  " : "FAIL", what);
	if (!cond) fails++;
}

static const char *write_tmp(const char *text)
{
	static char path[] = "build-native/shaderlist-fixture.cfg";
	FILE *f = fopen(path, "w");

	fputs(text, f);
	fclose(f);
	return path;
}

int main(void)
{
	sl_list l;
	char out[1024];
	int i;

	printf("no file:\n");
	ck(sl_load(&l, "build-native/no-such.cfg") == 1, "None alone");
	ck(!strcmp(l.e[0].name, "None"), "named None");
	ck(sl_fields(&l, 0, "/s", out, sizeof out) && !strcmp(out, "shader=none"),
	   "None sends shader=none");

	printf("a list:\n");
	sl_load(&l, write_tmp(
		"# comment\n"
		"\n"
		"  Real LCD | pixellate:nearest:0, lcd3x:nearest:0 | nearest  \n"
		"Smooth|stock:nearest:3|linear\n"
		"Two Bars | a:nearest | nearest\n"           /* no scale */
		"Bad Filter | a:bilinear:0 | nearest\n"
		"Big | a:nearest:5 | nearest\n"
		"Four | a:nearest:0,b:nearest:0,c:nearest:0,d:nearest:0 | nearest\n"
		"Bad Final | a:nearest:0 | soft\n"
		"None | a:nearest:0 | nearest\n"             /* the reserved name */
		"Smooth | b:nearest:0 | nearest\n"           /* a second Smooth */
		"Short | a:nearest:0\n"                      /* no final */
		"Long | a:nearest:0 | nearest | extra\n"));
	ck(l.count == 3, "None + the two good lines, the nine bad ones left out");
	ck(!strcmp(l.e[1].name, "Real LCD"), "names trimmed");
	ck(!strcmp(l.e[1].passes, "pixellate:nearest:0, lcd3x:nearest:0"), "passes kept");
	ck(sl_find(&l, "Smooth") == 2, "found by name");
	ck(sl_find(&l, "Gone") == 0, "a name no longer listed is None");
	ck(sl_find(&l, "") == 0 && sl_find(&l, NULL) == 0, "no name is None");
	ck(sl_fields(&l, 1, "/c/TortOS/shaders", out, sizeof out) &&
	   !strcmp(out, "shader=/c/TortOS/shaders/pixellate.glsl:nearest:0,"
	                "/c/TortOS/shaders/lcd3x.glsl:nearest:0\tfinal=nearest"),
	   "two passes become paths, final follows");
	ck(sl_fields(&l, 2, "/s", out, sizeof out) &&
	   !strcmp(out, "shader=/s/stock.glsl:nearest:3\tfinal=linear"), "one pass");
	ck(sl_fields(&l, 99, "/s", out, sizeof out) && !strcmp(out, "shader=none"),
	   "out of range is None");
	ck(!sl_fields(&l, 1, "/s", out, 30), "too small a buffer says so");

	printf("the shipped list (res/shaders/shaders.cfg):\n");
	sl_load(&l, "res/shaders/shaders.cfg");
	ck(l.count == 14, "None + 13 entries");
	for (i = 1; i < l.count; i++) {
		char buf[256], *p, *save = NULL, path[300], what[128];
		int ok = 1;

		strcpy(buf, l.e[i].passes);
		for (p = strtok_r(buf, ",", &save); p; p = strtok_r(NULL, ",", &save)) {
			*strchr(p, ':') = '\0';
			snprintf(path, sizeof path, "res/shaders/%s.glsl", p);
			if (access(path, R_OK) != 0) { printf("    missing %s\n", path); ok = 0; }
		}
		snprintf(what, sizeof what, "%s: every file present", l.e[i].name);
		ck(ok, what);
	}
	ck(sl_fields(&l, sl_find(&l, "Old TV"), "/storage/games-external/TortOS/shaders",
	             out, sizeof out), "the longest entry fits");

	printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
	return fails != 0;
}
