/* SPDX-License-Identifier: MIT */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void strip(char *s)
{
	char *e = s + strlen(s);
	while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
		*--e = '\0';
	char *b = s;
	while (*b == ' ' || *b == '\t') b++;
	if (b != s) memmove(s, b, strlen(b) + 1);
}

/* split a line on '|' into up to n fields, in place */
static int split(char *line, char *fields[], int n)
{
	int c = 0;
	char *p = line;
	while (c < n) {
		fields[c++] = p;
		char *bar = strchr(p, '|');
		if (!bar) break;
		*bar = '\0';
		p = bar + 1;
	}
	for (int i = 0; i < c; i++) strip(fields[i]);
	return c;
}

/* sys|display name|rom folder|core|tag|card|RRGGBB|extensions|disc bios|maker
 * makers|first maker|second maker|...  - the Maker order's groups, in order */
bool cfg_load_systems(const char *path, systems_cfg *out)
{
	char makers[CFG_MAX_SYSTEMS][CFG_STR];
	char maker[CFG_MAX_SYSTEMS][CFG_STR];
	int nmakers = 0, rows = 0;

	memset(out, 0, sizeof *out);
	FILE *f = fopen(path, "r");
	if (!f) return false;
	char line[1024];
	while (fgets(line, sizeof line, f) && out->count < CFG_MAX_SYSTEMS) {
		strip(line);
		if (!line[0] || line[0] == '#') continue;
		char *fld[CFG_MAX_SYSTEMS + 1] = { 0 };
		int n = split(line, fld, CFG_MAX_SYSTEMS + 1);
		if (strcmp(fld[0], "makers") == 0) {
			for (int i = 1; i < n && nmakers < CFG_MAX_SYSTEMS; i++)
				snprintf(makers[nmakers++], CFG_STR, "%s", fld[i]);
			continue;
		}
		if (n < 5 || strcmp(fld[0], "sys") != 0) continue;
		system_cfg *s = &out->systems[out->count];
		snprintf(s->name, CFG_STR, "%s", fld[1]);
		snprintf(s->folder, CFG_STR, "%s", fld[2]);
		snprintf(s->core, CFG_STR, "%s", fld[3]);
		snprintf(s->tag, sizeof s->tag, "%s", fld[4]);
		if (n > 5) snprintf(s->card, CFG_STR, "%s", fld[5]);
		s->accent = 0x3DD6FF;
		if (n > 6 && fld[6][0]) s->accent = (unsigned)strtoul(fld[6], NULL, 16);
		if (n > 7) snprintf(s->exts, CFG_STR, "%s", fld[7]);
		if (n > 8) snprintf(s->disc_bios, CFG_STR, "%s", fld[8]);
		snprintf(maker[out->count], CFG_STR, "%s", n > 9 ? fld[9] : "");
		if (!s->name[0] || !s->folder[0] || !s->core[0] || !s->tag[0]) continue;
		s->row = rows++;
		out->count++;
	}
	fclose(f);
	/* After the loop, so the makers| line may sit anywhere in the file. */
	for (int i = 0; i < out->count; i++) {
		int m = 0;
		while (m < nmakers && strcmp(makers[m], maker[i]) != 0) m++;
		out->systems[i].maker = m;
	}
	return out->count > 0;
}

int cfg_order_cmp(const system_cfg *x, const system_cfg *y, sys_order o)
{
	int c = 0;

	if (o == SYS_ORDER_AZ) c = strcasecmp(x->name, y->name);
	else if (o == SYS_ORDER_MAKER) c = x->maker - y->maker;
	return c ? c : x->row - y->row;
}

sys_order cfg_order_index(const char *id)
{
	if (id)
		for (int i = 0; i < SYS_ORDER_COUNT; i++)
			if (strcmp(SYS_ORDERS[i].id, id) == 0) return (sys_order)i;
	return SYS_ORDER_YEAR;
}
