/* SPDX-License-Identifier: MIT */
#ifndef TORTOS_MUSEQUEUE_H
#define TORTOS_MUSEQUEUE_H

#include <stdbool.h>

/* The order Muse plays a queue in - the play modes, and what comes next.
 *
 * musec.c holds the tracks and talks to the daemon; this only says which track
 * plays when. Pure - an array of numbers and a place in it, no socket and no
 * clock - so tools/musequeue-check.c can walk every mode through its edges,
 * which is where a player's bugs live: the last track, the first one, a mode
 * changed halfway through.
 *
 * A track is its place in the queue as it was handed over, which is album
 * order. Shuffle changes the ORDER, a permutation of those, and `pos` walks
 * it. In every other mode the order is the album's own. */

typedef enum {
	MUQ_IN_ORDER,     /* through once, then stop */
	MUQ_REPEAT_ALL,   /* through, and round again */
	MUQ_REPEAT_ONE,   /* the same track until someone changes it */
	MUQ_SHUFFLE,      /* a random order, shuffled again when it runs out */
	MUQ_MODES
} muq_mode;

typedef struct {
	int     *order;   /* order[pos] is the track playing */
	int      n, pos;
	muq_mode mode;
	unsigned seed;    /* the shuffle's generator, here so a check can fix it */
} muq;

/* A queue of `n` tracks, starting on `start`. Shuffled, `start` plays first
 * and the rest follow in a random order. False when there was no memory, and
 * the queue is then empty. */
bool muq_init(muq *q, int n, int start, muq_mode mode, unsigned seed);
void muq_free(muq *q);

/* The track at `pos`, or -1 for an empty queue. */
int muq_current(const muq *q);

/* The track ran out by itself: what plays now, or -1 to stop. Repeat one plays
 * it again. */
int muq_ended(muq *q);

/* The track would not play: what to try instead, or -1. As muq_ended, except
 * that repeat one moves on - trying a file that will not open again and again
 * is a loop, not a repeat. */
int muq_failed(muq *q);

/* R1: onward, wrapping in every mode but in order, where the last track is the
 * last. -1 when there is nowhere to go, and nothing moved. */
int muq_next(muq *q);

/* L1: back along the order actually played. Repeat wraps from the first track
 * to the last; in order and shuffle stop at the first. -1 when there is nowhere
 * to go, and nothing moved - the caller starts the track over instead. */
int muq_prev(muq *q);

/* A new mode, without changing the track. Into shuffle, the track playing stays
 * where it is and the rest are shuffled after it; out of shuffle, album order
 * picks up from the track playing. */
void muq_set_mode(muq *q, muq_mode m);

/* What muq_ended would play, without moving: -1 when that is nothing, or not
 * decided yet - a shuffle that has run out is shuffled again only when it gets
 * there. */
int muq_upcoming(const muq *q);

#endif
