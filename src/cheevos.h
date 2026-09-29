/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef TORTOS_CHEEVOS_H
#define TORTOS_CHEEVOS_H

#include <stdbool.h>
#include <stddef.h>

/* RetroAchievements, the launcher's half.
 *
 * The division is Diatom's ADR-0025 and ADR-0026: Diatom evaluates conditions
 * against every frame, because they compare against the PREVIOUS frame and
 * this process only sees the socket every 100ms. This side owns everything
 * else - which set belongs to which game, what an achievement is called, and
 * what the player has already earned.
 *
 * A set lives at `Roms/<System>/.cheevos/<name>.set`, beside the box art in
 * `.media/`, and is put there host-side by `tools/ra-sets.py`. There is no
 * network here: RetroAchievements is HTTPS only and the Brick has no TLS.
 * That costs nothing for detection, which is static, and blocks only
 * submitting unlocks back to an account - which needs client registration
 * anyway.
 *
 * One file, two readers. Diatom reads the bare `<id>\t<condition>` lines and
 * skips every `#` line; this reads the `#` lines for the menu. A second file
 * would eventually disagree with the first about which achievements exist.
 */

#define CHV_MAX     256    /* the largest measured is 203 - Mickey's Speedway USA */
#define CHV_TITLE    96
/* 242 is the longest of the 3542 achievements on the card, measured
 * 2026-09-10: Gain Ground on Genesis, "This Little Light of Mine", a paragraph
 * of conditions. At 192 it was cut at parse time, mid-sentence and with no
 * ellipsis, losing instructions rather than flavor - the reader was never told
 * that "No Exit allowed. Special Mode okay." was part of the requirement. */
#define CHV_DESC    256

typedef struct {
	int  id;
	int  points;
	bool earned;       /* before this game started */
	bool earned_now;   /* during it, so the menu can say what just happened */
	char title[CHV_TITLE];
	char desc[CHV_DESC];
} cheevo;

/* Roms/<folder>/.cheevos/<name>.set */
void chv_path(const char *roms_root, const char *folder, const char *name,
              char *out, size_t n);

/* Load the set for a game. False means there is no set, which is the ordinary
 * case for most of a library and is not an error. */
bool chv_load(const char *set_path);
void chv_clear(void);

int         chv_count(void);
const cheevo *chv_at(int i);
int         chv_game(void);        /* RetroAchievements game id */
int         chv_console(void);     /* RetroAchievements console id, for RUN */
const char *chv_game_title(void);
int         chv_earned(void);      /* including this session */
int         chv_points_earned(void);
int         chv_points_total(void);
int         chv_new_this_session(void);

/* Write what Diatom should watch: the set minus what is already earned.
 * Diatom has no account and no idea what earned means, so filtering is this
 * side's job (ADR-0026). Streams the source file rather than holding it -
 * a single condition string has been measured at 30,897 characters.
 * False means nothing is left to watch, and the caller should not send it. */
bool chv_write_active(const char *path);

/* An unlock arrived from Diatom. True if it was news, which is what decides
 * whether the store needs writing. */
bool chv_note_unlock(int id);

/* The earned store: one flat file for the whole library, keyed by game id and
 * achievement id, the same way favorites are keyed by tag and file.
 *
 * Each row carries whether RetroAchievements has it yet. The store is the
 * DURABLE record and the account is a peer to reconcile with, not the other
 * way round: a device that is offline for a week still knows what was earned,
 * and sends it when it can. That is what RA's own requirements call offline
 * queueing, and it is the right shape regardless of them.
 *
 *     <game id>\t<achievement id>\t<s|p>
 *
 * A row with no third field is read as pending, which is true of every row
 * written before this existed. */
void chv_earned_load(void);
bool chv_earned_save(void);

/* Record something earned. `synced` is true when it came FROM the account, so
 * it is already there and needs no sending. Returns true if this was new. */
bool chv_note_earned(int game, int id, bool synced);

/* What still needs sending. Iterating is by index into the store, so a caller
 * can walk them and mark each as it succeeds. */
int  chv_pending_count(void);
bool chv_pending_at(int n, int *game, int *id);
void chv_mark_synced(int game, int id);

#endif
