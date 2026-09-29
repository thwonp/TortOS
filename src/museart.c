/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* See museart.h. */
#include "museart.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "net.h"
#include "rajson.h"

/* MusicBrainz's rate is a request a second; the margin is so a clock tick
 * early is not a request early. 503 is how it says to slow down, and one is
 * answered by waiting three seconds and asking once more. */
#define MB_GAP_MS  1100
#define MB_BUSY_MS 3000

bool museart_pick(const char *json, size_t len, int tracks, char *rg, size_t n)
{
	jsv root, rels, it = { 0 }, r;
	int best_d = INT_MAX, best_score = -1;
	char best[64] = "";

	if (!json || !len) return false;
	root = js_root(json, len);
	if (!js_member(root, "releases", &rels)) return false;
	while (js_next(rels, &it, &r)) {
		jsv v, g;
		char id[64];
		int score = 0, tc = -1, d;

		if (js_member(r, "score", &v)) score = (int)js_int(v);
		/* Below 90 is MusicBrainz reaching: a live album with the title in
		 * its name, a remix, a different record by a similar name. */
		if (score < 90) continue;
		if (!js_member(r, "release-group", &g) || !js_member(g, "id", &v) ||
		    !js_str(v, id, sizeof id) || !id[0])
			continue;
		if (js_member(r, "track-count", &v)) tc = (int)js_int(v);
		d = tc < 0 ? INT_MAX - 1 : abs(tc - tracks);
		if (d < best_d || (d == best_d && score > best_score)) {
			best_d = d;
			best_score = score;
			snprintf(best, sizeof best, "%s", id);
		}
	}
	if (!best[0]) return false;
	snprintf(rg, n, "%s", best);
	return true;
}

/* A name as the inside of a quoted phrase: the quote and the backslash are
 * the two characters a phrase gives meaning, so they are escaped and nothing
 * else is. An album called Doolittle (Deluxe) is a phrase with parentheses in
 * it, not a group. */
static bool phrase(const char *in, char *out, size_t n)
{
	size_t o = 0;

	for (; *in; in++) {
		if (*in == '"' || *in == '\\') {
			if (o + 2 >= n) return false;
			out[o++] = '\\';
		}
		if (o + 1 >= n) return false;
		out[o++] = *in;
	}
	out[o] = '\0';
	return true;
}

bool museart_search_url(const char *artist, const char *album, char *out, size_t n)
{
	char a[300], b[300], q[700], enc[2200];
	int k;

	if (!phrase(artist, a, sizeof a) || !phrase(album, b, sizeof b)) return false;
	k = snprintf(q, sizeof q, "release:\"%s\" AND artist:\"%s\"", b, a);
	if (k < 0 || k >= (int)sizeof q) return false;
	net_urlencode(q, enc, sizeof enc);        /* at most three bytes a byte */
	k = snprintf(out, n, "https://musicbrainz.org/ws/2/release/?query=%s"
	             "&fmt=json&limit=25", enc);
	return k > 0 && k < (int)n;
}

static unsigned be16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }

bool museart_image_size(const char *path, int *w, int *h)
{
	unsigned char b[32];
	FILE *f = fopen(path, "rb");
	bool ok = false;

	if (!f) return false;
	if (fread(b, 1, 24, f) == 24) {
		if (!memcmp(b, "\x89PNG\r\n\x1a\n", 8) && !memcmp(b + 12, "IHDR", 4)) {
			*w = (int)((unsigned)b[16] << 24 | (unsigned)b[17] << 16 | be16(b + 18));
			*h = (int)((unsigned)b[20] << 24 | (unsigned)b[21] << 16 | be16(b + 22));
			ok = true;
		} else if (b[0] == 0xFF && b[1] == 0xD8) {
			/* Marker by marker to the frame header, skipping each segment
			 * by its length rather than reading it: an embedded cover can
			 * carry an EXIF thumbnail of its own ahead of the frame. */
			long at = 2;

			while (!ok && fseek(f, at, SEEK_SET) == 0 && fread(b, 1, 4, f) == 4) {
				unsigned m = b[1], len = be16(b + 2);

				if (b[0] != 0xFF) break;
				if (m == 0xFF) { at++; continue; }            /* padding */
				if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
					if (fread(b + 4, 1, 5, f) != 5) break;
					*h = (int)be16(b + 5);
					*w = (int)be16(b + 7);
					ok = true;
					break;
				}
				if (m == 0xD9 || m == 0xDA || len < 2) break;  /* no frame found */
				at += 2 + (long)len;
			}
		}
	}
	fclose(f);
	return ok && *w > 0 && *h > 0;
}

/* ---- the run ---------------------------------------------------------- */

enum { S_NEXT, S_SEARCH, S_COVER, S_DONE };

static museart_job   *g_jobs;
static int            g_n, g_i, g_state = S_DONE, g_retried, g_fails;
static bool           g_asked;
static unsigned       g_last_ms, g_until;
static char           g_reply[LIB_PATH], g_rg[64], g_cover[LIB_PATH * 2 + 8];
static museart_status g_st = { .landed = -1 };

static void finish(void)
{
	free(g_jobs);
	g_jobs = NULL;
	g_state = S_DONE;
}

bool museart_begin(const museart_job *jobs, int n, const char *reply)
{
	museart_cancel();
	memset(&g_st, 0, sizeof g_st);
	g_st.landed = -1;
	if (n <= 0) return false;
	g_jobs = malloc(sizeof *g_jobs * (size_t)n);
	if (!g_jobs) return false;
	memcpy(g_jobs, jobs, sizeof *g_jobs * (size_t)n);
	g_n = g_st.n = n;
	g_i = 0;
	g_retried = g_fails = 0;
	g_asked = false;
	g_until = 0;
	g_state = S_NEXT;
	snprintf(g_reply, sizeof g_reply, "%s", reply);
	return true;
}

/* On to the next album, counting this one as `outcome` said. Three failures in
 * a row - not a miss, a failure - means the network has gone, and a run that
 * carried on would spend a minute a album discovering it again. */
static void next(int *outcome)
{
	if (outcome) (*outcome)++;
	g_fails = outcome == &g_st.failed ? g_fails + 1 : 0;
	g_i++;
	g_st.done = g_i;
	g_retried = 0;
	g_state = S_NEXT;
	if (g_fails >= 3) {
		snprintf(g_st.problem, sizeof g_st.problem, "The network stopped answering");
		finish();
	}
}

static bool slurp(const char *path, char **out, size_t *len)
{
	FILE *f = fopen(path, "rb");
	long n;
	char *b;

	*out = NULL;
	if (!f) return false;
	if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0 || n > 4 * 1024 * 1024 ||
	    fseek(f, 0, SEEK_SET) != 0 || !(b = malloc((size_t)n + 1))) {
		fclose(f);
		return false;
	}
	*len = fread(b, 1, (size_t)n, f);
	b[*len] = '\0';
	fclose(f);
	*out = b;
	return true;
}

int museart_step(unsigned now)
{
	museart_job *j;
	char url[2400], dir[LIB_PATH * 2], *slash, *body;
	size_t len;
	int r, http;

	g_st.landed = -1;
	g_st.rg[0] = '\0';
	if (g_state == S_DONE) return 0;
	if (g_i >= g_n) { finish(); return 0; }
	j = &g_jobs[g_i];
	snprintf(g_st.now, sizeof g_st.now, "%s - %s", j->album, j->artist);

	switch (g_state) {
	case S_NEXT:
		if (g_asked && (int)(now - g_last_ms) < MB_GAP_MS) return 1;
		if (g_until && (int)(now - g_until) < 0) return 1;
		g_until = 0;
		if (!museart_search_url(j->artist, j->album, url, sizeof url) ||
		    !net_get_async(url, g_reply, 20)) {
			next(&g_st.failed);
			return g_state != S_DONE;
		}
		g_last_ms = now;
		g_asked = true;
		g_state = S_SEARCH;
		return 1;

	case S_SEARCH:
		if ((r = net_async_poll()) == 0) return 1;
		if (r < 0) {
			http = net_async_http();
			if (http == 503 && !g_retried) {
				g_retried = 1;
				g_until = now + MB_BUSY_MS;
				g_state = S_NEXT;
				return 1;
			}
			next(&g_st.failed);
			return g_state != S_DONE;
		}
		if (!slurp(g_reply, &body, &len)) { next(&g_st.failed); return g_state != S_DONE; }
		remove(g_reply);
		r = museart_pick(body, len, j->tracks, g_rg, sizeof g_rg);
		free(body);
		if (!r) { next(&g_st.missing); return 1; }

		/* The folder the cover goes in, which does not exist for an album
		 * whose files never carried one - there was nothing to extract. */
		snprintf(dir, sizeof dir, "%s", j->base);
		if ((slash = strrchr(dir, '/'))) { *slash = '\0'; mkdir(dir, 0755); }
		snprintf(g_cover, sizeof g_cover, "%s.jpg", j->base);
		snprintf(url, sizeof url,
		         "https://coverartarchive.org/release-group/%s/front-500", g_rg);
		if (!net_get_async(url, g_cover, 30)) { next(&g_st.failed); return g_state != S_DONE; }
		g_state = S_COVER;
		return 1;

	case S_COVER:
		if ((r = net_async_poll()) == 0) return 1;
		if (r < 0) {
			/* 404: MusicBrainz knows the record and nobody has given the
			 * archive its front cover - not found, not a failure. */
			next(net_async_http() == 404 ? &g_st.missing : &g_st.failed);
			return g_state != S_DONE;
		}
		/* The cover the files carried may have been a PNG. It is the old
		 * one now, and an album with two covers is one too many. */
		snprintf(dir, sizeof dir, "%s.png", j->base);
		remove(dir);
		g_st.landed = j->id;
		snprintf(g_st.rg, sizeof g_st.rg, "%s", g_rg);
		next(&g_st.found);
		return g_state != S_DONE;
	}
	return 1;
}

void museart_status_get(museart_status *st)
{
	*st = g_st;
}

void museart_cancel(void)
{
	if (g_state == S_SEARCH || g_state == S_COVER) net_async_abort();
	if (g_reply[0]) remove(g_reply);
	finish();
}
