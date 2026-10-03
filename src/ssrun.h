/* SPDX-License-Identifier: MIT */
#ifndef TORTOS_SSRUN_H
#define TORTOS_SSRUN_H

#include <stdbool.h>

#include "ssfetch.h"

/* ---- one game, without blocking a frame ---------------------------------
 *
 * The same work as ss_lookup, driven a step at a time, because the loop that
 * would wait for it is the loop that reads the power button - and a
 * ScreenScraper call measured 5.5 seconds per game. A handheld that stops
 * answering its own power button for five seconds is not slow, it is broken.
 *
 * One at a time: there is a single async slot in net.c, and this needs it for
 * the lookup and then again for the cover.
 *
 * ss_run_step returns 1 while it is working, 0 when it has finished, and -1
 * when it could not - no account, no answer, a name that did not survive the
 * check, or no cover on offer. -1 is the ordinary case for a game they do not
 * have, and it is the caller's signal to fall through to libretro. */
bool ss_run_begin(const char *folder, const char *file, const char *stem,
                  const char *rom_dir, const char *exts);
int  ss_run_step(void);
void ss_run_cancel(void);

/* What the run learned, valid once ss_run_step has returned 0. */
const ss_result *ss_run_result(void);

/* Where it got to, for a screen to say. Never a URL: that carries the
 * account. */
const char *ss_run_where(void);

/* ---- what a long run needs to know when to stop -------------------------
 *
 * Requests left on the account today, or -1 until a reply has said. Every
 * reply carries the account's own counters, so this costs nothing and is the
 * only number that is actually true - the day's 20,000 is shared with any
 * other device signed in as the same person, and with the same card scraped
 * twice.
 *
 * Survives ss_run_cancel: it is a fact about the account, not about the game
 * that was being looked up. */
int ss_run_left(void);

/* Whether the last refusal was "not now" rather than "not this game": HTTP
 * 429. The two arrive identically otherwise - curl runs with `fail`, so every
 * refusal is one exit code and no body - and they want opposite responses. A
 * 429 is worth waiting a beat and asking again; a 404 means they do not have
 * this game and asking again would be rude and useless. */
bool ss_run_too_many(void);

#endif
