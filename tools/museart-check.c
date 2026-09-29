/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* Does Muse pick the right record, and ask for it the way MusicBrainz asks?
 *
 *     make check-museart
 *
 * THE RULE, against what MusicBrainz actually said about the first card's
 * seven albums on 2026-09-19 (tools/fixtures/musicbrainz, trimmed to the fields
 * the rule reads). Two of them put a same-named single first and one matched
 * three bands, so a rule that took the first hit, or the first album, would
 * pass half of this.
 *
 * THE RUN, on a network made of stubs: it keeps MusicBrainz's pace, waits and
 * asks once more when told it is busy, counts a release with no front cover as
 * missing rather than failed, and stops after three failures in a row.
 *
 * No network, no device.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/museart.h"
#include "../src/net.h"

static int failures;

#define CHECK(cond, ...)                                                      \
	do {                                                                      \
		if (!(cond)) {                                                        \
			printf("  FAIL: ");                                               \
			printf(__VA_ARGS__);                                              \
			printf("\n        at %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
			failures++;                                                       \
		}                                                                     \
	} while (0)

static char *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	long n;
	char *b;

	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc((size_t)n + 1);
	*len = fread(b, 1, (size_t)n, f);
	b[*len] = '\0';
	fclose(f);
	return b;
}

/* ---- the rule ----------------------------------------------------------- */

static void the_rule(void)
{
	static const struct { const char *file; int tracks; const char *rg, *why; } t[] = {
		{ "la-strada", 6, "7f05f461-7ac5-44af-99f9-8445dbf1547b",
		  "La Strada: three bands answer, and six tracks is the 2009 EP" },
		{ "passion-pit", 5, "038e07e0-18a6-42fe-937c-45d729b605e5",
		  "a five-track rip of a six-track EP still finds it" },
		{ "pixies", 15, "ac9b0a0c-0da6-387f-8c09-65783187346a", "Trompe le Monde" },
		{ "radiohead", 12, "b8048f24-c026-3398-b23a-b5e50716cbc7",
		  "The Bends the album, not the single MusicBrainz lists first" },
		{ "sara-bareilles", 12, "c9b76206-b134-3ba8-a1de-265999104ec2",
		  "Little Voice the album, not the single MusicBrainz lists first" },
		{ "the-xx", 11, "23355caf-a543-4b5f-80fe-449101868fc1", "xx" },
		{ "they-might-be-giants", 19, "9eed8bfd-2eb6-3e02-a1ac-8cd9b2628ce6", "Flood" },
	};
	char path[256], rg[64];
	size_t len, i;

	printf("the rule, against MusicBrainz's own replies\n");
	for (i = 0; i < sizeof t / sizeof t[0]; i++) {
		char *j;

		snprintf(path, sizeof path, "tools/fixtures/musicbrainz/%s.json", t[i].file);
		j = slurp(path, &len);
		CHECK(j != NULL, "fixture %s", path);
		if (!j) continue;
		CHECK(museart_pick(j, len, t[i].tracks, rg, sizeof rg) && !strcmp(rg, t[i].rg),
		      "%s: got %s", t[i].why, rg);
		free(j);
	}
	CHECK(!museart_pick("{\"releases\":[]}", 15, 5, rg, sizeof rg),
	      "no releases is no album");
	CHECK(!museart_pick("{\"releases\":[{\"score\":60,\"track-count\":5,"
	                    "\"release-group\":{\"id\":\"x\"}}]}", 77, 5, rg, sizeof rg),
	      "a match MusicBrainz is not sure of is no match");
	CHECK(!museart_pick("not json", 8, 5, rg, sizeof rg), "and a reply that is not JSON");
}

static void the_url(void)
{
	char u[2400];

	printf("the search\n");
	CHECK(museart_search_url("Radiohead", "The Bends", u, sizeof u) &&
	      !strcmp(u, "https://musicbrainz.org/ws/2/release/?query=release%3A%22The%20"
	                 "Bends%22%20AND%20artist%3A%22Radiohead%22&fmt=json&limit=25"),
	      "a release by its title and artist: %s", u);
	CHECK(museart_search_url("A \"Band\"", "Back\\slash (Deluxe)", u, sizeof u) &&
	      strstr(u, "%5C%22Band%5C%22") && strstr(u, "Back%5C%5Cslash%20%28Deluxe%29"),
	      "quotes and backslashes escaped inside the phrase, parentheses left: %s", u);
	CHECK(!museart_search_url("x", "y", u, 40), "a URL that does not fit is refused");
	CHECK(museart_search_url("", "Trompe Le Monde", u, sizeof u) &&
	      strstr(u, "query=release%3A%22Trompe%20Le%20Monde%22&") && !strstr(u, "artist"),
	      "no artist is the title alone: %s", u);
}

/* The three albums that missed on the card, 2026-09-28, and what each is asked
 * as after its folder names find nothing. See museart_tries. */
static void the_tries(void)
{
	char ar[MUSEART_TRIES][128], al[MUSEART_TRIES][128];
	int n;

	printf("the ways of asking\n");
	n = museart_tries("Son Little", "Son Little (Deluxe Edition)", ar, al);
	CHECK(n == 2 && !strcmp(al[1], "Son Little") && !strcmp(ar[1], "Son Little"),
	      "an edition dropped from the title: %d, \"%s\"", n, n > 1 ? al[1] : "");
	n = museart_tries("Yo-Yo Ma, Stuart Duncan, Edgar Meyer, Chris Thile",
	                  "The Goat Rodeo Sessions", ar, al);
	CHECK(n == 2 && !strcmp(ar[1], "Yo-Yo Ma") && !strcmp(al[1], "The Goat Rodeo Sessions"),
	      "the first of several artists: %d, \"%s\"", n, n > 1 ? ar[1] : "");
	n = museart_tries("Trompe Le Monde", "Trompe Le Monde", ar, al);
	CHECK(n == 2 && !ar[1][0] && !strcmp(al[1], "Trompe Le Monde"),
	      "a folder with no artist above it: the title alone: %d", n);
	n = museart_tries("Simon & Garfunkel", "Bookends", ar, al);
	CHECK(n == 2 && !strcmp(ar[0], "Simon & Garfunkel") && !strcmp(ar[1], "Simon"),
	      "the names as typed are always asked first: %d", n);
	n = museart_tries("Radiohead", "The Bends", ar, al);
	CHECK(n == 1, "nothing to drop, nothing asked twice: %d", n);
	n = museart_tries("Talking Heads", "Fear Of Music [Bonus Tracks]", ar, al);
	CHECK(n == 2 && !strcmp(al[1], "Fear Of Music"), "brackets as well: \"%s\"",
	      n > 1 ? al[1] : "");
	n = museart_tries("X", "(Untitled)", ar, al);
	CHECK(n == 1, "a title that is all parentheses is left whole: %d", n);
}

static void the_sizes(void)
{
	/* The headers only: the size is read from them and nothing past. */
	static const unsigned char png[] = {
		0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R',
		0, 0, 0x01, 0x2C, 0, 0, 0x00, 0xE1,                          /* 300 x 225 */
	};
	static const unsigned char jpg[] = {
		0xFF, 0xD8,
		0xFF, 0xE1, 0x00, 0x08, 'E', 'x', 'i', 'f', 0, 0,            /* APP1, skipped */
		0xFF, 0xDB, 0x00, 0x04, 0, 0,                                /* DQT, skipped */
		0xFF, 0xC0, 0x00, 0x11, 0x08, 0x01, 0xF4, 0x01, 0xF3,        /* SOF0 499 x 500 */
	};
	const char *p = "/tmp/museart-check.img";
	int w = 0, h = 0;
	FILE *f;

	printf("a cover's size, from its header\n");
	f = fopen(p, "wb"); fwrite(png, 1, sizeof png, f); fclose(f);
	CHECK(museart_image_size(p, &w, &h) && w == 300 && h == 225, "PNG: %dx%d", w, h);
	f = fopen(p, "wb"); fwrite(jpg, 1, sizeof jpg, f); fclose(f);
	CHECK(museart_image_size(p, &w, &h) && w == 499 && h == 500,
	      "JPEG, past the segments before its frame: %dx%d", w, h);
	f = fopen(p, "wb"); fputs("GIF89a....................", f); fclose(f);
	CHECK(!museart_image_size(p, &w, &h), "anything else is no size");
	remove(p);
}

/* ---- the run, on stubs -------------------------------------------------- */

/* What the stub network will say, request by request: an HTTP status, 0 for a
 * request that never reached a server, or 200 with `body` (NULL writes a small
 * JPEG). */
typedef struct { int http; const char *body; } answer;
static answer   g_script[16];
static int      g_nscript, g_at, g_http = 200;
static char     g_path[1024], g_urls[16][2400];
static int      g_nurls;
static unsigned g_when[16], g_now;

bool net_get_async(const char *url, const char *path, int timeout_s)
{
	(void)timeout_s;
	if (g_nurls < 16) {
		snprintf(g_urls[g_nurls], sizeof g_urls[0], "%s", url);
		g_when[g_nurls++] = g_now;
	}
	snprintf(g_path, sizeof g_path, "%s", path);
	return true;
}

int net_async_poll(void)
{
	answer a = g_at < g_nscript ? g_script[g_at] : (answer){ 0, NULL };

	g_at++;
	g_http = a.http;
	if (a.http != 200) return -1;
	{
		FILE *f = fopen(g_path, "wb");

		if (a.body) fputs(a.body, f);
		else fwrite("\xFF\xD8\xFF\xD9", 1, 4, f);
		fclose(f);
	}
	return 1;
}

int net_async_http(void) { return g_http; }
void net_async_abort(void) { }

static const char *ONE = "{\"releases\":[{\"score\":100,\"track-count\":3,"
                         "\"release-group\":{\"id\":\"rg-one\"}}]}";

static void run(unsigned step_ms, museart_status *st, int *landed, int nl)
{
	int k = 0;

	while (museart_step(g_now)) {
		museart_status_get(st);
		if (st->landed >= 0 && k < nl) landed[k++] = st->landed;
		g_now += step_ms;
	}
	museart_status_get(st);
}

static void the_run(void)
{
	museart_job jobs[4];
	museart_status st;
	int landed[4] = { -1, -1, -1, -1 }, i;
	char dir[] = "/tmp/museart-check.XXXXXX";

	printf("a run, on a network of stubs\n");
	if (!mkdtemp(dir)) { perror("mkdtemp"); failures++; return; }
	memset(jobs, 0, sizeof jobs);
	for (i = 0; i < 4; i++) {
		snprintf(jobs[i].artist, sizeof jobs[i].artist, "Artist %d", i);
		snprintf(jobs[i].album, sizeof jobs[i].album, "Album %d", i);
		snprintf(jobs[i].base, sizeof jobs[i].base, "%s/a%d/.media/Album %d", dir, i, i);
		jobs[i].tracks = 3;
		jobs[i].id = 10 + i;
		{
			char p[1100];

			snprintf(p, sizeof p, "%s/a%d", dir, i);
			mkdir(p, 0755);
		}
	}

	/* Found; busy then found; found with no front cover; nothing matched. */
	g_nscript = 0;
	g_script[g_nscript++] = (answer){ 200, ONE };
	g_script[g_nscript++] = (answer){ 200, NULL };
	g_script[g_nscript++] = (answer){ 503, NULL };
	g_script[g_nscript++] = (answer){ 200, ONE };
	g_script[g_nscript++] = (answer){ 200, NULL };
	g_script[g_nscript++] = (answer){ 200, ONE };
	g_script[g_nscript++] = (answer){ 404, NULL };
	g_script[g_nscript++] = (answer){ 200, "{\"releases\":[]}" };
	g_at = g_nurls = 0;
	g_now = 5000;

	CHECK(museart_begin(jobs, 4, "/tmp/museart-check.reply"), "a run starts");
	run(50, &st, landed, 4);
	CHECK(st.found == 2 && st.missing == 2 && st.failed == 0,
	      "two found, two missing: %d found, %d missing, %d failed",
	      st.found, st.missing, st.failed);
	CHECK(landed[0] == 10 && landed[1] == 11 && landed[2] == -1,
	      "each cover reported as it lands: %d %d %d", landed[0], landed[1], landed[2]);
	/* Five searches - one asked twice, after the busy reply - and three covers:
	 * the last album matched nothing, so it never asks for one. */
	CHECK(g_nurls == 8, "eight requests, the busy search asked twice: %d", g_nurls);
	for (i = 1; i < g_nurls; i++)
		if (strstr(g_urls[i], "musicbrainz.org"))
			for (int k = i - 1; k >= 0; k--)
				if (strstr(g_urls[k], "musicbrainz.org")) {
					CHECK(g_when[i] - g_when[k] >= 1100,
					      "MusicBrainz is asked no more than once a second: %u ms",
					      g_when[i] - g_when[k]);
					break;
				}
	CHECK(strstr(g_urls[1], "coverartarchive.org/release-group/rg-one/front-500") != NULL,
	      "the cover is the release group's front, at 500: %s", g_urls[1]);
	{
		char p[1100];

		snprintf(p, sizeof p, "%s/a0/.media/Album 0.jpg", dir);
		CHECK(access(p, F_OK) == 0,
		      "written into a .media folder that did not exist before: %s", p);
	}

	/* A miss, and the next way of asking finds it: two searches a second
	 * apart, then the cover. */
	{
		museart_job one = jobs[0];

		snprintf(one.artist, sizeof one.artist, "Son Little");
		snprintf(one.album, sizeof one.album, "Son Little (Deluxe Edition)");
		g_nscript = 0;
		g_script[g_nscript++] = (answer){ 200, "{\"releases\":[]}" };
		g_script[g_nscript++] = (answer){ 200, ONE };
		g_script[g_nscript++] = (answer){ 200, NULL };
		g_at = g_nurls = 0;
		museart_begin(&one, 1, "/tmp/museart-check.reply");
		run(50, &st, landed, 0);
		CHECK(st.found == 1 && st.missing == 0 && g_nurls == 3,
		      "found on the second search: %d found, %d requests", st.found, g_nurls);
		CHECK(g_nurls == 3 && strstr(g_urls[0], "Deluxe") && !strstr(g_urls[1], "Deluxe") &&
		      g_when[1] - g_when[0] >= 1100,
		      "the edition dropped, a second after the first: %s", g_urls[1]);
	}

	/* Three failures in a row is a network that has gone. */
	g_nscript = 0;
	for (i = 0; i < 4; i++) g_script[g_nscript++] = (answer){ 0, NULL };
	g_at = g_nurls = 0;
	museart_begin(jobs, 4, "/tmp/museart-check.reply");
	run(50, &st, landed, 0);
	CHECK(st.failed == 3 && st.problem[0], "stops after three failures: %d, \"%s\"",
	      st.failed, st.problem);

	{
		char cmd[128];

		snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
		if (system(cmd) != 0) { }
	}
}

int main(void)
{
	printf("museart: album covers from MusicBrainz and the Cover Art Archive\n");
	the_rule();
	the_url();
	the_tries();
	the_sizes();
	the_run();
	if (failures) {
		printf("\n%d failure%s\n", failures, failures == 1 ? "" : "s");
		return 1;
	}
	printf("ok\n");
	return 0;
}
