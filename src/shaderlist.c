/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* See shaderlist.h. */
#include "shaderlist.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Leading and trailing blanks off, in place. */
static char *trim(char *s)
{
	char *e;

	while (isspace((unsigned char)*s)) s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
	return s;
}

/* The same grammar diatom checks (its ADR-0041), checked here too so a bad
 * line is caught when the list is read rather than when a player picks it. */
static bool passes_ok(const char *passes)
{
	char buf[256], *pass, *save = NULL;
	int n = 0;

	if (strlen(passes) >= sizeof buf) return false;
	strcpy(buf, passes);
	for (pass = strtok_r(buf, ",", &save); pass; pass = strtok_r(NULL, ",", &save)) {
		char *f, *sc, *end;
		long scale;

		pass = trim(pass);
		if (++n > 3) return false;
		if (!(sc = strrchr(pass, ':'))) return false;
		*sc++ = '\0';
		if (!(f = strrchr(pass, ':'))) return false;
		*f++ = '\0';
		scale = strtol(sc, &end, 10);
		if (!*pass || end == sc || *end || scale < 0 || scale > 4) return false;
		if (strcmp(f, "nearest") && strcmp(f, "linear")) return false;
	}
	return n > 0;
}

int sl_load(sl_list *l, const char *path)
{
	char line[512];
	FILE *fp;
	int ln = 0;

	memset(l, 0, sizeof *l);
	snprintf(l->e[0].name, SL_NAME, "None");
	l->count = 1;
	if (!(fp = fopen(path, "r"))) return l->count;

	while (fgets(line, sizeof line, fp)) {
		char *name, *passes, *final, *s = line;
		sl_entry *e;

		ln++;
		if (*trim(s) == '\0' || *trim(s) == '#') continue;
		name = strtok(s, "|");
		passes = strtok(NULL, "|");
		final = strtok(NULL, "|");
		if (!name || !passes || !final || strtok(NULL, "|")) goto bad;
		name = trim(name); passes = trim(passes); final = trim(final);
		if (!*name || strlen(name) >= SL_NAME || !strcmp(name, "None")) goto bad;
		if (!passes_ok(passes) || strlen(passes) >= sizeof e->passes) goto bad;
		if (strcmp(final, "nearest") && strcmp(final, "linear")) goto bad;
		if (sl_find(l, name)) goto bad;           /* a second entry by that name */
		if (l->count == SL_MAX) {
			fprintf(stderr, "shaders: %s: more than %d entries, the rest left out\n",
			        path, SL_MAX - 1);
			break;
		}
		e = &l->e[l->count++];
		snprintf(e->name, sizeof e->name, "%s", name);
		snprintf(e->passes, sizeof e->passes, "%s", passes);
		snprintf(e->final, sizeof e->final, "%s", final);
		continue;
	bad:
		fprintf(stderr, "shaders: %s:%d: not an entry, left out\n", path, ln);
	}
	fclose(fp);
	return l->count;
}

int sl_find(const sl_list *l, const char *name)
{
	int i;

	if (!name || !*name) return 0;
	for (i = 1; i < l->count; i++)
		if (!strcmp(l->e[i].name, name)) return i;
	return 0;
}

bool sl_fields(const sl_list *l, int i, const char *dir, char *out, size_t cap)
{
	char buf[256], *pass, *save = NULL;
	size_t n;

	if (i <= 0 || i >= l->count)
		return (size_t)snprintf(out, cap, "shader=none") < cap;
	n = (size_t)snprintf(out, cap, "shader=");
	strcpy(buf, l->e[i].passes);
	for (pass = strtok_r(buf, ",", &save); pass; pass = strtok_r(NULL, ",", &save)) {
		char *opts;

		pass = trim(pass);
		opts = strchr(pass, ':');       /* passes_ok: file:filter:scale */
		*opts++ = '\0';
		if (n >= cap) return false;
		n += (size_t)snprintf(out + n, cap - n, "%s%s/%s.glsl:%s",
		                      n > 7 ? "," : "", dir, pass, opts);
	}
	if (n >= cap) return false;
	n += (size_t)snprintf(out + n, cap - n, "\tfinal=%s", l->e[i].final);
	return n < cap;
}
