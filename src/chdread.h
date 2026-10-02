/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef TORTOS_CHDREAD_H
#define TORTOS_CHDREAD_H

#include <stdbool.h>
#include <stddef.h>

struct rc_hash_cdreader;

/* rhash's CD reader, extended to CHD (plorpos-gkd.53). rcheevos reads
 * .cue/.bin/.iso itself but has no CHD reader - its frontends bring their own,
 * as RetroArch does with libchdr - so this fills `r` with callbacks that open
 * a .chd through libchdr and hand every other path to rhash's default reader.
 * A disc is read a sector at a time, never whole. */
void chd_cdreader(struct rc_hash_cdreader *r);

/* The first n bytes of the first sector of a disc's track 1 - a .chd, a .cue
 * (or anything rhash's own reader opens), or the first disc an .m3u lists.
 * For the launcher reading a disc header without hashing it (plorpos-gkd.71). */
bool cd_read_head(const char *path, void *out, size_t n);

#endif
