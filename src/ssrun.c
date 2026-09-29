/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* See ssrun.h. Apart from ssfetch.c on purpose: this half reaches for the
 * card, the shrinker and the shelf's paths, while that half is the reading and
 * the judging and is linked by tools/ss-check.c with no SDL anywhere near it.
 * ADR-0001 is what that seam is for. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "ssrun.h"

#include "artscrape.h"
#include "artshrink.h"
#include "db.h"
#include "library.h"
#include "net.h"
#include "ss.h"


#define SS_TMP "/tmp/tortos-ss-reply.json"

/* Sized from what actually goes in them, the way artscrape.c sizes its own.
 * A destination sized "like the directory" is how the cross compiler came to
 * point out that the middle of the path could not fit - clang says nothing
 * about any of this, so the device build is the only place it shows up. */
#define SS_DIR_MAX  (LIB_PATH * 2)
#define SS_DEST_MAX (SS_DIR_MAX + LIB_PATH + 32)

static struct {
	enum { R_OFF, R_ASK, R_RETRY, R_ART, R_DONE, R_FAIL } phase;
	char      folder[64], file[LIB_PATH], stem[LIB_PATH];
	char      dir[SS_DIR_MAX], dest[SS_DEST_MAX];
	uint32_t  crc;
	ss_result got;
	char      where[64];
} g_run;

/* Outside g_run on purpose: the account's day is not the current game's
 * business and must survive a cancel. */
static int  g_left = -1;
static bool g_toomany;

const ss_result *ss_run_result(void) { return &g_run.got; }
const char      *ss_run_where(void)  { return g_run.where; }
int              ss_run_left(void)   { return g_left; }
bool             ss_run_too_many(void) { return g_toomany; }

void ss_run_cancel(void)
{
	if (g_run.phase == R_ASK || g_run.phase == R_RETRY || g_run.phase == R_ART)
		net_async_abort();
	remove(SS_TMP);
	memset(&g_run, 0, sizeof g_run);
}

/* Start one lookup, by checksum when there is one. */
static bool start_ask(uint32_t crc)
{
	char url[1024];
	bool ok;

	if (!ss_lookup_url(url, sizeof url, g_run.folder, g_run.file, crc))
		return false;
	remove(SS_TMP);
	ok = net_get_async(url, SS_TMP, SS_WAIT_S);
	memset(url, 0, sizeof url);          /* it carried both credentials */
	return ok;
}

bool ss_run_begin(const char *folder, const char *file, const char *stem,
                  const char *rom_dir, const char *exts)
{
	memset(&g_run, 0, sizeof g_run);
	g_toomany = false;
	if (!ss_signed_in() || !folder || !file || !stem) return false;
	if (ss_system_id(folder) == 0) return false;

	snprintf(g_run.folder, sizeof g_run.folder, "%s", folder);
	snprintf(g_run.file, sizeof g_run.file, "%s", file);
	snprintf(g_run.stem, sizeof g_run.stem, "%s", stem);
	/* The system's directory, handed in rather than built from platform.h:
	 * this file has no business knowing where the card keeps its ROMs, and
	 * that is also what keeps it linkable without SDL. */
	snprintf(g_run.dir, sizeof g_run.dir, "%s", rom_dir ? rom_dir : ".");
	/* A loose file has no central directory to read, so it is asked about by
	 * name alone - the same path a disc image takes. */
	if (!art_rom_crc(rom_dir, stem, exts, &g_run.crc)) g_run.crc = 0;

	snprintf(g_run.where, sizeof g_run.where, "looking it up");
	if (!start_ask(g_run.crc)) return false;
	g_run.phase = R_ASK;
	return true;
}

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* One line per request, for the log: the game, how it was asked, what came
 * back, how big and how long, and what was made of it. Never the URL, which
 * carries both credentials.
 *
 * The scraper used to say nothing at all, which is how twelve of the most
 * famous games on a fresh card came back with covers and no text and nothing
 * in the log to say why. BACKLOG 47. */
static void say_reply(const char *how, long bytes, int parse_ms, const char *what)
{
	char curl[24] = "";

	if (net_async_http() == 0)
		snprintf(curl, sizeof curl, ", curl %d", net_async_exit());
	fprintf(stderr, "ss: %s by %s: HTTP %d%s, %ld bytes, %d ms, read in %d ms - %s\n",
	        g_run.file, how, net_async_http(), curl, bytes, net_async_ms(),
	        parse_ms, what);
}

/* The reply file, into g_run.got. `bytes` takes its size and `parse_ms` the
 * time spent reading it, both for the log: a reply over the limit is refused
 * here, and reading one runs inside a frame. */
static bool read_reply(long *bytes, int *parse_ms)
{
	char *body;
	FILE *f = fopen(SS_TMP, "rb");
	long n = 0, t0 = now_ms();
	bool ok = false;

	*bytes = 0;
	*parse_ms = 0;
	if (!f) return false;
	if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0) *bytes = n;
	if (n > 0 && n < SS_REPLY_MAX &&
	    fseek(f, 0, SEEK_SET) == 0 && (body = malloc((size_t)n + 1))) {
		if (fread(body, 1, (size_t)n, f) == (size_t)n) {
			body[n] = '\0';
			ok = ss_parse(body, (size_t)n, g_run.file, &g_run.got);
			/* Even a reply about the wrong game states the account's day. */
			if (g_run.got.max_today > 0)
				g_left = g_run.got.max_today - g_run.got.used_today;
		}
		free(body);
	}
	fclose(f);
	remove(SS_TMP);
	*parse_ms = (int)(now_ms() - t0);
	return ok;
}

int ss_run_step(void)
{
	int poll;

	switch (g_run.phase) {
	case R_OFF:  return -1;
	case R_DONE: return 0;
	case R_FAIL: return -1;

	case R_ASK:
	case R_RETRY: {
		const char *how = (g_run.phase == R_RETRY || !g_run.crc) ? "name" : "checksum";
		bool again = g_run.phase == R_ASK && g_run.crc;
		char what[200];
		long bytes;
		int  parse_ms, http;

		poll = net_async_poll();
		if (poll == 0) return 1;                     /* still in flight */
		if (poll < 0) {
			http = net_async_http();
			g_toomany = (http == 429);
			say_reply(how, 0, 0, http == 429 ? "too many requests"
			                   : http == 404 ? "not a game they know"
			                   : http == 0   ? "no answer" : "refused");
			g_run.phase = R_FAIL;
			return -1;
		}
		if (!read_reply(&bytes, &parse_ms) || !g_run.got.found) {
			say_reply(how, bytes, parse_ms,
			          bytes >= SS_REPLY_MAX ? "too big, not read"
			          : bytes == 0 ? "empty" : "no game in it");
			g_run.phase = R_FAIL;
			return -1;
		}
		/* A checksum answer that fails the check is asked again by name; see
		 * ss_lookup for the measurement that says why. Once only - the retry
		 * has already been asked the only other way there is. */
		if (!g_run.got.name_ok) {
			snprintf(what, sizeof what, "\"%s\" fails the name check%s",
			         g_run.got.name, again ? ", asking by name" : "");
			say_reply(how, bytes, parse_ms, what);
			if (again) {
				snprintf(g_run.where, sizeof g_run.where, "asking by name");
				if (start_ask(0)) { g_run.phase = R_RETRY; return 1; }
			}
			/* The wrong game. The text may still be worth keeping, but the
			 * caller asked for art and the answer is no; libretro gets its
			 * turn. */
			g_run.phase = R_FAIL;
			return -1;
		}
		snprintf(what, sizeof what, "\"%s\"%s", g_run.got.name,
		         g_run.got.art[0] ? "" : ", no cover");
		say_reply(how, bytes, parse_ms, what);
		if (!g_run.got.art[0]) {
			g_run.phase = R_FAIL;
			return -1;
		}
		snprintf(g_run.dest, sizeof g_run.dest, "%s/.media/%s.png",
		         g_run.dir, g_run.stem);
		snprintf(g_run.where, sizeof g_run.where, "fetching the cover");
		if (!net_get_async(g_run.got.art, g_run.dest, 60)) {
			g_run.phase = R_FAIL;
			return -1;
		}
		g_run.phase = R_ART;
		return 1;
	}

	case R_ART: {
		struct stat st;

		poll = net_async_poll();
		if (poll == 0) return 1;
		fprintf(stderr, "ss: %s cover: HTTP %d, %ld bytes, %d ms%s\n",
		        g_run.file, net_async_http(),
		        poll > 0 && stat(g_run.dest, &st) == 0 ? (long)st.st_size : 0L,
		        net_async_ms(), poll > 0 ? "" : " - failed");
		if (poll < 0) { g_run.phase = R_FAIL; return -1; }
		/* Down to the size a card is ever drawn at, the same as a libretro
		 * cover: art_shrink queues it and returns. */
		art_shrink(g_run.dest);
		/* The text goes in with it. A scrape that fetched a cover and threw
		 * the year and the synopsis away would have to be run again to get
		 * them, and this is the only moment they are in hand. */
		db_game_set(db_lib(), g_run.folder, g_run.file, &g_run.got.meta);
		g_run.phase = R_DONE;
		return 0;
	}
	}
	return -1;
}
