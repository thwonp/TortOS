/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef TORTOS_FAVORITES_H
#define TORTOS_FAVORITES_H

#include <stdbool.h>

/* Favorites: a flat set of (system tag, ROM file) pairs.
 *
 * Keyed the same way save states are - the system's tag plus the launch path
 * relative to Roms/<folder> - and stored in the same directory, so a favorite
 * travels with the saves it belongs beside and survives a card reflash. Two
 * different systems can hold a file of the same name, which is why the tag is
 * half the key rather than a convenience.
 *
 * A set, not a list: the order games appear in the Favorites shelf comes from
 * the shelves they were favorited on, so that shelf is sorted the same way
 * every other one is instead of by the order someone happened to press Y.
 */

#define FAV_MAX      512
#define FAV_TAG_MAX   16
#define FAV_FILE_MAX 544     /* LIB_PATH */

/* Read them from the library database. None is not an error: nobody has
 * favorited anything yet. */
void fav_load(void);

/* The whole set, every time, removals included - so what is stored cannot
 * drift out of step with what is in memory. */
bool fav_save(void);

bool fav_is(const char *tag, const char *file);

/* Returns the state AFTER the toggle, so a caller can say what it did. Full
 * at FAV_MAX means adding fails and this returns false; removing always
 * works. */
bool fav_toggle(const char *tag, const char *file);

int  fav_count(void);
bool fav_at(int i, const char **tag, const char **file);

#endif
