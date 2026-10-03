/* SPDX-License-Identifier: MIT */
/* See rajson.h. Everything here works on spans into the caller's buffer;
 * nothing is copied until js_str is asked for a string. */
#include <stdlib.h>
#include <string.h>

#include "rajson.h"

static const char *skip_ws(const char *p, const char *e)
{
	while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
	return p;
}

static const char *skip_string(const char *p, const char *e)
{
	p++;                                            /* the opening quote */
	while (p < e && *p != '"') p += (*p == '\\' && p + 1 < e) ? 2 : 1;
	return p < e ? p + 1 : e;
}

/* Past one value, whatever kind it is.
 *
 * Three cases, and the scalar one is not decoration: an earlier version fell
 * into the object loop for every kind and consumed exactly one character of a
 * number or a `true`, so iterating an object derailed on its first scalar
 * member. That is `{"Success": true, "PatchData": ...}`, which is to say every
 * reply RetroAchievements sends. */
static const char *skip_value(const char *p, const char *e)
{
	int depth = 0;

	p = skip_ws(p, e);
	if (p >= e) return e;

	if (*p == '"') return skip_string(p, e);

	if (*p != '{' && *p != '[') {
		/* number, true, false, null: up to the next structural character */
		while (p < e && *p != ',' && *p != '}' && *p != ']' &&
		       *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
			p++;
		return p;
	}

	do {
		if (p >= e) return e;
		if (*p == '"') { p = skip_string(p, e); continue; }
		if (*p == '{' || *p == '[') { depth++; p++; continue; }
		if (*p == '}' || *p == ']') { depth--; p++; continue; }
		p++;
	} while (depth > 0);

	return p;
}

jsv js_root(const char *text, size_t len)
{
	jsv v;
	v.b = text ? text : "";
	v.e = v.b + (text ? len : 0);
	v.b = skip_ws(v.b, v.e);
	return v;
}

bool js_member(jsv v, const char *name, jsv *out)
{
	const char *p = skip_ws(v.b, v.e);
	size_t want = strlen(name);

	if (p >= v.e || *p != '{') return false;
	p++;

	for (;;) {
		const char *k;
		size_t klen;

		p = skip_ws(p, v.e);
		if (p >= v.e || *p == '}') return false;
		if (*p != '"') return false;

		k = p + 1;
		p = skip_string(p, v.e);
		klen = (size_t)(p - k) - (p > k ? 1 : 0);   /* less the closing quote */

		p = skip_ws(p, v.e);
		if (p >= v.e || *p != ':') return false;
		p++;
		p = skip_ws(p, v.e);

		/* Compared raw. RetroAchievements' keys are plain ASCII with no
		 * escapes; a key that needed unescaping would fail to match here
		 * rather than matching something wrong. */
		if (klen == want && !memcmp(k, name, want)) {
			out->b = p;
			out->e = skip_value(p, v.e);
			return true;
		}
		p = skip_value(p, v.e);
		p = skip_ws(p, v.e);
		if (p < v.e && *p == ',') { p++; continue; }
		return false;
	}
}

bool js_next(jsv arr, jsv *it, jsv *elem)
{
	const char *p = skip_ws(arr.b, arr.e);

	if (p >= arr.e || *p != '[') return false;

	if (!it->b) {
		p++;                                   /* first element */
	} else {
		p = skip_ws(it->e, arr.e);
		if (p >= arr.e || *p != ',') return false;
		p++;
	}
	p = skip_ws(p, arr.e);
	if (p >= arr.e || *p == ']') return false;

	elem->b = p;
	elem->e = skip_value(p, arr.e);
	*it = *elem;
	return true;
}

static void put_utf8(char **o, char *end, unsigned cp)
{
	char *p = *o;

	if (cp < 0x80) {
		if (p + 1 >= end) return;
		*p++ = (char)cp;
	} else if (cp < 0x800) {
		if (p + 2 >= end) return;
		*p++ = (char)(0xC0 | (cp >> 6));
		*p++ = (char)(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		if (p + 3 >= end) return;
		*p++ = (char)(0xE0 | (cp >> 12));
		*p++ = (char)(0x80 | ((cp >> 6) & 0x3F));
		*p++ = (char)(0x80 | (cp & 0x3F));
	} else {
		if (p + 4 >= end) return;
		*p++ = (char)(0xF0 | (cp >> 18));
		*p++ = (char)(0x80 | ((cp >> 12) & 0x3F));
		*p++ = (char)(0x80 | ((cp >> 6) & 0x3F));
		*p++ = (char)(0x80 | (cp & 0x3F));
	}
	*o = p;
}

static unsigned hex4(const char *p)
{
	unsigned v = 0;
	int i;

	for (i = 0; i < 4; i++) {
		char c = p[i];
		v <<= 4;
		if      (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
		else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
		else return 0xFFFFFFFFu;
	}
	return v;
}

bool js_str(jsv v, char *out, size_t n)
{
	const char *p = skip_ws(v.b, v.e);
	char *o = out, *end = out + n;

	if (!out || n == 0) return false;
	out[0] = '\0';
	if (p >= v.e || *p != '"') return false;
	p++;

	while (p < v.e && *p != '"') {
		if (*p != '\\') { if (o + 1 < end) *o++ = *p; p++; continue; }
		if (++p >= v.e) break;
		switch (*p) {
		case 'n': if (o + 1 < end) *o++ = '\n'; p++; break;
		case 't': if (o + 1 < end) *o++ = '\t'; p++; break;
		case 'r': if (o + 1 < end) *o++ = '\r'; p++; break;
		case 'b': if (o + 1 < end) *o++ = '\b'; p++; break;
		case 'f': if (o + 1 < end) *o++ = '\f'; p++; break;
		case 'u': {
			unsigned cp;

			if (p + 4 >= v.e) { p = v.e; break; }
			cp = hex4(p + 1);
			p += 5;
			if (cp == 0xFFFFFFFFu) { cp = 0xFFFD; }
			else if (cp >= 0xD800 && cp <= 0xDBFF) {
				/* A high surrogate needs its pair. Without one, U+FFFD -
				 * a lone surrogate encoded as UTF-8 is a malformed sequence
				 * and SDL_ttf draws it as a box or drops the rest of the
				 * string. */
				if (p + 5 < v.e && p[0] == '\\' && p[1] == 'u') {
					unsigned lo = hex4(p + 2);
					if (lo >= 0xDC00 && lo <= 0xDFFF) {
						cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
						p += 6;
					} else {
						cp = 0xFFFD;
					}
				} else {
					cp = 0xFFFD;
				}
			} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
				cp = 0xFFFD;
			}
			put_utf8(&o, end, cp);
			break;
		}
		default:  if (o + 1 < end) *o++ = *p; p++; break;   /* \" \\ \/ */
		}
	}
	*o = '\0';
	return true;
}

long js_int(jsv v)
{
	const char *p = skip_ws(v.b, v.e);

	if (p < v.e && *p == '"') p++;      /* RA sends some numbers as strings */
	return strtol(p, NULL, 10);
}

bool js_is_true(jsv v)
{
	const char *p = skip_ws(v.b, v.e);
	return (size_t)(v.e - p) >= 4 && !memcmp(p, "true", 4);
}
