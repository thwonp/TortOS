/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef TORTOS_CHDREAD_H
#define TORTOS_CHDREAD_H

struct rc_hash_cdreader;

/* rhash's CD reader, extended to CHD (plorpos-gkd.53). rcheevos reads
 * .cue/.bin/.iso itself but has no CHD reader - its frontends bring their own,
 * as RetroArch does with libchdr - so this fills `r` with callbacks that open
 * a .chd through libchdr and hand every other path to rhash's default reader.
 * A disc is read a sector at a time, never whole. */
void chd_cdreader(struct rc_hash_cdreader *r);

#endif
