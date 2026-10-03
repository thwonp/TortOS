/* SPDX-License-Identifier: MIT */
#ifndef MUSE_PCM_H
#define MUSE_PCM_H

#include <stdbool.h>
#include <stdint.h>

/* The output: ALSA, opened by name, at 48 kHz S16 stereo.
 *
 * libasound directly rather than through SDL, because Diatom's ADR-0030 found
 * SDL cannot drive a bluealsa device string and a headset is a requirement.
 * And reached with dlopen, the way src/db.c reaches sqlite: the sysroot has no
 * ALSA headers, the device has the library, and nine functions do not justify
 * fetching a header tree for them.
 *
 * `default` goes through the Brick's dmix, which is what lets Muse play over a
 * running game without either of them knowing. It NEVER touches the mixer:
 * the launcher owns the mute switch and cuts HpSpeaker Switch below the mix,
 * and a producer that writes that control to turn the speaker on is exactly
 * the bug ADR-0031 exists to forbid. */

bool pcm_open(const char *device);   /* "default", or a bluealsa PCM name */
void pcm_close(void);

/* Blocks until the frames are queued, recovering from an underrun on the way.
 * False on an error that would not recover. */
bool pcm_write(const int16_t *frames, int n);

/* Throw away what is queued. For pause and seek: what has been heard is where
 * the player IS, and anything still in the buffer would play a moment of the
 * old position after the new one had been asked for. */
void pcm_drop(void);

/* Frames queued but not yet heard, so the caller can say where the listener
 * actually is rather than where the decoder got to. */
long pcm_queued(void);

const char *pcm_error(void);
/* Whether a device is open at all - pcm_open fails closed. */
bool pcm_is_open(void);
/* Whether the last open failed because another process holds the device -
 * the one failure worth waiting out. */
bool pcm_busy(void);

/* A line for the record of what was done to the output: the last 32 are kept
 * and written out by pcm_trail_dump, from a stall or a fatal signal. */
void pcm_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Write that record to `fd`. Only write(), so a signal handler may call it. */
void pcm_trail_dump(int fd);
/* How long the write in progress has been going, in ms, or 0 when none is:
 * a stall that never returns never gets noted, so the report asks. */
long pcm_in_write_ms(void);

#endif
