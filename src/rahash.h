/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef TORTOS_RAHASH_H
#define TORTOS_RAHASH_H

#include <stdbool.h>
#include <stddef.h>

/* RetroAchievements identifies a game by an MD5 over the ROM - but not always
 * over the WHOLE ROM. Each console has its own rule about what to skip, and
 * getting one wrong produces a hash RA has never seen, which is indistinguish-
 * able from a game it does not know.
 *
 * Two couplings worth stating, because both fail silently:
 *
 *   - THE ENTRY. A zipped archive is hashed on its LARGEST entry, which is the
 *     one Diatom loads (its src/zip.c). Taking the first instead would hash one
 *     game and run another, and every achievement would be for something else.
 *   - THE RULES. They are RetroAchievements', restated here in C and in
 *     tools/ra-check.py. Both are checked against the same ROMs; see
 *     `make check-rahash`.
 */

/* Writes 32 lowercase hex digits plus a NUL, so `out` needs 33 bytes.
 * `tag` is a systems.cfg tag - NES, SFC, MD, GB. False means the file could
 * not be read or the archive could not be opened.
 *
 * A disc - anything on the PlayStation shelf, a .chd/.cue/.m3u on the PC
 * Engine one - is hashed by RA's own rhash instead (third_party/, with
 * chdread.c for CHD), a few sectors read, never the image. Anything else over
 * 64 MB is refused rather than read whole: no cartridge comes near it, and a
 * disc that reached the cartridge path would be hundreds of MB. */
bool ra_hash_rom(const char *path, const char *tag, char *out);

/* Plain MD5 of a buffer, same 33-byte output. Exposed because submitting an
 * unlock needs one too: RetroAchievements signs the request with
 * md5(achievement id + username + hardcore flag) and checks it server-side.
 * One implementation, two callers - a second MD5 in this repository would be
 * a second thing to get subtly wrong. */
void ra_md5_hex(const void *data, size_t len, char *out);

#endif
