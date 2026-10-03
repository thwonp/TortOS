/* SPDX-License-Identifier: MIT */
/* Print the RetroAchievements hash of each ROM given, so it can be diffed
 * against the same rules written in Python.
 *
 *     tools/rahash-check.c  <tag> <rom>...   ->  <hash>\t<rom>
 *
 * Driven by `make check-rahash`, which runs both over the whole test library
 * and requires every one of them to agree. Two implementations of someone
 * else's spec is a duplication; this is what keeps it from being a drift.
 */
#include <stdio.h>
#include "../src/rahash.h"

int main(int argc, char **argv)
{
	int i, bad = 0;

	if (argc < 3) { fprintf(stderr, "usage: rahash-check <tag> <rom>...\n"); return 2; }
	for (i = 2; i < argc; i++) {
		char h[33];
		if (ra_hash_rom(argv[i], argv[1], h)) printf("%s\t%s\n", h, argv[i]);
		else { printf("FAILED\t%s\n", argv[i]); bad++; }
	}
	return bad ? 1 : 0;
}
