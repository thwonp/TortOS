/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* See chdread.h. The track and sector layout follows NextUI's
 * workspace/all/minarch/chd_reader.c (PolyForm Noncommercial, like this file),
 * with one change: a track's pregap is skipped only when the CHD stores it
 * (PGTYPE starting 'V'), as MAME's chdman writes it and RetroArch's
 * chdstream reads it. NextUI skips it always, which lands a track whose
 * pregap was not stored that many sectors late. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <libchdr/cdrom.h>
#include <libchdr/chd.h>

#include "rc_hash.h"

#include "chdread.h"

typedef struct {
	chd_file *chd;
	int       type;        /* CD_TRACK_* */
	uint32_t  first;       /* the track's first data frame in the CHD */
	uint32_t  frame;       /* bytes per frame (2448: sector + subcode) */
	uint32_t  per_hunk;    /* frames per hunk */
	uint32_t  cached;      /* hunk in buf, or UINT32_MAX */
	uint8_t  *buf;
} chd_track;

/* A handle from either reader: rhash passes it back to us blind. */
typedef struct {
	bool  chd;
	void *h;
} track;

static rc_hash_cdreader_t g_default;

static int track_type(const char *s)
{
	static const struct { const char *name; int type; } T[] = {
		{ "MODE1", CD_TRACK_MODE1 },           { "MODE1_RAW", CD_TRACK_MODE1_RAW },
		{ "MODE1/2352", CD_TRACK_MODE1_RAW },  { "MODE2", CD_TRACK_MODE2 },
		{ "MODE2_FORM1", CD_TRACK_MODE2_FORM1 }, { "MODE2_FORM2", CD_TRACK_MODE2_FORM2 },
		{ "MODE2_FORM_MIX", CD_TRACK_MODE2_FORM_MIX }, { "MODE2_RAW", CD_TRACK_MODE2_RAW },
		{ "MODE2/2352", CD_TRACK_MODE2_RAW },  { "AUDIO", CD_TRACK_AUDIO },
	};
	for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
		if (!strcmp(s, T[i].name)) return T[i].type;
	return CD_TRACK_MODE1;
}

/* One track's metadata, in any of the three tag formats chdman has written. */
static bool track_meta(chd_file *chd, int i, int *type, int *frames, int *pregap,
                       bool *pregap_stored)
{
	static const uint32_t TAGS[] = { CDROM_TRACK_METADATA2_TAG,
	                                 CDROM_TRACK_METADATA_TAG, GDROM_TRACK_METADATA_TAG };
	char meta[256], ty[32], sub[32], pgtype[32] = "";
	uint32_t len = 0;
	int n, no;

	for (n = 0; n < 3; n++)
		if (chd_get_metadata(chd, TAGS[n], (uint32_t)i, meta, sizeof meta - 1,
		                     &len, NULL, NULL) == CHDERR_NONE) break;
	if (n == 3) return false;
	meta[len < sizeof meta ? len : sizeof meta - 1] = '\0';
	*pregap = 0;
	if (sscanf(meta, "TRACK:%d TYPE:%31s SUBTYPE:%31s FRAMES:%d PREGAP:%d PGTYPE:%31s",
	           &no, ty, sub, frames, pregap, pgtype) < 4)
		return false;
	*type = track_type(ty);
	*pregap_stored = pgtype[0] == 'V';
	return true;
}

static void *chd_open_track(const char *path, uint32_t want)
{
	chd_file *chd = NULL;
	chd_track *t;
	const chd_header *hd;
	int type[CD_MAX_TRACKS], frames[CD_MAX_TRACKS], pregap[CD_MAX_TRACKS];
	bool stored[CD_MAX_TRACKS];
	int n = 0, pick = -1, i;
	uint32_t at = 0;

	if (chd_open(path, CHD_OPEN_READ, NULL, &chd) != CHDERR_NONE) return NULL;
	while (n < CD_MAX_TRACKS &&
	       track_meta(chd, n, &type[n], &frames[n], &pregap[n], &stored[n]))
		n++;

	if (want == RC_HASH_CDTRACK_FIRST_DATA) {
		for (i = 0; i < n && pick < 0; i++)
			if (type[i] != CD_TRACK_AUDIO) pick = i;
	} else if (want == RC_HASH_CDTRACK_LAST) {
		pick = n - 1;
	} else if (want == RC_HASH_CDTRACK_LARGEST) {
		for (i = 0; i < n; i++)
			if (pick < 0 || frames[i] > frames[pick]) pick = i;
	} else if (want >= 1 && want <= (uint32_t)n) {
		pick = (int)want - 1;
	}
	if (pick < 0) { chd_close(chd); return NULL; }

	/* Tracks follow each other, each padded to a multiple of 4 frames. A
	 * stored pregap counts in FRAMES, ahead of the track's own data. */
	for (i = 0; i < pick; i++) at += (uint32_t)((frames[i] + 3) & ~3);
	if (stored[pick]) at += (uint32_t)pregap[pick];

	hd = chd_get_header(chd);
	t = calloc(1, sizeof *t);
	if (!t || !hd->unitbytes || !(t->buf = malloc(hd->hunkbytes))) {
		free(t);
		chd_close(chd);
		return NULL;
	}
	t->chd      = chd;
	t->type     = type[pick];
	t->first    = at;
	t->frame    = hd->unitbytes;
	t->per_hunk = hd->hunkbytes / hd->unitbytes;
	t->cached   = UINT32_MAX;
	return t;
}

/* `sector` counts from the track's first data frame: first_track_sector says
 * 0, and rhash adds its offsets to that. */
static size_t chd_read_sector(chd_track *t, uint32_t sector, void *out, size_t want)
{
	uint32_t f = t->first + sector, hunk = f / t->per_hunk;
	const uint8_t *s;
	size_t skip = 0, size = 2048;

	if (hunk != t->cached) {
		if (chd_read(t->chd, hunk, t->buf) != CHDERR_NONE) return 0;
		t->cached = hunk;
	}
	s = t->buf + (f % t->per_hunk) * t->frame;

	if (t->type == CD_TRACK_AUDIO) {
		size = 2352;
	} else if (t->type == CD_TRACK_MODE1_RAW || t->type == CD_TRACK_MODE2_RAW ||
	           t->type == CD_TRACK_MODE2_FORM_MIX) {
		/* A raw sector says its own mode after the sync: 1 puts the data
		 * after a 16-byte header, 2 (Form 1) after 8 more of subheader. */
		skip = s[15] == 2 ? 24 : 16;
	} else if (t->type == CD_TRACK_MODE2) {
		skip = 8;
	}
	if (want > size) want = size;
	memcpy(out, s + skip, want);
	return want;
}

static void chd_close_track(chd_track *t)
{
	chd_close(t->chd);
	free(t->buf);
	free(t);
}

/* ------------------------------------------------- the reader rhash calls */

static bool is_chd(const char *path)
{
	const char *dot = strrchr(path, '.');
	return dot && !strcasecmp(dot, ".chd");
}

static void *open_track(const char *path, uint32_t want)
{
	track *t = calloc(1, sizeof *t);

	if (!t) return NULL;
	t->chd = is_chd(path);
	t->h = t->chd ? chd_open_track(path, want) : g_default.open_track(path, want);
	if (!t->h) { free(t); return NULL; }
	return t;
}

static void *open_track_iterator(const char *path, uint32_t want,
                                 const rc_hash_iterator_t *it)
{
	track *t;

	if (!is_chd(path) && g_default.open_track_iterator) {
		if (!(t = calloc(1, sizeof *t))) return NULL;
		t->h = g_default.open_track_iterator(path, want, it);
		if (!t->h) { free(t); return NULL; }
		return t;
	}
	return open_track(path, want);
}

static size_t read_sector(void *h, uint32_t sector, void *out, size_t n)
{
	track *t = h;
	return t->chd ? chd_read_sector(t->h, sector, out, n)
	              : g_default.read_sector(t->h, sector, out, n);
}

static void close_track(void *h)
{
	track *t = h;

	if (t->chd) chd_close_track(t->h);
	else        g_default.close_track(t->h);
	free(t);
}

static uint32_t first_track_sector(void *h)
{
	track *t = h;
	return t->chd ? 0 : g_default.first_track_sector(t->h);
}

void chd_cdreader(rc_hash_cdreader_t *r)
{
	rc_hash_get_default_cdreader(&g_default);
	memset(r, 0, sizeof *r);
	r->open_track          = open_track;
	r->open_track_iterator = open_track_iterator;
	r->read_sector         = read_sector;
	r->close_track         = close_track;
	r->first_track_sector  = first_track_sector;
}

/* ---------------------------------------------- the launcher's own reads */

/* A playlist's first disc: its first line that is not blank or a comment,
 * relative to the playlist. */
static bool m3u_first(const char *path, char *out, size_t n)
{
	char line[512];
	const char *slash = strrchr(path, '/');
	FILE *f = fopen(path, "r");
	bool ok = false;

	if (!f) return false;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (!line[0] || line[0] == '#') continue;
		if (line[0] == '/' || !slash)
			ok = snprintf(out, n, "%s", line) < (int)n;
		else
			ok = snprintf(out, n, "%.*s/%s", (int)(slash - path), path, line) < (int)n;
		break;
	}
	fclose(f);
	return ok;
}

bool cd_read_head(const char *path, void *out, size_t n)
{
	char disc[1024];
	const char *dot = strrchr(path, '.');
	rc_hash_iterator_t it;
	void *h;
	size_t got = 0;

	if (dot && !strcasecmp(dot, ".m3u")) {
		if (!m3u_first(path, disc, sizeof disc)) return false;
		path = disc;
	}
	/* Through an iterator, as rhash opens a track: rhash's own reader has no
	 * open_track, only open_track_iterator, which reads the file through the
	 * iterator's callbacks. */
	rc_hash_get_default_cdreader(&g_default);
	rc_hash_initialize_iterator(&it, path, NULL, 0);
	if ((h = open_track_iterator(path, 1, &it))) {
		got = read_sector(h, first_track_sector(h), out, n);
		close_track(h);
	}
	rc_hash_destroy_iterator(&it);
	return got == n;
}
