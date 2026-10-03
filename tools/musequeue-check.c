/* SPDX-License-Identifier: MIT */
/* Does Muse play the tracks it should, in the order it should?
 *
 *     make check-musequeue
 *
 * Every mode walked through the places a player gets wrong: the last track, the
 * first one, running out while shuffled, a track that will not open, and the
 * mode changing under a track that is playing - which must not change the
 * track. Shuffle is checked as a permutation, every track once a round, and
 * never the same track twice running across a reshuffle.
 *
 * No SDL, no daemon, no device.
 */
#include <stdio.h>
#include <string.h>

#include "../src/musequeue.h"

static int failures;

#define CHECK(cond, ...)                                                      \
	do {                                                                      \
		if (!(cond)) {                                                        \
			printf("  FAIL: ");                                               \
			printf(__VA_ARGS__);                                              \
			printf("\n        at %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
			failures++;                                                       \
		}                                                                     \
	} while (0)

/* Each track exactly once in the order. */
static int is_permutation(const muq *q)
{
	int seen[64] = { 0 }, i;

	for (i = 0; i < q->n; i++) {
		if (q->order[i] < 0 || q->order[i] >= q->n || seen[q->order[i]]) return 0;
		seen[q->order[i]] = 1;
	}
	return 1;
}

static void in_order(void)
{
	muq q;

	printf("in order\n");
	muq_init(&q, 5, 2, MUQ_IN_ORDER, 1);
	CHECK(muq_current(&q) == 2, "starts on the track chosen: %d", muq_current(&q));
	CHECK(muq_upcoming(&q) == 3, "the next is the next: %d", muq_upcoming(&q));
	CHECK(muq_ended(&q) == 3 && muq_ended(&q) == 4, "plays on in album order");
	CHECK(muq_upcoming(&q) == -1, "nothing follows the last track");
	CHECK(muq_ended(&q) == -1, "and it stops there");
	CHECK(muq_next(&q) == -1 && muq_current(&q) == 4, "R1 on the last goes nowhere");
	CHECK(muq_prev(&q) == 3, "L1 goes back one");
	muq_free(&q);
	muq_init(&q, 5, 0, MUQ_IN_ORDER, 1);
	CHECK(muq_prev(&q) == -1 && muq_current(&q) == 0,
	      "L1 on the first goes nowhere - the caller restarts the track");
	muq_free(&q);
}

static void repeat_all(void)
{
	muq q;

	printf("repeat all\n");
	muq_init(&q, 3, 2, MUQ_REPEAT_ALL, 1);
	CHECK(muq_upcoming(&q) == 0, "the last is followed by the first");
	CHECK(muq_ended(&q) == 0, "and plays it");
	CHECK(muq_prev(&q) == 2, "L1 from the first wraps to the last");
	CHECK(muq_next(&q) == 0, "R1 from the last wraps to the first");
	muq_free(&q);
}

static void repeat_one(void)
{
	muq q;

	printf("repeat one\n");
	muq_init(&q, 4, 1, MUQ_REPEAT_ONE, 1);
	CHECK(muq_upcoming(&q) == 1, "what follows is itself");
	CHECK(muq_ended(&q) == 1 && muq_ended(&q) == 1, "and it plays again, and again");
	CHECK(muq_failed(&q) == 2, "a track that will not open is not repeated");
	CHECK(muq_next(&q) == 3, "R1 still moves on");
	CHECK(muq_next(&q) == 0, "and wraps");
	muq_free(&q);
}

static void shuffle(void)
{
	muq q;
	int played[16] = { 0 }, i, t, last, twice = 0, round;

	printf("shuffle\n");
	muq_init(&q, 10, 3, MUQ_SHUFFLE, 12345);
	CHECK(muq_current(&q) == 3, "the chosen track plays first: %d", muq_current(&q));
	CHECK(is_permutation(&q), "the order is every track once");
	played[3] = 1;
	for (i = 1; i < 10; i++) {
		t = muq_ended(&q);
		CHECK(t >= 0 && t < 10 && !played[t], "a round plays each track once: %d", t);
		if (t >= 0 && t < 10) played[t] = 1;
	}
	for (i = 0; i < 10; i++) CHECK(played[i], "track %d was played", i);
	CHECK(muq_upcoming(&q) == -1, "what follows a round is not decided yet");

	/* Many rounds, because the reshuffle's one rule - not the same track twice
	 * running - is a chance event on any single round. */
	for (round = 0; round < 200; round++) {
		last = muq_current(&q);
		t = muq_ended(&q);
		CHECK(t >= 0, "a shuffle keeps going after a round");
		if (t == last) twice++;
		CHECK(is_permutation(&q), "the new round is every track once");
		for (i = 1; i < 10; i++) muq_ended(&q);
	}
	CHECK(twice == 0, "never the same track twice across a reshuffle: %d times", twice);

	muq_free(&q);
	muq_init(&q, 5, 0, MUQ_SHUFFLE, 99);
	t = muq_current(&q);
	muq_ended(&q);
	CHECK(muq_prev(&q) == t, "L1 goes back along the order actually played");
	CHECK(muq_prev(&q) == -1, "and stops at the first");
	muq_free(&q);

	muq_init(&q, 1, 0, MUQ_SHUFFLE, 7);
	CHECK(muq_ended(&q) == 0 && muq_ended(&q) == 0, "one track shuffled plays on");
	muq_free(&q);
}

static void changing_mode(void)
{
	muq q;
	int i, identity = 1;

	printf("changing mode\n");
	muq_init(&q, 8, 5, MUQ_IN_ORDER, 42);
	muq_set_mode(&q, MUQ_SHUFFLE);
	CHECK(muq_current(&q) == 5, "into shuffle, the same track is still playing: %d",
	      muq_current(&q));
	CHECK(q.pos == 0 && is_permutation(&q), "with the rest shuffled after it");
	muq_ended(&q);
	muq_ended(&q);
	i = muq_current(&q);
	muq_set_mode(&q, MUQ_IN_ORDER);
	CHECK(muq_current(&q) == i, "out of shuffle, the same track is still playing");
	for (int k = 0; k < q.n; k++) if (q.order[k] != k) identity = 0;
	CHECK(identity, "and album order is back");
	CHECK(muq_upcoming(&q) == (i + 1 < 8 ? i + 1 : -1),
	      "picking up from that track: %d", muq_upcoming(&q));

	muq_set_mode(&q, MUQ_REPEAT_ONE);
	CHECK(muq_current(&q) == i && muq_ended(&q) == i, "repeat one keeps the track");
	muq_set_mode(&q, MUQ_REPEAT_ALL);
	CHECK(muq_current(&q) == i, "and so does repeat all");
	muq_free(&q);
}

static void empty(void)
{
	muq q;

	printf("an empty queue\n");
	muq_init(&q, 0, 0, MUQ_SHUFFLE, 1);
	CHECK(muq_current(&q) == -1 && muq_ended(&q) == -1 && muq_next(&q) == -1 &&
	      muq_prev(&q) == -1 && muq_upcoming(&q) == -1 && muq_failed(&q) == -1,
	      "answers nothing, and does not fall over");
	muq_set_mode(&q, MUQ_IN_ORDER);
	muq_free(&q);
}

int main(void)
{
	printf("musequeue: the play modes\n");
	in_order();
	repeat_all();
	repeat_one();
	shuffle();
	changing_mode();
	empty();
	if (failures) {
		printf("\n%d failure%s\n", failures, failures == 1 ? "" : "s");
		return 1;
	}
	printf("ok\n");
	return 0;
}
