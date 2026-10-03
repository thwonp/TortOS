/* SPDX-License-Identifier: MIT */
#ifndef TORTOS_RAFETCH_H
#define TORTOS_RAFETCH_H

#include <stdbool.h>
#include <stddef.h>

/* RetroAchievements, the parts that need the network.
 *
 * This is the normal client workflow, on the device, the way any other RA
 * frontend does it: hash the ROM, ask which game it is, fetch the set, and
 * cache it beside the ROM. tools/ra-sets.py does the same thing from a host
 * and writes the same file; it exists now only to pre-seed a library in bulk
 * or to work offline, not because the device cannot.
 *
 * The credential kept here is a TOKEN, not a password. RA's login returns one
 * and it is what every later call uses, so the password is typed once and
 * never stored.
 */

#define RA_TOKEN_MAX 64
#define RA_USER_MAX  64

/* Read and write the account in the device database. Per-device rather than
 * per-card: a card moved to another handheld must not carry a session token
 * with it. The database asks for 0600 because of this token, which the card's
 * exfat mount ignores - see db.c. */
bool ra_creds_load(void);
bool ra_creds_save(void);
void ra_creds_clear(void);
bool ra_signed_in(void);
const char *ra_user(void);
/* The account copied out, for work handed to another thread. See rafetch.c. */
void ra_creds_copy(char *user, size_t un, char *token, size_t tn);

/* Exchange a password for a token. The password is used and dropped; nothing
 * writes it anywhere. `err` takes RA's own message when it refuses, which is
 * the difference between a wrong password and a site that is down. */
bool ra_sign_in(const char *user, const char *password, char *err, size_t errn);

/* The conversion on its own, so it can be checked against real responses with
 * no network involved. tools/raset-check.c does exactly that, against the
 * files tools/ra-sets.py writes from the same JSON. */
bool ra_set_from_json(const char *json, size_t len, long gameid,
                      const char *out_path);

/* ---- the account ---------------------------------------------------------
 *
 * Every call here blocks for as long as its timeout, except the ones that only
 * start or poll a request - ra_sync_* and ra_fetch_* - which never wait. None
 * of the blocking ones may be made from inside the in-game wait loop, which is
 * also the power button's watchdog: a request in there would make the device
 * stop answering it. They belong on either side of a game, where the launcher
 * owns the screen.
 */

/* What the account already holds for a game, softcore, asked without waiting
 * for the answer.
 *
 * This is what makes a count mean something. Without it the launcher reports
 * what THIS DEVICE has seen, which looks exactly like an account total and is
 * not one - measured 2026-08-29 as 3/40 against the site's 13/40.
 *
 * Start it before the game and poll it while the game runs: a request that
 * takes 310-460ms lands about half a second in. The cost of waiting at launch
 * was 320ms on the front of every launch, on a launcher whose whole point is
 * that a warm one is 15ms. Measured on the device 2026-08-29; 150ms of it is
 * the TLS handshake and no amount of caching removes that.
 *
 * It used to be collected only when the game ended, blocking until it had
 * landed. So the first session of a game read 0 of N in its own menu, and a
 * quit straight after a launch on a slow network waited out the request.
 *
 * ra_sync_poll never waits: 1 the answer is in and `*n` ids are in `out`, 0 it
 * is still on its way, -1 nothing was asked or the asking failed. It shares
 * the ONE async slot with the set fetch and the box art scraper, so anything
 * else that wants the slot calls ra_sync_abandon first. */
void ra_sync_begin(long gameid);
int  ra_sync_poll(int *out, int max, int *n);
bool ra_sync_pending(void);
void ra_sync_abandon(void);

/* Finding and fetching a set WHILE the game runs, rather than in front of it.
 *
 * The first play of a game needs two requests - which game is this, and what
 * are its achievements - and doing them before the launch put most of a second
 * between pressing A and the game appearing. So the game starts with no set,
 * these run behind it, and the launcher hands the set over with SETCHEEVOS
 * once it lands. That message exists for exactly this: ADR-0026 calls it "the
 * normal path whenever the set is still downloading when the player presses
 * A", and then the first version blocked instead.
 *
 * The cost is that nothing is watched for the first second or two of the first
 * ever play of a game. You are at a title screen; every launch after is
 * instant from the cache.
 *
 * `rom_hash` is the caller's, because hashing is local work and does not
 * belong in a state machine about network requests. Drive with ra_fetch_step:
 * 0 still working, 1 the set is written, -1 idle or failed. */
void ra_fetch_begin(const char *rom_hash, const char *set_path);
int  ra_fetch_step(void);
long ra_fetch_gameid(void);

/* Submit one unlock, softcore. Signed the way rcheevos signs it -
 * md5(achievement id + username + hardcore flag) - because the server checks
 * it. `rom_hash` is optional and is what real clients send.
 *
 * 1 accepted, 0 the account already had it, -1 a real failure. The middle case
 * is not an error and must not be treated as one: it is the account agreeing,
 * and calling it a failure leaves the row owed forever and stops everything
 * queued behind it. */
int ra_submit_unlock(int achievement_id, const char *rom_hash);

/* The same, for an account passed in rather than the one held here.
 *
 * Exists so the unlock queue can be flushed from a worker thread: it touches no
 * state in this file, only its arguments and a request of its own, so it is
 * safe to call while the main thread signs in, signs out, or makes a request of
 * its own. ra_submit_unlock is this with the account held here. */
int ra_submit_unlock_as(const char *user, const char *token,
                        int achievement_id, const char *rom_hash);

#endif
