/* gamelist.xml import - see gamelist.h.
 *
 * Not an XML parser. An ES gamelist is flat: <game> blocks, each a handful of
 * text-only child tags, and the device has no XML library to lean on. So this
 * finds the blocks and the tags by name and decodes what XML can put in text -
 * the five named entities, numeric ones, CDATA and line-end normalization.
 * Anything subtler than that is not in any gamelist a scraper writes. */
#include "gamelist.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Bounded strstr: gamelists are read whole and are not NUL-safe to search
 * past a block's end. */
static const char *seek(const char *p, const char *end, const char *s)
{
	size_t n = strlen(s);

	for (; p + n <= end; p++)
		if (*p == *s && !memcmp(p, s, n)) return p;
	return NULL;
}

/* "<name" followed by what can end a tag name - so <game> is not found inside
 * <gameList>, nor <name> inside <names>. */
static const char *seek_open(const char *p, const char *end, const char *name,
                             size_t n)
{
	while ((p = seek(p, end, name))) {
		const char *q = p + n;

		if (q < end && (*q == '>' || *q == '/' || *q == ' ' || *q == '\t' ||
		                *q == '\r' || *q == '\n'))
			return p;
		p++;
	}
	return NULL;
}

static void put_utf8(char *out, size_t *o, size_t cap, unsigned long c)
{
	char b[4];
	size_t n, i;

	if (c < 0x80)         { b[0] = (char)c; n = 1; }
	else if (c < 0x800)   { b[0] = (char)(0xC0 | c >> 6);
	                        b[1] = (char)(0x80 | (c & 0x3F)); n = 2; }
	else if (c < 0x10000) { b[0] = (char)(0xE0 | c >> 12);
	                        b[1] = (char)(0x80 | (c >> 6 & 0x3F));
	                        b[2] = (char)(0x80 | (c & 0x3F)); n = 3; }
	else if (c < 0x110000){ b[0] = (char)(0xF0 | c >> 18);
	                        b[1] = (char)(0x80 | (c >> 12 & 0x3F));
	                        b[2] = (char)(0x80 | (c >> 6 & 0x3F));
	                        b[3] = (char)(0x80 | (c & 0x3F)); n = 4; }
	else return;
	/* A character goes in whole or not at all: a synopsis cut short is fine,
	 * one ending in half a character draws as a box. */
	if (*o + n > cap) { *o = cap; return; }
	for (i = 0; i < n; i++) out[(*o)++] = b[i];
}

/* Raw bytes of a UTF-8 string, whole characters only. */
static void put_bytes(char *out, size_t *o, size_t cap, const char *s, size_t n)
{
	size_t i = 0;

	while (i < n && *o < cap) {
		unsigned char c = (unsigned char)s[i];
		size_t k = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;

		if (i + k > n) k = n - i;
		if (*o + k > cap) { *o = cap; return; }
		/* XML reads CR LF, and a lone CR, as LF. */
		if (c == '\r') {
			out[(*o)++] = '\n';
			if (i + 1 < n && s[i + 1] == '\n') i++;
			i++;
			continue;
		}
		memcpy(out + *o, s + i, k);
		*o += k;
		i += k;
	}
}

static const struct { const char *name; char c; } ENTITY[] = {
	{ "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' },
};

/* Text between a tag's '>' and its close, decoded into out (NUL-terminated,
 * at most size - 1 bytes). */
static void decode(const char *p, const char *end, char *out, size_t size)
{
	size_t o = 0, cap = size - 1;

	while (p < end && o < cap) {
		const char *q;

		if (end - p >= 9 && !memcmp(p, "<![CDATA[", 9)) {
			q = seek(p + 9, end, "]]>");
			if (!q) q = end;
			put_bytes(out, &o, cap, p + 9, (size_t)(q - p - 9));
			p = q + (q < end ? 3 : 0);
			continue;
		}
		if (*p == '&' && (q = memchr(p, ';', (size_t)(end - p))) && q - p <= 10) {
			size_t n = (size_t)(q - p - 1), i;
			unsigned long c = 0;
			char *e;
			bool ok = false;

			if (n > 1 && p[1] == '#') {
				c = p[2] == 'x' || p[2] == 'X' ? strtoul(p + 3, &e, 16)
				                               : strtoul(p + 2, &e, 10);
				ok = e == q;
			} else {
				for (i = 0; i < sizeof ENTITY / sizeof *ENTITY; i++)
					if (strlen(ENTITY[i].name) == n &&
					    !memcmp(p + 1, ENTITY[i].name, n)) {
						c = (unsigned char)ENTITY[i].c;
						ok = true;
						break;
					}
			}
			if (ok) {
				put_utf8(out, &o, cap, c);
				p = q + 1;
				continue;
			}
		}
		q = p + 1;
		while (q < end && *q != '&' && *q != '<') q++;
		put_bytes(out, &o, cap, p, (size_t)(q - p));
		p = q;
	}
	out[o] = '\0';
}

/* The text of <name> inside [p, end), decoded; "" when absent or empty. */
static void field(const char *p, const char *end, const char *name,
                  char *out, size_t size)
{
	char open[32], close[32];
	const char *a, *b;
	size_t n;

	out[0] = '\0';
	n = (size_t)snprintf(open, sizeof open, "<%s", name);
	snprintf(close, sizeof close, "</%s>", name);
	if (!(a = seek_open(p, end, open, n))) return;
	if (!(a = memchr(a, '>', (size_t)(end - a)))) return;
	if (a[-1] == '/') return;                            /* <name/> */
	a++;
	if (!(b = seek(a, end, close))) return;
	decode(a, b, out, size);
}

/* tools/gamelist-import.py's clean(): a tab or line break becomes a space,
 * and the ends are trimmed. */
static void clean(char *s)
{
	char *p, *e;

	for (p = s; *p; p++)
		if (*p == '\t' || *p == '\n' || *p == '\r') *p = ' ';
	for (p = s; *p == ' '; p++)
		;
	e = p + strlen(p);
	while (e > p && e[-1] == ' ') e--;
	memmove(s, p, (size_t)(e - p));
	s[e - p] = '\0';
}

int gl_parse(const char *xml, size_t len, gl_game_fn fn, void *ctx)
{
	const char *end = xml + len, *p, *b, *e;
	int n = 0;

	if (!seek(xml, end, "<gameList")) return -1;
	for (p = xml; (b = seek_open(p, end, "<game", 5)); p = e) {
		char path[1024], raw[64], file[1024], *slash;
		game_meta m;
		double v;

		/* <game/>, or a block with no end: nothing to read in either. */
		if (!(e = memchr(b, '>', (size_t)(end - b)))) break;
		if (e[-1] == '/') continue;
		if (!(e = seek(b, end, "</game>"))) break;

		field(b, e, "path", path, sizeof path);
		if (!path[0]) continue;
		slash = strrchr(path, '/');
		snprintf(file, sizeof file, "%s", slash ? slash + 1 : path);
		clean(file);
		if (!file[0]) continue;

		memset(&m, 0, sizeof m);
		field(b, e, "name", m.title, sizeof m.title);
		clean(m.title);
		/* First four digits of an ES releasedate (YYYYMMDDT000000); any other
		 * shape is a game with no known year. */
		field(b, e, "releasedate", raw, sizeof raw);
		clean(raw);
		if (strlen(raw) >= 4 && strspn(raw, "0123456789") >= 4)
			snprintf(m.year, sizeof m.year, "%.4s", raw);
		field(b, e, "publisher", m.publisher, sizeof m.publisher);
		clean(m.publisher);
		field(b, e, "developer", m.developer, sizeof m.developer);
		clean(m.developer);
		field(b, e, "players", m.players, sizeof m.players);
		clean(m.players);
		field(b, e, "genre", m.genres, sizeof m.genres);
		clean(m.genres);
		/* ES rates 0.0-1.0; TortOS's note is out of 20. 0 is ES for "never
		 * rated", so it stays blank rather than reading as the worst score.
		 * nearbyint rounds half to even, as Python's round() does. */
		field(b, e, "rating", raw, sizeof raw);
		v = strtod(raw, NULL);
		if (v > 0 && v < 1000)
			snprintf(m.note, sizeof m.note, "%.0f", nearbyint(v * 20));
		field(b, e, "desc", m.synopsis, sizeof m.synopsis);

		fn(ctx, file, &m);
		n++;
		e += 7;
	}
	return n;
}

/* The whole file, NUL-terminated, or NULL. */
static char *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	char *buf = NULL;
	long n;

	if (!f) return NULL;
	if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 && n < 64L << 20 &&
	    fseek(f, 0, SEEK_SET) == 0 && (buf = malloc((size_t)n + 1))) {
		if (fread(buf, 1, (size_t)n, f) == (size_t)n) {
			buf[n] = '\0';
			*len = (size_t)n;
		} else {
			free(buf);
			buf = NULL;
		}
	}
	fclose(f);
	return buf;
}

typedef struct {
	db *d;
	const char *folder;
	bool overwrite;
	gl_result *r;
} import_ctx;

static void import_one(void *ctx, const char *file, const game_meta *m)
{
	import_ctx *c = ctx;
	int got = db_game_import(c->d, c->folder, file, m, c->overwrite);

	if (got > 0) c->r->wrote++;
	else if (got == 0) c->r->skipped++;
	else c->r->bad++;
}

void gl_import(db *d, const char *roms, const systems_cfg *sys, bool overwrite,
               gl_result *r)
{
	static const char *const NAMES[] = { "gamelist.xml", "miyoogamelist.xml" };
	int s;

	memset(r, 0, sizeof *r);
	for (s = 0; s < sys->count; s++) {
		const char *folder = sys->systems[s].folder;
		char path[CFG_STR * 3];
		char *xml = NULL;
		size_t len = 0, k;
		bool found = false;

		if (!folder[0]) continue;
		for (k = 0; k < sizeof NAMES / sizeof *NAMES && !found; k++) {
			snprintf(path, sizeof path, "%s/%s/%s", roms, folder, NAMES[k]);
			if (access(path, F_OK) == 0) found = true;
		}
		if (!found) continue;
		xml = slurp(path, &len);
		if (xml) {
			import_ctx c = { d, folder, overwrite, r };

			if (gl_parse(xml, len, import_one, &c) >= 0) {
				r->lists++;
				free(xml);
				continue;
			}
			free(xml);
		}
		if (!r->unreadable++)
			snprintf(r->first_unreadable, sizeof r->first_unreadable, "%s",
			         folder);
	}
}
