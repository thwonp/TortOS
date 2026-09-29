/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* Convert a RetroAchievements patch response to a set file, with no network.
 *
 *     raset-check <gameid> <patch.json> <out.set>
 *
 * Exists so the C converter can be diffed against the Python one in
 * tools/ra-sets.py over real responses. Both read the same JSON and both write
 * the file Diatom evaluates; if they disagree, one of them is wrong about a
 * game somebody is playing.
 */
#include <stdio.h>
#include <stdlib.h>
#include "../src/rafetch.h"

int main(int argc, char **argv)
{
	char *buf;
	long n;
	FILE *f;

	if (argc != 4) { fprintf(stderr, "usage: raset-check <gameid> <json> <out>\n"); return 2; }
	f = fopen(argv[2], "rb");
	if (!f) { perror(argv[2]); return 2; }
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)n + 1);
	if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); return 2; }
	fclose(f);
	buf[n] = '\0';

	if (!ra_set_from_json(buf, (size_t)n, strtol(argv[1], NULL, 10), argv[3])) {
		fprintf(stderr, "raset-check: conversion produced nothing\n");
		return 1;
	}
	free(buf);
	return 0;
}
