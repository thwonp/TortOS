/* SPDX-License-Identifier: MIT */
/* See ssfetch.h for what is pure here and why. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ssfetch.h"

#include "net.h"
#include "rajson.h"
#include "ss.h"

#define SS_API  "https://api.screenscraper.fr/api2"

/* Their id for each shelf folder. Measured against systemesListe.php on
 * 2026-09-16 by matching on letters and digits alone, because they write
 * "Neo-Geo Pocket Color" where systems.cfg writes "Neo Geo Pocket Color" and a
 * hyphen is not a reason to leave 92 games unlooked-up.
 *
 * TurboGrafx-16 covers its CD games too. They have a separate id for PC Engine
 * CD (114), and asking either one returned the same games with the same covers
 * and prose, so the shelf's single folder needs only the single id.
 *
 * Arcade (75) and Neo Geo (142) were not measured here: taken 2026-10-01 from
 * Skyscraper's platforms_idmap.csv, which agrees with every id above. */
static const struct { const char *folder; int id; } SYS[] = {
	{ "Arcade",               75 },
	{ "NES",                   3 },
	{ "Master System",         2 },
	{ "Game Boy",              9 },
	{ "Genesis",               1 },
	{ "TurboGrafx-16",        31 },
	{ "Game Gear",            21 },
	{ "Neo Geo",             142 },
	{ "SNES",                  4 },
	{ "PlayStation",          57 },
	{ "Neo Geo Pocket",       25 },
	{ "Game Boy Color",       10 },
	{ "Neo Geo Pocket Color", 82 },
	{ "Game Boy Advance",     12 },
};

int ss_system_id(const char *folder)
{
	size_t i;

	if (!folder) return 0;
	for (i = 0; i < sizeof SYS / sizeof SYS[0]; i++)
		if (strcmp(SYS[i].folder, folder) == 0) return SYS[i].id;
	return 0;
}

/* ---- is the reply about the game we asked about? ------------------------- */

/* A title as comparable words. Region tags and a file's extension go,
 * punctuation splits, articles drop, and a roman numeral becomes a digit -
 * "Dragon Quest III" and "Dragon Quest 3" are the same game and one of them is
 * what the file is called.
 *
 * THE EXTENSION ONLY FROM A FILE NAME. Their titles have none, and taking the
 * last dot as one cut "Super Mario Bros. 3" to "Super Mario Bros", so its 3
 * never matched the file's and a reply about the right game was thrown away.
 * Seen on the fresh card 2026-09-26. */
#define TOK_MAX 24
#define TOK_LEN 24

static int tokens(const char *s, bool is_file, char out[TOK_MAX][TOK_LEN])
{
	static const char *drop[] = { "the", "a", "an", "of", "de", "le", "la" };
	static const struct { const char *r; const char *d; } ROMAN[] = {
		{ "i", "1" }, { "ii", "2" }, { "iii", "3" }, { "iv", "4" }, { "v", "5" },
		{ "vi", "6" }, { "vii", "7" }, { "viii", "8" }, { "ix", "9" }, { "x", "10" },
	};
	char buf[512];
	size_t bi = 0;
	int n = 0, depth = 0;
	const char *p;

	/* Drop (USA), [!], (Rev 1) and the extension before anything is split:
	 * a region tag is about the dump, not the game. */
	for (p = s; *p && bi + 1 < sizeof buf; p++) {
		if (*p == '(' || *p == '[') { depth++; continue; }
		if (*p == ')' || *p == ']') { if (depth) depth--; continue; }
		if (!depth) buf[bi++] = *p;
	}
	buf[bi] = '\0';
	if (is_file && (p = strrchr(buf, '.')) && strlen(p) <= 5 && p != buf)
		*(char *)p = '\0';

	for (p = buf; *p; ) {
		char w[TOK_LEN];
		size_t wi = 0;
		size_t k;

		while (*p && !isalnum((unsigned char)*p)) p++;
		while (*p && isalnum((unsigned char)*p)) {
			if (wi + 1 < sizeof w) w[wi++] = (char)tolower((unsigned char)*p);
			p++;
		}
		if (!wi) continue;
		w[wi] = '\0';
		for (k = 0; k < sizeof drop / sizeof drop[0]; k++)
			if (strcmp(w, drop[k]) == 0) break;
		if (k < sizeof drop / sizeof drop[0]) continue;
		for (k = 0; k < sizeof ROMAN / sizeof ROMAN[0]; k++)
			if (strcmp(w, ROMAN[k].r) == 0) {
				snprintf(w, sizeof w, "%s", ROMAN[k].d);
				break;
			}
		if (n < TOK_MAX) snprintf(out[n++], TOK_LEN, "%s", w);
	}
	return n;
}

static int digits_of(const char *s, bool is_file, char out[TOK_MAX][TOK_LEN])
{
	char all[TOK_MAX][TOK_LEN];
	int n = tokens(s, is_file, all), i, k = 0;

	for (i = 0; i < n; i++) {
		const char *c = all[i];

		while (*c && isdigit((unsigned char)*c)) c++;
		/* memcpy, not snprintf: both are TOK_LEN arrays and the source is
		 * already terminated inside one, but the cross compiler cannot see
		 * that and warns the copy may truncate. A warning nobody can act on is
		 * a warning everybody learns to scroll past. */
		if (*c == '\0' && all[i][0]) memcpy(out[k++], all[i], TOK_LEN);
	}
	return k;
}

bool ss_name_ok(const char *file, const char *const *names, int n)
{
	char want[TOK_MAX][TOK_LEN], got[TOK_MAX][TOK_LEN];
	int nw, ng, i, j, k;

	if (!file) return true;
	nw = digits_of(file, true, want);
	if (nw == 0) return true;            /* nothing here to check it with */

	for (i = 0; i < n; i++) {
		if (!names[i]) continue;
		ng = digits_of(names[i], false, got);
		for (j = 0; j < nw; j++)
			for (k = 0; k < ng; k++)
				if (strcmp(want[j], got[k]) == 0) return true;
	}
	return false;
}

/* ---- reading a reply ----------------------------------------------------- */

/* The first of `regions` that this array carries, else its first entry.
 * ScreenScraper answers with every region it knows and leaves the choosing to
 * the caller; "us" first is what the account's own favregion says. */
static bool pick_region(jsv arr, const char *const *regions, int nreg,
                        const char *key, char *out, size_t n)
{
	jsv it = { 0 }, e, v;
	int r;

	out[0] = '\0';
	for (r = 0; r < nreg; r++) {
		it = (jsv){ 0 };
		while (js_next(arr, &it, &e)) {
			char reg[16];

			if (!js_member(e, "region", &v) || !js_str(v, reg, sizeof reg))
				continue;
			if (strcmp(reg, regions[r]) != 0) continue;
			if (js_member(e, key, &v) && js_str(v, out, n) && out[0]) return true;
		}
	}
	it = (jsv){ 0 };
	if (js_next(arr, &it, &e) && js_member(e, key, &v) && js_str(v, out, n))
		return out[0] != '\0';
	return false;
}

/* `{ "text": ... }`, which is how a single-valued field arrives. */
static void text_of(jsv parent, const char *key, char *out, size_t n)
{
	jsv o, v;

	out[0] = '\0';
	if (js_member(parent, key, &o) && js_member(o, "text", &v))
		js_str(v, out, n);
}

/* The five entities their prose actually carries. Measured over 78 replies on
 * 2026-09-15: only &quot; appeared, and the others cost four lines to cover
 * rather than a later evening wondering why one synopsis says &amp;. */
static void unescape(char *s)
{
	static const struct { const char *e; char c; } ENT[] = {
		{ "&quot;", '"' }, { "&apos;", '\'' }, { "&#39;", '\'' },
		{ "&lt;", '<' }, { "&gt;", '>' }, { "&amp;", '&' },
	};
	char *r = s, *w = s;

	while (*r) {
		size_t i;

		if (*r == '&') {
			for (i = 0; i < sizeof ENT / sizeof ENT[0]; i++) {
				size_t n = strlen(ENT[i].e);

				if (strncmp(r, ENT[i].e, n) == 0) {
					*w++ = ENT[i].c;
					r += n;
					break;
				}
			}
			if (i < sizeof ENT / sizeof ENT[0]) continue;
		}
		*w++ = *r++;
	}
	*w = '\0';
}

/* The English synopsis. No fallback to another language on purpose: a
 * paragraph of French under an English menu is worse than the row saying
 * nothing, and the row already knows how to say nothing. */
static void synopsis_of(jsv jeu, char *out, size_t n)
{
	jsv arr, it = { 0 }, e, v;

	out[0] = '\0';
	if (!js_member(jeu, "synopsis", &arr)) return;
	while (js_next(arr, &it, &e)) {
		char lang[8];

		if (!js_member(e, "langue", &v) || !js_str(v, lang, sizeof lang)) continue;
		if (strcmp(lang, "en") != 0) continue;
		if (js_member(e, "text", &v) && js_str(v, out, n)) {
			unescape(out);
			return;
		}
	}
}

static void genres_of(jsv jeu, char *out, size_t n)
{
	jsv arr, it = { 0 }, e, noms, nit, ne, v;
	size_t used = 0;

	out[0] = '\0';
	if (!js_member(jeu, "genres", &arr)) return;
	while (js_next(arr, &it, &e)) {
		if (!js_member(e, "noms", &noms)) continue;
		nit = (jsv){ 0 };
		while (js_next(noms, &nit, &ne)) {
			char lang[8], name[64];

			if (!js_member(ne, "langue", &v) || !js_str(v, lang, sizeof lang)) continue;
			if (strcmp(lang, "en") != 0) continue;
			if (!js_member(ne, "text", &v) || !js_str(v, name, sizeof name)) continue;
			if (used && used + 1 < n) out[used++] = ',';
			used += (size_t)snprintf(out + used, n - used, "%s", name);
			if (used >= n - 1) return;
		}
	}
}

static void esrb_of(jsv jeu, char *out, size_t n)
{
	jsv arr, it = { 0 }, e, v;

	out[0] = '\0';
	if (!js_member(jeu, "classifications", &arr)) return;
	while (js_next(arr, &it, &e)) {
		char type[16];

		if (!js_member(e, "type", &v) || !js_str(v, type, sizeof type)) continue;
		if (strcmp(type, "ESRB") != 0) continue;
		if (js_member(e, "text", &v) && js_str(v, out, n)) return;
	}
}

/* The box-2D cover, in the region the shelf would rather have. */
static void art_of(jsv jeu, ss_result *out)
{
	static const char *REG[] = { "us", "wor", "ss", "eu", "au", "br", "jp", "kr" };
	jsv arr, it = { 0 }, e, v;
	int best = -1;

	out->art[0] = '\0';
	out->art_region[0] = '\0';
	if (!js_member(jeu, "medias", &arr)) return;
	while (js_next(arr, &it, &e)) {
		char type[32], reg[16], url[512];
		int rank;

		if (!js_member(e, "type", &v) || !js_str(v, type, sizeof type)) continue;
		if (strcmp(type, "box-2D") != 0) continue;
		if (!js_member(e, "url", &v) || !js_str(v, url, sizeof url) || !url[0]) continue;
		reg[0] = '\0';
		if (js_member(e, "region", &v)) js_str(v, reg, sizeof reg);
		for (rank = 0; rank < (int)(sizeof REG / sizeof REG[0]); rank++)
			if (strcmp(reg, REG[rank]) == 0) break;
		if (best >= 0 && rank >= best) continue;
		best = rank;
		snprintf(out->art, sizeof out->art, "%s", url);
		snprintf(out->art_region, sizeof out->art_region, "%s", reg);
	}
}

bool ss_parse(const char *json, size_t len, const char *file, ss_result *out)
{
	static const char *TEXT_REG[] = { "us", "wor", "ss", "eu", "jp" };
	const char *names[8];
	char namebuf[8][128];
	jsv root, resp, jeu, arr, it = { 0 }, e, v;
	int nn = 0;

	memset(out, 0, sizeof *out);
	if (!json || !len) return false;

	root = js_root(json, len);
	if (!js_member(root, "response", &resp)) return false;
	/* The account's own counters, read before the game and kept even when the
	 * reply turns out not to be about one. They arrive with every call, and
	 * they are the only honest way to know how much of the day is left.
	 *
	 * AS STRINGS, not numbers: "requeststoday": "20". js_int answers 0 for a
	 * string, which would have read as a fresh quota on every reply - the
	 * worst possible direction for that mistake. */
	{
		jsv who, n;
		char num[16];

		if (js_member(resp, "ssuser", &who)) {
			if (js_member(who, "requeststoday", &n) && js_str(n, num, sizeof num))
				out->used_today = atoi(num);
			if (js_member(who, "maxrequestsperday", &n) && js_str(n, num, sizeof num))
				out->max_today = atoi(num);
		}
	}
	if (!js_member(resp, "jeu", &jeu)) return false;
	out->found = true;

	if (js_member(jeu, "noms", &arr)) {
		while (nn < 8 && js_next(arr, &it, &e)) {
			if (js_member(e, "text", &v) &&
			    js_str(v, namebuf[nn], sizeof namebuf[nn]) && namebuf[nn][0]) {
				names[nn] = namebuf[nn];
				nn++;
			}
		}
		pick_region(arr, TEXT_REG, 5, "text", out->name, sizeof out->name);
	}
	if (!out->name[0] && nn) snprintf(out->name, sizeof out->name, "%s", names[0]);
	out->name_ok = ss_name_ok(file, names, nn);

	if (js_member(jeu, "dates", &arr)) {
		char date[32];

		if (pick_region(arr, TEXT_REG, 5, "text", date, sizeof date))
			snprintf(out->meta.year, sizeof out->meta.year, "%.4s", date);
	}
	text_of(jeu, "editeur",     out->meta.publisher, sizeof out->meta.publisher);
	text_of(jeu, "developpeur", out->meta.developer, sizeof out->meta.developer);
	text_of(jeu, "joueurs",     out->meta.players,   sizeof out->meta.players);
	text_of(jeu, "note",        out->meta.note,      sizeof out->meta.note);
	genres_of(jeu, out->meta.genres, sizeof out->meta.genres);
	esrb_of(jeu, out->meta.esrb, sizeof out->meta.esrb);
	synopsis_of(jeu, out->meta.synopsis, sizeof out->meta.synopsis);
	art_of(jeu, out);
	return true;
}

/* ---- asking ------------------------------------------------------------- */

bool ss_lookup_url(char *out, size_t n, const char *folder, const char *file,
                   uint32_t crc)
{
	char auth[512], enc[512];
	int id = ss_system_id(folder);
	int len;

	if (!out || !n) return false;
	out[0] = '\0';
	if (!file || id == 0) return false;
	/* The credentials arrive already encoded and already joined; this file
	 * never sees the password. See ss.h. */
	if (!ss_auth_query(auth, sizeof auth)) return false;
	net_urlencode(file, enc, sizeof enc);

	/* The name goes on BOTH forms. Measured 2026-09-16: a checksum with no
	 * name is answered "no answer" - crc=1F3C05A1 alone found nothing where
	 * the same crc with the file's name found Gunstar Heroes. The size is not
	 * needed either way, so it is not sent. */
	if (crc)
		len = snprintf(out, n, "%s/jeuInfos.php?%s&systemeid=%d&romtype=rom"
		                       "&crc=%08X&romnom=%s",
		               SS_API, auth, id, crc, enc);
	else
		len = snprintf(out, n, "%s/jeuInfos.php?%s&systemeid=%d&romtype=rom"
		                       "&romnom=%s",
		               SS_API, auth, id, enc);

	memset(auth, 0, sizeof auth);
	if (len < 0 || (size_t)len >= n) { out[0] = '\0'; return false; }
	return true;
}

/* One request, into `out`. */
static bool ask(const char *folder, const char *file, uint32_t crc, ss_result *out)
{
	char *body;
	char url[1024];
	long got;
	bool ok = false;

	memset(out, 0, sizeof *out);
	if (!ss_lookup_url(url, sizeof url, folder, file, crc)) return false;
	if (!(body = malloc(SS_REPLY_MAX))) return false;

	got = net_get_buf(url, NULL, 0, body, SS_REPLY_MAX, SS_WAIT_S);
	memset(url, 0, sizeof url);          /* it carried both credentials */
	if (got > 0) ok = ss_parse(body, (size_t)got, file, out);
	free(body);
	return ok;
}

bool ss_lookup(const char *folder, const char *file, uint32_t crc, ss_result *out)
{
	ss_result byname;

	if (!ask(folder, file, crc, out)) return false;
	if (!crc || !out->found || out->name_ok) return true;

	/* A CHECKSUM ANSWER THAT FAILS THE NAME CHECK IS ASKED AGAIN BY NAME,
	 * and that is worth a second request because it is usually right.
	 *
	 * Measured 2026-09-16 on the four games this card's whole run got wrong:
	 *
	 *   Kid Niki 2             crc -> Kid Niki - Radical Ninja
	 *                         name -> Kaiketsu Yancha Maru 2      RIGHT
	 *   Shin Megami Tensei II   crc -> Shin Megami Tensei
	 *                         name -> Shin Megami Tensei 2        RIGHT
	 *   Mother 3                crc -> ZZZ(notgame):#NONGAME
	 *                         name -> Mother 3                    RIGHT
	 *   Dragon Quest III        crc -> Dragon Quest 1 And 2
	 *                         name -> Dragon Quest 1 And 2        still wrong
	 *
	 * Three of four rescued, and the fourth still fails the check on the
	 * retry, so it falls through to libretro as it should. The retry costs one
	 * request on the 9 games in 1,708 that get flagged, and nothing on the
	 * rest.
	 *
	 * The retry is NOT trusted blindly: it has to pass the same check. Asking
	 * by name is how "Kid Niki 2" found the first Kid Niki in the first place,
	 * so a name answer is no more inherently right than a checksum one. */
	if (ask(folder, file, 0, &byname) && byname.found && byname.name_ok) {
		*out = byname;
		return true;
	}
	return true;      /* the flagged answer stands; the caller decides */
}
