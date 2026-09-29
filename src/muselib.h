/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef TORTOS_MUSELIB_H
#define TORTOS_MUSELIB_H

#include <stdbool.h>
#include <stddef.h>

#include "library.h"

/* What is in /mnt/SDCARD/Music, as Muse's screen browses it: artists, their
 * albums, and each album's tracks.
 *
 * READ FROM THE FOLDERS, NOT THE TAGS. The launcher does not link FFmpeg - the
 * daemon does, and asking it about every file on the card would be a request
 * per track on the way into a screen. Folder and file names are what the card
 * is organized by anyway, and they are what the person who filled it chose.
 *
 * Two shapes are understood:
 *
 *   Music/<artist>/<album>/<tracks>    the ordinary one
 *   Music/<name>/<tracks>              a folder of tracks with no artist above
 *                                      it - a podcast, a compilation - which is
 *                                      its own artist with one album
 *
 * Tracks sort by file name, which puts "01 ..." before "02 ..."; the number
 * is then taken off the name that is shown. Pure: directory reads and nothing
 * else, so tools/muselib-check.c can hand it a folder and look. */

typedef struct { char name[128]; char path[LIB_PATH]; } ml_track;
typedef struct { char name[128]; int first, n; } ml_album;    /* tracks[first..] */
typedef struct { char name[128]; int first, n; } ml_artist;   /* albums[first..] */

typedef struct {
	ml_artist *artists; int nartists;
	ml_album  *albums;  int nalbums;
	ml_track  *tracks;  int ntracks;
} ml_lib;

/* Scan `root`. False only when the folder is not there; an empty folder is an
 * empty library and true. `path` in each track is relative to `root`. */
bool ml_scan(const char *root, ml_lib *out);
void ml_free(ml_lib *lib);

/* Whether a file name is audio Muse plays, by extension. */
bool ml_is_audio(const char *name);

/* The name a track is shown by: no extension, and no leading track number -
 * "03 High And Dry.mp3" is "High And Dry". */
void ml_track_name(const char *file, char *out, int n);

/* The album `track` - a path relative to the root - is in, or -1. By folder,
 * not by name: two artists can each have a "Greatest Hits", and the folder is
 * the one thing that tells them apart. */
int ml_album_of(const ml_lib *l, const char *track);

/* Where album `al`'s cover is kept, without its extension: in a .media folder
 * beside the album's own, named after it - the way box art sits in
 * Roms/<system>/.media. Music/Radiohead/The Bends keeps its cover in
 * Music/Radiohead/.media/The Bends.jpg, and a folder of tracks straight under
 * the root keeps its in Music/.media. The scan skips dot folders, so a .media
 * is never taken for an album. "" for an album with no tracks. */
void ml_cover_base(const char *root, const ml_lib *l, int al, char *out, size_t n);

/* The orders Muse's shelf can be in, which its menu's Sort By steps through.
 *
 * By artist is the folder's own order, the scan's: artists alphabetically,
 * then each one's albums. By album is the same albums by their own titles, the
 * artist breaking a tie - two artists can each have a "Greatest Hits". Case
 * does not count and leading articles do, in both, as on a games shelf.
 *
 * Nothing here orders by date or by listening. Muse keeps no record of what
 * was played, and the card knows no release years - the tags that carry them
 * are read by the daemon, not the scan. */
typedef enum { ML_BY_ARTIST, ML_BY_ALBUM, ML_ORDERS } ml_order;

/* What the setting stores, and what the menu row shows. */
const char *ml_order_name(ml_order o);
const char *ml_order_label(ml_order o);
/* The order a stored name is. By artist for one it does not know. */
ml_order ml_order_index(const char *name);

/* The shelf in order `by`: out[k] is the album card k is, for every album.
 * Out of memory it is the scan's order, which is a shelf and not a wrong one. */
void ml_shelf_order(const ml_lib *l, ml_order by, int *out);

#endif
