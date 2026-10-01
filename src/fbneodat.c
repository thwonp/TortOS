/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include "fbneodat.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The file held whole, each tab and newline turned to a NUL, so a set name is
 * a string and its description is the string after it. 8,382 sets. */
static char *g_buf;
static const char **g_set;
static int g_sets;
static bool g_loaded;

static void load(const char *path)
{
	FILE *f;
	long len = -1;
	char *p, *end;
	int lines = 0;

	g_loaded = true;
	if (!(f = fopen(path, "rb"))) {
		fprintf(stderr, "fbneodat: no %s, so arcade shelves show set names\n", path);
		return;
	}
	if (fseek(f, 0, SEEK_END) == 0) len = ftell(f);
	if (len > 0 && len < 16L << 20 && fseek(f, 0, SEEK_SET) == 0 &&
	    (g_buf = malloc((size_t)len + 1)) &&
	    fread(g_buf, 1, (size_t)len, f) == (size_t)len) {
		g_buf[len] = '\0';
		for (p = g_buf; *p; p++) lines += *p == '\n';
		g_set = malloc(((size_t)lines + 1) * sizeof *g_set);
	}
	fclose(f);
	if (!g_set) {
		fprintf(stderr, "fbneodat: cannot read %s\n", path);
		free(g_buf);
		g_buf = NULL;
		return;
	}
	/* One `set<TAB>description` per line, already in strcmp order. A line
	 * with no tab is not a row and is passed over. */
	for (p = g_buf, end = g_buf + len; p < end; ) {
		char *nl = memchr(p, '\n', (size_t)(end - p)), *tab;

		if (!nl) nl = end;
		*nl = '\0';
		if ((tab = strchr(p, '\t')) && tab != p && tab[1]) {
			*tab = '\0';
			g_set[g_sets++] = p;
		}
		p = nl + 1;
	}
	fprintf(stderr, "fbneodat: %d arcade sets from %s\n", g_sets, path);
}

static int by_set(const void *key, const void *row)
{
	return strcmp(key, *(const char *const *)row);
}

const char *fbneo_desc(const char *path, const char *set)
{
	char low[256];
	const char **row;
	size_t k;

	if (!path || !set) return NULL;
	if (!g_loaded) load(path);
	if (!g_sets) return NULL;
	/* Sets are lower case; a zip renamed MK3.ZIP is still mk3. */
	for (k = 0; set[k] && k < sizeof low - 1; k++)
		low[k] = (char)tolower((unsigned char)set[k]);
	low[k] = '\0';
	row = bsearch(low, g_set, (size_t)g_sets, sizeof *g_set, by_set);
	return row ? *row + strlen(*row) + 1 : NULL;
}
