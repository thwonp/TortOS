/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef TORTOS_ARTSCRAPE_H
#define TORTOS_ARTSCRAPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"

/* Box art, from libretro's thumbnail collection.
 *
 * A port of tools/scrape-art.py, which is still the way to fill a card from a
 * host. The rules are the tool's and were measured there over 178 ROMs on
 * 2026-08-29; carrying them over exactly is the point, because the tempting
 * simplification is the one that loses a sixth of the library:
 *
 *     exact filename only                       148/178   83%
 *     plus stripping (Translated) and friends   158/178   88%
 *     fetch the index, match normalized titles  174/178   97%
 *
 * MATCHING IS AGAINST THE DIRECTORY INDEX, NOT BY GUESSING FILENAMES. Guessing
 * cannot find what it does not know to guess: Master System sat at 9 of 20
 * under every variant scheme, because libretro carries `Sonic The Hedgehog
 * (USA, Europe, Brazil) (En)` and the card has the same game without the
 * `(En)`. One index fetch shows that immediately and costs nine requests for
 * the whole library rather than one probe per ROM per guess.
 *
 * No account, no API key. That is why this source was chosen: ScreenScraper
 * refuses every call without a devid it issues by hand, and TheGamesDB now
 * refuses keyless requests outright.
 *
 * The four it cannot match are fan translations - romhacks, absent from any
 * No-Intro-derived database under those names. No provider has them and they
 * want art supplied by hand, which is what Over The Hare is for.
 *
 * AND WHERE NO NAME MATCHES, THE CHECKSUM. A file can carry a name the catalog
 * no longer uses - an older No-Intro name, a GoodTools one - and then no name
 * rule finds a cover libretro does have. A zip's own record of its ROM's CRC
 * names the dump in No-Intro's list, and No-Intro's names are what libretro
 * files its covers under. Measured 2026-09-14 on the main card: 29 more covers,
 * every one the right game, and all 50 Neo Geo Pocket misses turned out to be
 * names, not missing art.
 */

/* One index fetch per system, then one download per game that has no art. Art
 * that is already there costs nothing: it is skipped without a request, which
 * is what makes running this again cheap and a canceled run free to restart.
 *
 * Driven from a frame loop, one art_step per frame. No step waits on the
 * network: each starts a request or polls the one already running, so the
 * loop calling it goes on reading the power button - see art_step.
 *
 * The library is passed in rather than reached for. Nothing here should know
 * how the launcher stores its config, and a scraper that reads a global is a
 * scraper no check can hand a fixture to.
 *
 * `only` is NULL for the ordinary run. Given a ROM's name without its
 * extension, the run is that one game and nothing else, fetched whether or not
 * it already has a cover. It never deletes: a download lands beside the old
 * cover and is renamed over it only once it has arrived whole, so a game
 * libretro has no art for - a translation, homebrew, a cover added by hand -
 * keeps the one it had. Replace used to delete first and fetch after, and lost
 * exactly those.
 *
 * `fbneo_dat` is FBNeo's set table (fbneodat.h), or NULL. A shelf whose core
 * is fbneo has zips named for their sets, and libretro files that art under
 * FBNeo's description of the set instead, so the table is what it is asked
 * by. The cover is still saved under the set name, where the shelf looks. */
void art_begin(const systems_cfg *sys, const char *roms_dir, const char *fbneo_dat,
               const char *only);
void art_cancel(void);

/* One unit of work, so the caller can draw between them. Returns:
 *
 *    1  still going    - `now` says what it is doing
 *    0  finished
 *   -1  nothing started, or it could not begin at all
 *
 * A step starts one request - an index or an image - or polls the one in
 * flight, and never waits for it; the transfer runs in curl while the caller
 * draws. What a step can still cost is local work: parsing an index once it
 * has arrived, or checking whether a game's art is already on the card. */
int art_step(void);

/* What to put on screen. `now` is a game or a system name, never a URL. */
typedef struct {
	int  systems, systems_done;
	int  found, missing, skipped;
	char now[128];       /* what it is doing: a game, or a system */
	/* Why a whole system was passed over, if one was. Kept apart from `now`
	 * because the two are wanted at different times: `now` is progress and is
	 * meaningless once the run ends, while this is the only thing worth
	 * saying afterwards. Showing `now` at the end left the panel naming the
	 * last game it fetched, which reads as still working on it. */
	char problem[128];
} art_progress;

void art_status(art_progress *out);

/* The matching rule, exposed because it IS the feature.
 *
 * Every percentage in the comment above rests on this one function agreeing
 * with tools/scrape-art.py's norm(). A port that drifts here does not fail -
 * it quietly finds a sixth fewer games, which looks exactly like libretro
 * having less art than it does. tools/artscrape-check.c runs both over the
 * real library and compares, the same way the two ROM hashers are checked
 * against each other. */
void art_norm(const char *in, char *out, size_t outn);

/* How well `cand`'s region and language tags match `want`'s. Higher is better
 * and it may go negative.
 *
 * Exposed because it decides which of several identical-looking candidates a
 * game gets, and getting that wrong is silent: 1703 of the 13418 NES entries
 * normalize to a title some other entry also normalizes to, so a US dump could
 * be handed Japanese box art and nothing would say so. tools/artscrape-check.c
 * asserts the orderings that matter. */
int art_tag_score(const char *want, const char *cand);

/* The CRC32 of the ROM inside <dir>/<stem>.zip, as the zip itself records it.
 *
 * Exposed because ScreenScraper wants the same number and one implementation
 * of it is enough. It reads the central directory, so nothing is decompressed
 * and nothing is hashed - a few kilobytes, which is why it can run inside a
 * frame. The entry taken is the first the shelf's extensions allow, so a zip
 * carrying a readme beside the ROM is still named by the ROM.
 *
 * Zips only. 1,680 of this card's ROMs are zips and a loose file would have to
 * be read whole; false for one, which the caller treats as "ask by name". */
bool art_rom_crc(const char *dir, const char *stem, const char *exts,
                 uint32_t *crc);

#endif
