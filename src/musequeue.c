/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* See musequeue.h. */
#include "musequeue.h"

#include <stdlib.h>

/* xorshift32. The queue's own, so shuffling is repeatable under a check and
 * touches no global state anything else relies on. */
static unsigned rnd(unsigned *s)
{
	unsigned x = *s ? *s : 0x9E3779B9u;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*s = x;
	return x;
}

/* Fisher-Yates over order[from..n), leaving everything before `from` alone. */
static void shuffle_from(muq *q, int from)
{
	int i;

	for (i = q->n - 1; i > from; i--) {
		int j = from + (int)(rnd(&q->seed) % (unsigned)(i - from + 1));
		int t = q->order[i];

		q->order[i] = q->order[j];
		q->order[j] = t;
	}
}

/* Album order with `first` moved to the front, and the rest shuffled. */
static void shuffle_around(muq *q, int first)
{
	int i;

	for (i = 0; i < q->n; i++) q->order[i] = i;
	q->order[0] = first;
	q->order[first] = 0;
	shuffle_from(q, 1);
}

/* A shuffle that has run out, shuffled again - but never onto the track that
 * just finished, which would play it twice running and reads as a stuck
 * player rather than as chance. */
static void reshuffle(muq *q)
{
	int last = q->order[q->n - 1], i;

	for (i = 0; i < q->n; i++) q->order[i] = i;
	shuffle_from(q, 0);
	if (q->n > 1 && q->order[0] == last) {
		int j = 1 + (int)(rnd(&q->seed) % (unsigned)(q->n - 1));
		int t = q->order[0];

		q->order[0] = q->order[j];
		q->order[j] = t;
	}
}

bool muq_init(muq *q, int n, int start, muq_mode mode, unsigned seed)
{
	int i;

	q->order = n > 0 ? malloc(sizeof *q->order * (size_t)n) : NULL;
	q->n = q->order ? n : 0;
	q->pos = 0;
	q->mode = mode;
	q->seed = seed;
	if (n > 0 && !q->order) return false;
	if (start < 0 || start >= q->n) start = 0;
	if (mode == MUQ_SHUFFLE && q->n > 0) {
		shuffle_around(q, start);
		return true;
	}
	for (i = 0; i < q->n; i++) q->order[i] = i;
	q->pos = start;
	return true;
}

void muq_free(muq *q)
{
	free(q->order);
	q->order = NULL;
	q->n = q->pos = 0;
}

int muq_current(const muq *q)
{
	return q->n > 0 ? q->order[q->pos] : -1;
}

/* One step on, with the end of the order answered by the mode. */
static int onward(muq *q)
{
	if (q->n == 0) return -1;
	if (q->pos + 1 < q->n) return q->order[++q->pos];
	switch (q->mode) {
	case MUQ_REPEAT_ALL:
	case MUQ_REPEAT_ONE:
		q->pos = 0;
		return q->order[0];
	case MUQ_SHUFFLE:
		reshuffle(q);
		q->pos = 0;
		return q->order[0];
	default:
		return -1;
	}
}

int muq_ended(muq *q)
{
	if (q->mode == MUQ_REPEAT_ONE) return muq_current(q);
	return onward(q);
}

int muq_failed(muq *q)
{
	return onward(q);
}

int muq_next(muq *q)
{
	return onward(q);
}

int muq_prev(muq *q)
{
	if (q->n == 0) return -1;
	if (q->pos > 0) return q->order[--q->pos];
	if (q->mode == MUQ_REPEAT_ALL || q->mode == MUQ_REPEAT_ONE) {
		q->pos = q->n - 1;
		return q->order[q->pos];
	}
	return -1;
}

void muq_set_mode(muq *q, muq_mode m)
{
	int cur = muq_current(q), i;

	if (m == q->mode) return;
	if (q->n > 0 && m == MUQ_SHUFFLE) {
		shuffle_around(q, cur);
		q->pos = 0;
	} else if (q->n > 0 && q->mode == MUQ_SHUFFLE) {
		for (i = 0; i < q->n; i++) q->order[i] = i;
		q->pos = cur;
	}
	q->mode = m;
}

int muq_upcoming(const muq *q)
{
	if (q->n == 0) return -1;
	if (q->mode == MUQ_REPEAT_ONE) return q->order[q->pos];
	if (q->pos + 1 < q->n) return q->order[q->pos + 1];
	if (q->mode == MUQ_REPEAT_ALL) return q->order[0];
	return -1;
}
