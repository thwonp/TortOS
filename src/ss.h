/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef TORTOS_SS_H
#define TORTOS_SS_H

#include <stdbool.h>
#include <stddef.h>

/* The ScreenScraper account, and whether this build can talk to them at all.
 *
 * TWO CREDENTIALS, AND THEY BELONG TO DIFFERENT PEOPLE. A call carries a
 * developer pair that identifies the software and a player pair that
 * identifies whoever is holding the device, and both are required.
 *
 * The developer pair is built in: mk/cross.mk reads SS_DEVID and SS_DEVPASS
 * from the environment into a generated header, so they never enter the
 * repository. An unset environment is a supported build - ss_have_dev() is
 * then false, and the launcher scrapes from libretro alone. That is the whole
 * of the protection and it is worth being exact about what it is not: the
 * binary still contains both strings, `strings` finds them in seconds, and
 * encrypting them with a key shipped beside them would be the same secret with
 * an extra step. What it stops is the automated harvesting of public
 * repositories, which is how these are actually taken. BACKLOG 31.
 *
 * The player pair is asked for on the device and required rather than
 * optional, which is what bounds the rest: every request spends that player's
 * own quota and is attributed to their account, so a leaked developer id on
 * its own buys very little. A full card is about 1,700 requests against a
 * daily allowance of 20,000 - most of a day for one person, and impossible for
 * an account shared by every install.
 *
 * NO TOKEN, unlike RetroAchievements. Their sign-in trades a password for a
 * token and the password is dropped; ScreenScraper authenticates every single
 * request with the password itself, so the device stores a reusable credential
 * for as long as the account is configured. It lives in the per-device
 * database, which is 0600 for exactly this kind of thing, beside the RA
 * account and for the same reason: an account belongs to whoever is holding
 * the handheld, not to the card. */

#define SS_USER_MAX 64
#define SS_PASS_MAX 96

/* Whether this build carries a developer pair. False is not an error: it is a
 * build made without the environment set, and every caller falls back to
 * libretro the same way it does for a player who has not signed in. */
bool ss_have_dev(void);

bool ss_creds_load(void);
bool ss_creds_save(void);
void ss_creds_clear(void);

/* Both halves present: this build can talk to them, and a player has said who
 * they are. The one question every caller actually asks. */
bool ss_signed_in(void);
const char *ss_user(void);

/* Confirm an account and keep it. There is no token to exchange, so this
 * fetches the account's own record instead - the nearest thing to a sign-in
 * check - and stores the pair only once it has come back.
 *
 * `err` takes a short reason on failure. It cannot always say which reason:
 * curl is run with `fail`, so an HTTP error gives us a status and no body, and
 * "wrong password" and "their site is down" arrive looking the same. Saying so
 * is better than picking one. */
bool ss_sign_in(const char *user, const char *password, char *err, size_t errn);

/* The credential half of a query string: devid, devpassword, softname, output
 * and the player's ssid and sspassword, urlencoded and joined with `&`.
 *
 * A FRAGMENT RATHER THAN THE PASSWORD ITSELF. Every caller needs the same six
 * parameters and none of them needs to hold the password to build them, so it
 * does not leave this file. False when either half is missing.
 *
 * What comes back is a credential: it is never logged, never printed, and the
 * caller wipes it when it is done. */
bool ss_auth_query(char *out, size_t n);

#endif
