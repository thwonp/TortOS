/* SPDX-License-Identifier: MIT */
#ifndef MUSE_COVER_H
#define MUSE_COVER_H

#include <stddef.h>

/* The picture a file carries - the album cover a tagger put in it - written
 * out where the launcher can load it.
 *
 * Here and not in the launcher because reading it means reading ID3, MP4
 * atoms, FLAC blocks and Vorbis comments, and FFmpeg already reads all four:
 * it hands every one of them back as the same thing, a stream marked as an
 * attached picture whose one packet is the image file.
 *
 * Written to `base` plus the extension its format has, ".jpg" or ".png", with
 * the full name put in `out`. The folder `base` is in is made if it is not
 * there - one level, which is all the launcher's layout ever needs. Through a
 * temporary name and a rename, so the launcher can never load half of one.
 *
 * 1 written, 0 the file carries no picture, -1 the file could not be read or
 * the picture could not be written. */
int cover_extract(const char *path, const char *base, char *out, size_t n);

#endif
