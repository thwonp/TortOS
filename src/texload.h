/* SPDX-License-Identifier: MIT */
#ifndef TORTOS_TEXLOAD_H
#define TORTOS_TEXLOAD_H

#include <stdbool.h>
#include <SDL.h>

/* Decoding card art off the render thread.
 *
 * cf_draw asks for every visible card every frame and the decode used to
 * happen right there, inside the frame: 14-60ms against a 16.7ms budget,
 * measured on this device. That is why a letter jump showed two still
 * pictures - the tween was running correctly and the device could not produce
 * frames for it.
 *
 * Time-slicing cannot fix it. prime_toward already slices at one card per
 * frame and one card still overruns the frame by itself; the cost is inside
 * IMG_Load and does not subdivide. So it runs on a thread.
 *
 * The split is by what needs the renderer. IMG_Load and the format conversion
 * are pure CPU and happen on the worker; SDL_CreateTextureFromSurface needs
 * the renderer and stays on the main thread, where it costs 1-2ms.
 *
 * The visible consequence is that a cold card is blank for a few frames rather
 * than freezing the shelf. It is not slower - the art lands at the same moment
 * either way, and what changes is whether everything else kept moving while it
 * was coming. */

bool texload_start(void);
void texload_stop(void);

/* Everything in flight is stale from here on: the shelf it was for has been
 * rebuilt and its indices no longer mean what they meant. Results already
 * decoded are dropped rather than installed against the wrong game. */
void texload_bump(void);

/* Drop every request not started yet. Unlike texload_bump nothing is disowned:
 * a decode already under way still lands and is installed, because the shelf
 * it was for has not changed shape - only where anyone is looking. For a cursor
 * that has moved on, so the next requests are not served behind a place nobody
 * is looking at any more. */
void texload_forget(void);

/* Decode for (sys, idx), trying `first` then `second`. Either may be NULL.
 * Ignored if that card is already queued, in flight, or waiting to be taken,
 * so a caller may ask every frame without piling work up.
 *
 * False means there is no worker - the thread never started - and the caller
 * must decode it itself. A full queue is not a failure: it returns true and
 * the card is simply asked for again next frame, which is the whole point of
 * asking being cheap. */
bool texload_want(int sys, int idx, const char *first, const char *second);

/* One finished decode, or false if none is ready. The caller owns `surf` and
 * must free it; it is NULL when neither file could be read, which means the
 * caller should draw its own placeholder.
 *
 * The surface is already ARGB8888, so the caller only has to make a texture
 * from it. Where the art stops is NOT computed here: content_bottom scans
 * trailing empty rows and stops at the first row with anything in it, so it
 * costs the padding rather than the image, and duplicating it out of main.c
 * to save that would be the worse trade. */
bool texload_take(int *sys, int *idx, SDL_Surface **surf);

/* Whether anything is waiting to be taken, without taking it.
 *
 * For a loop that only draws when something changed. Finished decodes are
 * installed while drawing, so a loop sitting idle would never draw and never
 * find them: art that finished while nothing moved would wait for the next
 * press. This lets the loop notice there is something to install. */
bool texload_ready(void);

#endif
