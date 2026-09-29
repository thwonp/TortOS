/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef MUSE_DEC_H
#define MUSE_DEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One file, decoded and brought to what the Brick's mixer takes: 48 kHz,
 * signed 16-bit, stereo, interleaved.
 *
 * Everything goes through the device's own FFmpeg 6.1 - the libraries in
 * /usr/lib, which is why there is no decoder of our own here. Measured on the
 * Brick 2026-09-17 (BACKLOG, the Muse spike): MP3, AAC, M4B, FLAC and Opus at
 * 88-116x realtime and about 1% of a core, 12-14 MB resident, with the M4B's
 * chapters and every file's tags read by the same library. */

#define DEC_RATE     48000
#define DEC_CHANNELS 2

typedef struct dec dec;

/* Open `path` and position it at `at` seconds, playing at `speed` (1.0 is
 * normal; 0.5-2.0 is what anyone uses, and atempo changes it without changing
 * pitch). NULL on failure, with the reason in `err`. */
dec *dec_open(const char *path, double at, double speed, char *err, size_t errn);

/* Up to `max` frames into `buf` (a frame is one sample per channel). Returns
 * how many, 0 at the end of the file, <0 on an error the file cannot recover
 * from. */
int dec_read(dec *d, int16_t *buf, int max);

/* Where the output is, in the FILE's seconds - which is not the number of
 * seconds played once the speed is not 1. */
double dec_pos(const dec *d);
double dec_len(const dec *d);

/* A tag, or "" when the file has none: "title", "artist", "album". */
const char *dec_tag(const dec *d, const char *key);

/* Chapters: M4B carries them; everything else answers 0. */
int         dec_chapters(const dec *d);
double      dec_chapter_at(const dec *d, int i);
const char *dec_chapter_title(const dec *d, int i);

void dec_close(dec *d);

#endif
