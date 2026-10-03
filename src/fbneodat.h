/* SPDX-License-Identifier: MIT */
#ifndef TORTOS_FBNEODAT_H
#define TORTOS_FBNEODAT_H

/* FBNeo's own description of an arcade set - "Mortal Kombat 3 (rev 2.1)" for
 * mk3 - or NULL when the table does not list it or cannot be read.
 *
 * `path` is res/fbneo-titles.tsv as tools/gen-fbneo-titles.py writes it, read
 * the first time it is asked for and kept: the shelf titles (titles.c) and the
 * box art names (artscrape.c) both ask, and 359 KB is not worth reading twice.
 * The set is matched whatever its case. The description comes back raw,
 * revision and all, because libretro files arcade art under exactly that. */
const char *fbneo_desc(const char *path, const char *set);

#endif
