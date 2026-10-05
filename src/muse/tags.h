/* SPDX-License-Identifier: MIT */
#ifndef MUSE_TAGS_H
#define MUSE_TAGS_H

#include <stddef.h>

/* Who a folder of tracks is by, from their tags, for the launcher to file an
 * album that sits straight in Music/ under its artist (Eric's call,
 * 2026-10-05).
 *
 * The album artist when the tracks that carry one all agree, which is how a
 * rip is tagged (TPE2 filled, TPE1 empty - see snapshot in muse.c); failing
 * that, the artist when every track has one and they all agree, so a mix of
 * many artists answers nothing rather than its first track's. "" when there
 * is no clear answer, and the album stays where it is.
 *
 * Here because FFmpeg already reads ID3, MP4 atoms, FLAC blocks and Vorbis
 * comments, as cover.c says. A file counts when FFmpeg finds an audio stream
 * in it, so a cover.jpg beside the tracks has no say.
 *
 * Returns how many tracks were read, or -1 when the folder could not be. */
int tags_folder_artist(const char *dir, char *out, size_t n);

#endif
