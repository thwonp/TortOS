/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* See muselib.h. */
#include "muselib.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* What FFmpeg on the device was measured playing (BACKLOG, the Muse spike),
 * plus the containers those codecs arrive in. */
static const char *AUDIO[] = {
	"mp3", "m4a", "m4b", "aac", "flac", "ogg", "oga", "opus", "wav", NULL
};

bool ml_is_audio(const char *name)
{
	const char *dot = strrchr(name, '.');
	int i;

	if (!dot || name[0] == '.') return false;
	for (i = 0; AUDIO[i]; i++)
		if (!strcasecmp(dot + 1, AUDIO[i])) return true;
	return false;
}

void ml_track_name(const char *file, char *out, int n)
{
	const char *p = file, *dot = strrchr(file, '.');
	int len;

	/* "03 ", "03 - ", "03. ", "3-" - a number and its punctuation, but only
	 * when a name follows: a track CALLED "1979" keeps its name. */
	while (isdigit((unsigned char)*p)) p++;
	if (p > file && p - file <= 3) {
		const char *q = p;

		while (*q == ' ' || *q == '-' || *q == '.' || *q == '_') q++;
		if (q > p && *q && q != dot) p = q; else p = file;
	} else {
		p = file;
	}
	len = dot && dot > p ? (int)(dot - p) : (int)strlen(p);
	if (len >= n) len = n - 1;
	memcpy(out, p, (size_t)len);
	out[len] = '\0';
}

/* How much of a path is its folder: "A/B/c.mp3" is 3, "c.mp3" is 0. */
static size_t dir_len(const char *path)
{
	const char *s = strrchr(path, '/');

	return s ? (size_t)(s - path) : 0;
}

int ml_album_of(const ml_lib *l, const char *track)
{
	size_t n;
	int i;

	if (!track || !track[0]) return -1;
	n = dir_len(track);
	for (i = 0; i < l->nalbums; i++) {
		const char *p;

		if (l->albums[i].n <= 0) continue;
		p = l->tracks[l->albums[i].first].path;
		/* The same length first: "Pixies/Doolittle" is a prefix of
		 * "Pixies/Doolittle (Deluxe)", and they are two albums. */
		if (dir_len(p) == n && !strncmp(p, track, n)) return i;
	}
	return -1;
}

void ml_cover_base(const char *root, const ml_lib *l, int al, char *out, size_t n)
{
	const char *p;
	size_t d, cut;

	out[0] = '\0';
	if (al < 0 || al >= l->nalbums || l->albums[al].n <= 0) return;
	p = l->tracks[l->albums[al].first].path;
	d = dir_len(p);                         /* "Radiohead/The Bends" */
	for (cut = d; cut > 0 && p[cut - 1] != '/'; cut--) { }
	/* p[0..cut) is the parent with its slash, p[cut..d) the album's name. */
	snprintf(out, n, "%s/%.*s.media/%.*s", root, (int)cut, p,
	         (int)(d - cut), p + cut);
}

/* ---- a small growable list of names, sorted ------------------------------- */

typedef struct { char **v; int n, cap; } names;

static void names_add(names *l, const char *s)
{
	if (l->n == l->cap) {
		int cap = l->cap ? l->cap * 2 : 32;
		char **v = realloc(l->v, sizeof *v * (size_t)cap);

		if (!v) return;
		l->v = v;
		l->cap = cap;
	}
	l->v[l->n] = strdup(s);
	if (l->v[l->n]) l->n++;
}

static int cmp_name(const void *a, const void *b)
{
	return strcasecmp(*(char *const *)a, *(char *const *)b);
}

static void names_free(names *l)
{
	int i;

	for (i = 0; i < l->n; i++) free(l->v[i]);
	free(l->v);
	memset(l, 0, sizeof *l);
}

/* The subfolders and the audio files in `dir`, each sorted. Dot entries are
 * skipped: macOS leaves AppleDouble files beside every copied track, and they
 * carry the track's own extension. */
static void list_dir(const char *dir, names *dirs, names *files)
{
	DIR *d = opendir(dir);
	struct dirent *e;

	if (!d) return;
	while ((e = readdir(d))) {
		char full[LIB_PATH * 2];
		struct stat st;

		if (e->d_name[0] == '.') continue;
		if (snprintf(full, sizeof full, "%s/%s", dir, e->d_name) >= (int)sizeof full)
			continue;
		if (stat(full, &st) != 0) continue;
		if (S_ISDIR(st.st_mode)) { if (dirs) names_add(dirs, e->d_name); }
		else if (files && ml_is_audio(e->d_name)) names_add(files, e->d_name);
	}
	closedir(d);
	if (dirs)  qsort(dirs->v, (size_t)dirs->n, sizeof *dirs->v, cmp_name);
	if (files) qsort(files->v, (size_t)files->n, sizeof *files->v, cmp_name);
}

/* ---- the library ---------------------------------------------------------- */

static bool grow(void **v, int n, int *cap, size_t sz)
{
	void *p;
	int c;

	if (n < *cap) return true;
	c = *cap ? *cap * 2 : 64;
	p = realloc(*v, sz * (size_t)c);
	if (!p) return false;
	*v = p;
	*cap = c;
	return true;
}

/* One album: `rel` is its folder relative to the root. */
static void add_album(ml_lib *l, int *acap, int *tcap, const char *root,
                      const char *rel, const char *name, const names *files)
{
	ml_album *al;
	int i;

	if (files->n == 0) return;
	if (!grow((void **)&l->albums, l->nalbums, acap, sizeof *l->albums)) return;
	al = &l->albums[l->nalbums++];
	snprintf(al->name, sizeof al->name, "%s", name);
	al->first = l->ntracks;
	al->n = 0;
	for (i = 0; i < files->n; i++) {
		ml_track *t;

		if (!grow((void **)&l->tracks, l->ntracks, tcap, sizeof *l->tracks)) break;
		t = &l->tracks[l->ntracks];
		if (snprintf(t->path, sizeof t->path, "%s/%s", rel, files->v[i])
		    >= (int)sizeof t->path)
			continue;
		ml_track_name(files->v[i], t->name, sizeof t->name);
		l->ntracks++;
		al->n++;
	}
	(void)root;
}

bool ml_scan(const char *root, ml_lib *out)
{
	names top = { 0 };
	int acap = 0, tcap = 0, rcap = 0, i;
	struct stat st;

	memset(out, 0, sizeof *out);
	if (stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) return false;

	list_dir(root, &top, NULL);
	for (i = 0; i < top.n; i++) {
		char dir[LIB_PATH * 2];
		names sub = { 0 }, files = { 0 };
		ml_artist *ar;
		int j, before = out->nalbums;

		snprintf(dir, sizeof dir, "%s/%s", root, top.v[i]);
		list_dir(dir, &sub, &files);

		/* Music/<name>/<tracks>: its own artist, one album of the same name. */
		add_album(out, &acap, &tcap, root, top.v[i], top.v[i], &files);
		for (j = 0; j < sub.n; j++) {
			char adir[LIB_PATH * 2], rel[LIB_PATH];
			names tracks = { 0 };

			snprintf(adir, sizeof adir, "%s/%s", dir, sub.v[j]);
			snprintf(rel, sizeof rel, "%s/%s", top.v[i], sub.v[j]);
			list_dir(adir, NULL, &tracks);
			add_album(out, &acap, &tcap, root, rel, sub.v[j], &tracks);
			names_free(&tracks);
		}
		names_free(&sub);
		names_free(&files);

		if (out->nalbums == before) continue;          /* nothing to play */
		if (!grow((void **)&out->artists, out->nartists, &rcap, sizeof *out->artists))
			break;
		ar = &out->artists[out->nartists++];
		snprintf(ar->name, sizeof ar->name, "%s", top.v[i]);
		ar->first = before;
		ar->n = out->nalbums - before;
	}
	names_free(&top);
	return true;
}

void ml_free(ml_lib *lib)
{
	free(lib->artists);
	free(lib->albums);
	free(lib->tracks);
	memset(lib, 0, sizeof *lib);
}

/* ---- the shelf's orders ----------------------------------------------------- */

static const struct { const char *name, *label; } ORDERS[ML_ORDERS] = {
	[ML_BY_ARTIST] = { "artist", "Artist" },
	[ML_BY_ALBUM]  = { "album",  "Album"  },
};

const char *ml_order_name(ml_order o)
{
	return ORDERS[(unsigned)o < ML_ORDERS ? o : ML_BY_ARTIST].name;
}

const char *ml_order_label(ml_order o)
{
	return ORDERS[(unsigned)o < ML_ORDERS ? o : ML_BY_ARTIST].label;
}

ml_order ml_order_index(const char *name)
{
	int i;

	for (i = 0; name && i < ML_ORDERS; i++)
		if (!strcmp(ORDERS[i].name, name)) return (ml_order)i;
	return ML_BY_ARTIST;
}

/* An album as the album order compares it: its title, pointed at rather than
 * copied, and its number in the scan. The number is the tie-break, and it is
 * the artist one - the scan reads artists in order, so of two albums with the
 * same title the one whose artist comes first has the lower number. It is also
 * what keeps the order a function of the folder rather than of qsort. */
typedef struct { const char *album; int al; } shelf_key;

static int by_album(const void *a, const void *b)
{
	const shelf_key *x = a, *y = b;
	int c = strcasecmp(x->album, y->album);

	return c ? c : x->al - y->al;
}

void ml_shelf_order(const ml_lib *l, ml_order by, int *out)
{
	shelf_key *k;
	int i;

	for (i = 0; i < l->nalbums; i++) out[i] = i;      /* the scan's, by artist */
	if (by != ML_BY_ALBUM || l->nalbums < 2) return;
	k = malloc(sizeof *k * (size_t)l->nalbums);
	if (!k) return;
	for (i = 0; i < l->nalbums; i++) {
		k[i].album = l->albums[i].name;
		k[i].al    = i;
	}
	qsort(k, (size_t)l->nalbums, sizeof *k, by_album);
	for (i = 0; i < l->nalbums; i++) out[i] = k[i].al;
	free(k);
}
