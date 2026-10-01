/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* Whole scraper runs, against a stubbed network.
 *
 *     make check-artrun
 *
 * The real src/artscrape.c, driven step by step to the end of a run with no
 * device and no download, for the two things its passes can get silently
 * wrong.
 *
 * REPLACE. It used to delete the cover and then run the scrape to fill the gap.
 * For a game libretro has no art for - a translation, homebrew, a cover added
 * by hand - there was nothing to fill it with, and the cover was simply gone.
 * Now it runs over that one game without deleting anything, and a download is
 * renamed over the old cover only once it has arrived. Pinned: a cover libretro
 * has replaces the old one, one it has not leaves the old one exactly as it was,
 * and a one-game run neither touches another game nor turns an over-long name
 * into a whole-system scrape.
 *
 * THE CHECKSUM PASS. For a game no name matched, the CRC in its zip names it
 * through No-Intro's list, and that name is matched against the catalog. A
 * wrong CRC is not an error anyone sees, it is a different game's cover, so
 * pinned: the ROM inside a zip is named rather than a readme beside it, a CRC
 * the list does not have stays missing, and a list that will not download
 * leaves the run finishing as it did before the pass existed.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>

#include "../src/artscrape.h"
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

/* The real one needs SDL_image, and nothing here is about resizing. */
void art_shrink(const char *path);
void art_shrink(const char *path) { (void)path; }

/* ---- the network, stubbed ------------------------------------------------
 *
 * The same contract as src/net.c: a GET is started and polled, and the file
 * appears at `path` only when the poll says 1 - written beside it first and
 * renamed over it, which is what makes a replace atomic. A request for anything
 * not being served answers -1 and leaves nothing behind. */
static const char *g_serve_image;    /* "Hit%20Game" is served, or NULL */
static const char *g_serve_also;     /* and a second, or NULL */
static const char *g_serve_dat;      /* the No-Intro list, or NULL for a 404 */
static const char *INDEX_OTHER =
	"<a href=\"Unrelated%20Title%20%28USA%29.png\">x</a>\n";
static const char *g_index = NULL;   /* the catalog; INDEX_OTHER when NULL */
static char g_pend_url[2048], g_pend_path[2048];
static bool g_pending;
static char g_urls[4096];            /* every URL asked for, one per line */
static int  g_nreq;

bool net_get_async(const char *url, const char *path, int timeout_s)
{
	(void)timeout_s;
	snprintf(g_pend_url, sizeof g_pend_url, "%s", url);
	snprintf(g_pend_path, sizeof g_pend_path, "%s", path);
	g_pending = true;
	g_nreq++;
	if (strlen(g_urls) + strlen(url) + 2 < sizeof g_urls) {
		strcat(g_urls, url);
		strcat(g_urls, "\n");
	}
	return true;
}

static bool write_file(const char *path, const char *text)
{
	FILE *f = fopen(path, "wb");

	if (!f) return false;
	fputs(text, f);
	return fclose(f) == 0;
}

int net_async_poll(void)
{
	char part[2100];
	const char *body = NULL;
	size_t n = strlen(g_pend_url);

	if (!g_pending) return -1;
	g_pending = false;

	if (n && g_pend_url[n - 1] == '/') {
		/* The catalog: by default other games only, so a fuzzy pass finds
		 * nothing. */
		body = g_index ? g_index : INDEX_OTHER;
	} else if (n > 4 && !strcmp(g_pend_url + n - 4, ".dat")) {
		body = g_serve_dat;
	} else if ((g_serve_image && strstr(g_pend_url, g_serve_image)) ||
	           (g_serve_also && strstr(g_pend_url, g_serve_also))) {
		body = "NEW";
	}
	if (!body) return -1;

	snprintf(part, sizeof part, "%s.part", g_pend_path);
	if (!write_file(part, body)) return -1;
	return rename(part, g_pend_path) == 0 ? 1 : -1;
}

void net_async_abort(void) { g_pending = false; }
int  net_async_http(void)  { return 200; }
int  net_async_ms(void)    { return 0; }
int  net_async_exit(void)  { return 0; }

/* ---- ScreenScraper, stubbed the same way ---------------------------------
 *
 * The real client is four calls with a step machine behind them and a whole
 * second network path; what this file needs to hold is the RUN's behavior
 * around it - that a hit costs libretro nothing, that a miss falls through,
 * and that a refusal stops the pass instead of hammering the server. So the
 * client is the thing stubbed, and the rules under test stay real. */
static bool g_ss_on;          /* is there an account */
static const char *g_ss_has;  /* the one stem they have a cover for, or NULL */
static int  g_ss_busy;        /* how many more lookups answer 429 */
static int  g_ss_left = -1;   /* what the account says is left today */
static int  g_ss_asked;       /* lookups started */
static char g_ss_stem[256];
static char g_ss_dest[512];
static bool g_ss_toomany;

bool ss_signed_in(void) { return g_ss_on; }

bool ss_run_begin(const char *folder, const char *file, const char *stem,
                  const char *rom_dir, const char *exts)
{
	(void)folder; (void)file; (void)exts;
	if (!g_ss_on) return false;
	snprintf(g_ss_stem, sizeof g_ss_stem, "%s", stem);
	snprintf(g_ss_dest, sizeof g_ss_dest, "%s/.media/%s.png", rom_dir, stem);
	g_ss_asked++;
	return true;
}

int ss_run_step(void)
{
	g_ss_toomany = false;
	if (g_ss_busy > 0) { g_ss_busy--; g_ss_toomany = true; return -1; }
	if (!g_ss_has || strcmp(g_ss_has, g_ss_stem)) return -1;
	return write_file(g_ss_dest, "SS") ? 0 : -1;
}

void ss_run_cancel(void) { g_ss_stem[0] = '\0'; }
int  ss_run_left(void) { return g_ss_left; }
bool ss_run_too_many(void) { return g_ss_toomany; }

/* ---- the card, in /tmp --------------------------------------------------- */

static char g_root[256], g_shelf[320], g_media[400];

static void make_card(void)
{
	char p[512];

	snprintf(g_root, sizeof g_root, "/tmp/tortos-artrun-%ld", (long)getpid());
	snprintf(g_shelf, sizeof g_shelf, "%s/Game Boy", g_root);
	snprintf(g_media, sizeof g_media, "%s/.media", g_shelf);
	mkdir(g_root, 0755);
	mkdir(g_shelf, 0755);
	mkdir(g_media, 0755);
	snprintf(p, sizeof p, "%s/Hit Game.gb", g_shelf);    write_file(p, "rom");
	snprintf(p, sizeof p, "%s/Other Game.gb", g_shelf);  write_file(p, "rom");
}

static void set_cover(const char *text)
{
	char p[512];

	snprintf(p, sizeof p, "%s/Hit Game.png", g_media);
	write_file(p, text);
}

static const char *cover(void)
{
	static char buf[64];
	char p[512];
	FILE *f;
	size_t n;

	snprintf(p, sizeof p, "%s/Hit Game.png", g_media);
	if (!(f = fopen(p, "rb"))) return "(gone)";
	n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = '\0';
	return buf;
}

/* Any game's cover, by stem. */
static const char *cover_of(const char *stem)
{
	static char buf[64];
	char p[512];
	FILE *f;
	size_t n;

	snprintf(p, sizeof p, "%s/%s.png", g_media, stem);
	if (!(f = fopen(p, "rb"))) return "(none)";
	n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = '\0';
	return buf;
}

static void put16(FILE *f, unsigned v)
{
	fputc((int)(v & 255), f);
	fputc((int)((v >> 8) & 255), f);
}

static void put32(FILE *f, unsigned long v)
{
	put16(f, (unsigned)(v & 0xFFFF));
	put16(f, (unsigned)((v >> 16) & 0xFFFF));
}

/* A stored zip holding `n` four-byte files. The CRCs are written as given, not
 * computed: the pass reads what the zip records and never hashes, so the test
 * can choose them. Local headers, central directory and end record, laid out
 * as the format says. */
static void write_zip(const char *path, int n, const char **names,
                      const unsigned long *crcs)
{
	FILE *f = fopen(path, "wb");
	long offs[8], cd, end;
	int i;

	if (!f) return;
	for (i = 0; i < n; i++) {
		offs[i] = ftell(f);
		put32(f, 0x04034b50UL);
		put16(f, 20); put16(f, 0); put16(f, 0); put16(f, 0); put16(f, 0);
		put32(f, crcs[i]); put32(f, 4); put32(f, 4);
		put16(f, (unsigned)strlen(names[i])); put16(f, 0);
		fputs(names[i], f);
		fputs("data", f);
	}
	cd = ftell(f);
	for (i = 0; i < n; i++) {
		put32(f, 0x02014b50UL);
		put16(f, 20); put16(f, 20); put16(f, 0); put16(f, 0); put16(f, 0); put16(f, 0);
		put32(f, crcs[i]); put32(f, 4); put32(f, 4);
		put16(f, (unsigned)strlen(names[i]));
		put16(f, 0); put16(f, 0); put16(f, 0); put16(f, 0);
		put32(f, 0); put32(f, (unsigned long)offs[i]);
		fputs(names[i], f);
	}
	end = ftell(f);
	put32(f, 0x06054b50UL);
	put16(f, 0); put16(f, 0); put16(f, (unsigned)n); put16(f, (unsigned)n);
	put32(f, (unsigned long)(end - cd)); put32(f, (unsigned long)cd);
	put16(f, 0);
	fclose(f);
}

static bool part_left(void)
{
	char p[512];
	struct stat st;

	snprintf(p, sizeof p, "%s/Hit Game.png.part", g_media);
	return stat(p, &st) == 0;
}

/* A whole run, to the end. Bounded, so a step machine that never finishes is a
 * failing check rather than a hung one. */
static int run(const char *only, art_progress *out)
{
	systems_cfg sys;
	int steps = 0, r;

	memset(&sys, 0, sizeof sys);
	sys.count = 1;
	snprintf(sys.systems[0].folder, sizeof sys.systems[0].folder, "Game Boy");
	snprintf(sys.systems[0].exts, sizeof sys.systems[0].exts, "gb,zip");

	g_urls[0] = '\0';
	g_nreq = 0;
	art_begin(&sys, g_root, NULL, only);
	while ((r = art_step()) == 1 && steps < 1000) steps++;
	art_status(out);
	return r;
}

int main(void)
{
	art_progress st;
	char big[1024];

	printf("artrun: whole scraper runs, against a stubbed network\n");
	make_card();

	printf("  a cover libretro has replaces the old one:\n");
	set_cover("OLD");
	g_serve_image = "Hit%20Game";
	CHECK(run("Hit Game", &st) == 0, "the run did not finish");
	CHECK(!strcmp(cover(), "NEW"), "the cover reads \"%s\", wanted NEW", cover());
	CHECK(st.found == 1, "found %d, wanted 1", st.found);
	CHECK(!strstr(g_urls, "Other%20Game"),
	      "a one-game replace asked about another game:\n%s", g_urls);
	CHECK(!part_left(), "a part-file was left beside the cover");

	printf("  a cover libretro has not got leaves the old one alone:\n");
	set_cover("OLD");
	g_serve_image = NULL;
	CHECK(run("Hit Game", &st) == 0, "the run did not finish");
	CHECK(!strcmp(cover(), "OLD"),
	      "the cover reads \"%s\" - a miss must keep the one it had", cover());
	CHECK(st.found == 0, "found %d on a miss", st.found);
	CHECK(strstr(g_urls, "Hit%20Game.png") != NULL,
	      "the game was never asked for, so the miss proves nothing:\n%s", g_urls);
	CHECK(!part_left(), "a part-file was left beside the cover");

	printf("  the ordinary run still skips a game that has art:\n");
	set_cover("OLD");
	g_serve_image = NULL;
	CHECK(run(NULL, &st) == 0, "the run did not finish");
	CHECK(!strstr(g_urls, "Hit%20Game"),
	      "the ordinary run asked for a game that already has art:\n%s", g_urls);
	CHECK(strstr(g_urls, "Other%20Game") != NULL,
	      "the ordinary run did not ask for the game with no art:\n%s", g_urls);
	CHECK(!strcmp(cover(), "OLD"), "the ordinary run changed a cover it skipped");

	printf("  a name too long to hold is refused, not widened to the shelf:\n");
	memset(big, 'x', sizeof big - 1);
	big[sizeof big - 1] = '\0';
	CHECK(run(big, &st) == 0, "the run did not finish");
	CHECK(g_nreq == 0, "%d request(s) for a refused name:\n%s", g_nreq, g_urls);

	/* ---- the checksum pass ---------------------------------------------- */
	{
		const char *names[] = { "readme.txt", "Old Name (USA).gb" };
		const unsigned long crcs[] = { 0xDEADBEEFUL, 0x1234ABCDUL };
		char p[512];
		/* The readme's CRC names a game the catalog also has. Taking the
		 * first entry in the zip would fetch that one - a wrong cover, which
		 * is exactly the failure nothing on screen would report. */
		const char *LIST =
			"clrmamepro (\n\tname \"Nintendo - Game Boy\"\n)\n\n"
			"game (\n\tname \"Readme Decoy (USA)\"\n"
			"\trom ( name \"Readme Decoy (USA).gb\" size 4 crc DEADBEEF md5 00 )\n)\n"
			"game (\n\tname \"New Name - Subtitle (USA)\"\n"
			"\trom ( name \"New Name - Subtitle (USA).gb\" size 4 crc 1234ABCD md5 00 )\n)\n";
		const char *CATALOG =
			"<a href=\"Readme%20Decoy%20%28USA%29.png\">x</a>\n"
			"<a href=\"New%20Name%20-%20Subtitle%20%28USA%29.png\">x</a>\n";

		snprintf(p, sizeof p, "%s/Old Name (USA).zip", g_shelf);
		write_zip(p, 2, names, crcs);
		g_index = CATALOG;

		printf("  a zip no name reaches is named by the ROM's checksum:\n");
		g_serve_dat = LIST;
		/* Both candidates are served, so only the name chosen decides which
		 * cover lands - and the file's own name is not, so it cannot be found
		 * by name first. */
		g_serve_image = "New%20Name%20-%20Subtitle";
		g_serve_also = "Readme%20Decoy";
		CHECK(run(NULL, &st) == 0, "the run did not finish");
		CHECK(strstr(g_urls, "metadat/no-intro/Nintendo%20-%20Game%20Boy.dat") != NULL,
		      "the No-Intro list was never asked for:\n%s", g_urls);
		CHECK(strstr(g_urls, "New%20Name%20-%20Subtitle%20%28USA%29.png") != NULL,
		      "the cover No-Intro names for the ROM was not fetched:\n%s", g_urls);
		CHECK(!strstr(g_urls, "Readme%20Decoy"),
		      "the readme's checksum named the game:\n%s", g_urls);
		CHECK(!strcmp(cover_of("Old Name (USA)"), "NEW"),
		      "the cover reads \"%s\", wanted NEW", cover_of("Old Name (USA)"));

		printf("  a checksum the list does not have stays missing:\n");
		snprintf(p, sizeof p, "%s/Old Name (USA).png", g_media);
		remove(p);
		g_serve_dat = "clrmamepro (\n\tname \"Nintendo - Game Boy\"\n)\n";
		CHECK(run(NULL, &st) == 0, "the run did not finish");
		CHECK(!strcmp(cover_of("Old Name (USA)"), "(none)"),
		      "a cover appeared for a checksum nobody lists");
		CHECK(!strstr(g_urls, "New%20Name"), "a cover was fetched with no name to go on");

		printf("  a list that will not download leaves the run as it was:\n");
		g_serve_dat = NULL;
		CHECK(run(NULL, &st) == 0, "the run did not finish");
		CHECK(!strcmp(cover_of("Old Name (USA)"), "(none)"),
		      "a cover appeared with no list to name it");
		CHECK(st.missing >= 2, "missing %d - the unnamed games were not counted",
		      st.missing);
		g_index = NULL;
		g_serve_also = NULL;
	}

	/* ---- the second source, which goes first ---------------------------
	 *
	 * Four rules, and the last two are the ones that had to exist before a run
	 * over 1,708 games could: a refusal that means "not now" is worth one more
	 * ask, and a day that is spent is worth none. */
	{
		char p[512];

		printf("  ScreenScraper goes first, and libretro is never asked:\n");
		snprintf(p, sizeof p, "%s/Hit Game.png", g_media);  remove(p);
		snprintf(p, sizeof p, "%s/Other Game.png", g_media); remove(p);
		g_ss_on = true;
		g_ss_has = "Hit Game";
		g_ss_busy = 0;
		g_ss_left = -1;
		g_ss_asked = 0;
		g_serve_image = "Hit%20Game";
		CHECK(run(NULL, &st) == 0, "the run did not finish");
		CHECK(!strcmp(cover_of("Hit Game"), "SS"),
		      "the cover reads \"%s\", wanted the ScreenScraper one",
		      cover_of("Hit Game"));
		CHECK(!strstr(g_urls, "Hit%20Game"),
		      "libretro was asked for a cover ScreenScraper had already:\n%s",
		      g_urls);

		printf("  a game they have not got falls through to libretro:\n");
		snprintf(p, sizeof p, "%s/Hit Game.png", g_media);  remove(p);
		g_ss_has = NULL;
		CHECK(run(NULL, &st) == 0, "the run did not finish");
		CHECK(!strcmp(cover_of("Hit Game"), "NEW"),
		      "the cover reads \"%s\", wanted libretro's", cover_of("Hit Game"));

		printf("  \"too many at once\" is asked again, once:\n");
		snprintf(p, sizeof p, "%s/Hit Game.png", g_media);  remove(p);
		g_ss_has = "Hit Game";
		g_ss_busy = 1;                  /* refuse the first ask only */
		g_ss_asked = 0;
		CHECK(run(NULL, &st) == 0, "the run did not finish");
		CHECK(!strcmp(cover_of("Hit Game"), "SS"),
		      "a 429 was not retried: the cover reads \"%s\"",
		      cover_of("Hit Game"));
		CHECK(g_ss_asked >= 2, "asked %d times, wanted the game asked again",
		      g_ss_asked);

		printf("  refused twice, it stops and leaves the rest to libretro:\n");
		snprintf(p, sizeof p, "%s/Hit Game.png", g_media);  remove(p);
		snprintf(p, sizeof p, "%s/Other Game.png", g_media); remove(p);
		g_ss_busy = 99;                 /* refuse everything */
		g_ss_asked = 0;
		g_serve_image = "Hit%20Game";
		CHECK(run(NULL, &st) == 0, "the run did not finish");
		CHECK(g_ss_asked == 2, "asked %d times, wanted two and then a stop",
		      g_ss_asked);
		CHECK(!strcmp(cover_of("Hit Game"), "NEW"),
		      "libretro did not finish the job after the pass stopped");
		CHECK(strstr(st.problem, "too many") != NULL,
		      "the run did not say why it stopped: \"%s\"", st.problem);

		printf("  a spent day is not asked at all:\n");
		snprintf(p, sizeof p, "%s/Hit Game.png", g_media);  remove(p);
		g_ss_busy = 0;
		g_ss_left = 10;                 /* under SS_DAY_FLOOR */
		g_ss_asked = 0;
		CHECK(run(NULL, &st) == 0, "the run did not finish");
		CHECK(g_ss_asked == 0, "asked %d times with the day spent", g_ss_asked);
		CHECK(!strcmp(cover_of("Hit Game"), "NEW"),
		      "libretro did not cover for the spent quota");
		CHECK(strstr(st.problem, "quota") != NULL,
		      "the run did not say the quota was spent: \"%s\"", st.problem);

		g_ss_on = false;
		g_ss_left = -1;
	}

	printf("  an arcade set is asked for by FBNeo's name and kept under its own:\n");
	{
		char shelf[400], media[420], dat[400], p[512];
		systems_cfg sys;
		int steps = 0, r;
		FILE *f;

		snprintf(shelf, sizeof shelf, "%s/Arcade", g_root);
		snprintf(media, sizeof media, "%s/.media", shelf);
		mkdir(shelf, 0755);
		mkdir(media, 0755);
		snprintf(p, sizeof p, "%s/mk3.zip", shelf);   write_file(p, "rom");
		snprintf(p, sizeof p, "%s/1943.zip", shelf);  write_file(p, "rom");
		snprintf(dat, sizeof dat, "%s/fbneo-titles.tsv", g_root);
		write_file(dat, "1943\t1943: The Battle of Midway (Euro)\n"
		                "mk3\tMortal Kombat 3 (rev 2.1)\n");

		memset(&sys, 0, sizeof sys);
		sys.count = 1;
		snprintf(sys.systems[0].folder, sizeof sys.systems[0].folder, "Arcade");
		snprintf(sys.systems[0].exts, sizeof sys.systems[0].exts, "zip");
		snprintf(sys.systems[0].core, sizeof sys.systems[0].core, "fbneo");
		g_serve_image = "/Mortal%20Kombat%203%20";
		g_serve_also = "/1943_%20The%20Battle%20of%20Midway";
		g_index = NULL;
		g_urls[0] = '\0';
		g_nreq = 0;
		art_begin(&sys, g_root, dat, NULL);
		while ((r = art_step()) == 1 && steps < 1000) steps++;
		CHECK(r == 0, "the run did not finish");
		snprintf(p, sizeof p, "%s/mk3.png", media);
		f = fopen(p, "rb");
		CHECK(f != NULL, "no cover saved as mk3.png:\n%s", g_urls);
		if (f) fclose(f);
		snprintf(p, sizeof p, "%s/1943.png", media);
		f = fopen(p, "rb");
		CHECK(f != NULL, "a colon in the name was not asked for as _:\n%s", g_urls);
		if (f) fclose(f);
		CHECK(!strstr(g_urls, "/mk3.png"),
		      "asked libretro for the set name, which it never files under:\n%s", g_urls);
		g_serve_image = g_serve_also = NULL;
	}

	{
		char cmd[400];
		snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_root);
		if (system(cmd) != 0) printf("  (could not remove %s)\n", g_root);
	}

	if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
	printf("\nok: every check passed\n");
	return 0;
}
