/* SPDX-License-Identifier: MIT */
/* Does Muse read the Music folder the way its header says?
 *
 *     make check-muselib
 *
 * A folder is built under /tmp in both shapes muselib understands, with the
 * litter a Mac leaves on a card copied from it, and read back. Then the
 * questions the screens ask of it: which album is this track in, where is that
 * album's cover kept, and which album is each card on the shelf in either of
 * its orders.
 *
 * No SDL, no daemon, no device.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/muselib.h"

static int failures;

#define CHECK(cond, ...)                                                      \
	do {                                                                      \
		if (!(cond)) {                                                        \
			printf("  FAIL: ");                                               \
			printf(__VA_ARGS__);                                              \
			printf("\n        at %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
			failures++;                                                       \
		}                                                                     \
	} while (0)

static char root[256];

static void touch(const char *rel)
{
	char p[512];
	FILE *f;

	snprintf(p, sizeof p, "%s/%s", root, rel);
	f = fopen(p, "w");
	if (f) fclose(f);
}

static void dir(const char *rel)
{
	char p[512];

	snprintf(p, sizeof p, "%s/%s", root, rel);
	mkdir(p, 0755);
}

static int album_named(const ml_lib *l, const char *name)
{
	int i;

	for (i = 0; i < l->nalbums; i++)
		if (!strcmp(l->albums[i].name, name)) return i;
	return -1;
}

static void names(void)
{
	char out[128];

	printf("track names\n");
	ml_track_name("03 High And Dry.mp3", out, sizeof out);
	CHECK(!strcmp(out, "High And Dry"), "number and space: got \"%s\"", out);
	ml_track_name("01 - Orphan.flac", out, sizeof out);
	CHECK(!strcmp(out, "Orphan"), "number and dash: got \"%s\"", out);
	ml_track_name("1979.mp3", out, sizeof out);
	CHECK(!strcmp(out, "1979"), "a track CALLED a number keeps it: got \"%s\"", out);
	ml_track_name("Intro.opus", out, sizeof out);
	CHECK(!strcmp(out, "Intro"), "no number: got \"%s\"", out);
}

static void scan(void)
{
	ml_lib l;
	char base[512], want[512];
	int bends, bends2, pod, i;

	printf("the folder\n");
	dir("Radiohead");
	dir("Radiohead/The Bends");
	dir("Radiohead/The Bends 2");
	dir("Radiohead/.media");
	dir("Podcast");
	dir("Empty");
	touch("Radiohead/The Bends/02 Bones.mp3");
	touch("Radiohead/The Bends/01 Planet Telex.mp3");
	touch("Radiohead/The Bends/._01 Planet Telex.mp3");   /* AppleDouble */
	touch("Radiohead/The Bends/notes.txt");
	touch("Radiohead/The Bends 2/01 Bonus.mp3");
	touch("Radiohead/.media/The Bends.jpg");               /* a cover, not an album */
	touch("Podcast/ep1.mp3");
	touch("loose.mp3");                                   /* under the root: no album */
	dir("Singles");                                       /* where the launcher moves it */
	touch("Singles/A Single.mp3");

	CHECK(ml_scan(root, &l), "a folder that is there scans");
	CHECK(l.nartists == 3, "two artists with music and Singles, not the empty one: %d",
	      l.nartists);
	CHECK(l.nalbums == 4, "three albums and Singles, and .media is not one: %d", l.nalbums);
	CHECK(l.ntracks == 5, "five tracks, no AppleDouble, no text, nothing loose: %d",
	      l.ntracks);

	printf("Singles\n");
	{
		int s = album_named(&l, ML_SINGLES);

		CHECK(s >= 0 && l.albums[s].singles && !l.albums[s].book,
		      "Music/Singles is an album marked as Singles");
		CHECK(album_named(&l, "The Bends") >= 0 &&
		      !l.albums[album_named(&l, "The Bends")].singles,
		      "and no other album is");
	}

	bends  = album_named(&l, "The Bends");
	bends2 = album_named(&l, "The Bends 2");
	pod    = album_named(&l, "Podcast");
	CHECK(bends >= 0 && bends2 >= 0 && pod >= 0, "the albums are named by folder");
	if (bends < 0 || bends2 < 0 || pod < 0) { ml_free(&l); return; }
	CHECK(l.albums[bends].n == 2, "The Bends has two tracks: %d", l.albums[bends].n);
	CHECK(!strcmp(l.tracks[l.albums[bends].first].name, "Planet Telex"),
	      "sorted by file name, shown without the number: \"%s\"",
	      l.tracks[l.albums[bends].first].name);

	printf("which album a track is in\n");
	CHECK(ml_album_of(&l, "Radiohead/The Bends/02 Bones.mp3") == bends,
	      "a track in The Bends");
	CHECK(ml_album_of(&l, "Radiohead/The Bends 2/01 Bonus.mp3") == bends2,
	      "a folder whose name starts with another's is its own album");
	CHECK(ml_album_of(&l, "Podcast/ep1.mp3") == pod, "the flat shape");
	CHECK(ml_album_of(&l, "Radiohead/Pablo Honey/01 You.mp3") == -1,
	      "a folder the scan never saw");
	CHECK(ml_album_of(&l, "") == -1, "nothing playing");

	printf("where covers are kept\n");
	ml_cover_base(root, &l, bends, base, sizeof base);
	snprintf(want, sizeof want, "%s/Radiohead/.media/The Bends", root);
	CHECK(!strcmp(base, want), "beside the album: got %s", base);
	ml_cover_base(root, &l, pod, base, sizeof base);
	snprintf(want, sizeof want, "%s/.media/Podcast", root);
	CHECK(!strcmp(base, want), "under the root for the flat shape: got %s", base);
	ml_cover_base(root, &l, l.nalbums, base, sizeof base);
	CHECK(base[0] == '\0', "no album, no path");

	for (i = 0; i < l.ntracks; i++)
		CHECK(l.tracks[i].path[0] != '/', "paths are relative: %s", l.tracks[i].path);
	ml_free(&l);
}

/* The shelf's two orders, on a folder where they differ: two artists with a
 * "Greatest Hits" each, and an album named in lower case, which has to sort
 * where its letters say and not after every capital. */
static void orders(void)
{
	static const char *const want[] = {
		"amnesiac", "Greatest Hits", "Greatest Hits", "Parklife", "The Bends"
	};
	char sub[300];
	ml_lib l;
	int out[8], k;

	printf("the shelf's orders\n");
	CHECK(ml_order_index("album") == ML_BY_ALBUM, "album is stored as \"album\"");
	CHECK(ml_order_index("artist") == ML_BY_ARTIST, "artist as \"artist\"");
	CHECK(ml_order_index("name") == ML_BY_ARTIST && ml_order_index(NULL) == ML_BY_ARTIST,
	      "anything else is the folder's own order");
	CHECK(!strcmp(ml_order_label(ML_BY_ALBUM, false), "Album") &&
	      !strcmp(ml_order_label(ML_BY_ARTIST, false), "Artist"), "what the menu says");
	CHECK(!strcmp(ml_order_label(ML_BY_ALBUM, true), "Title") &&
	      !strcmp(ml_order_label(ML_BY_ARTIST, true), "Author"),
	      "and what it says on the books");
	CHECK(ml_order_index("year") == ML_BY_YEAR &&
	      !strcmp(ml_order_label(ML_BY_YEAR, false), "Year") &&
	      !strcmp(ml_order_label(ML_BY_YEAR, true), "Year"), "year, on music and books");
	CHECK(!strcmp(ml_order_name((ml_order)ML_ORDERS), "artist"),
	      "a number out of range reads as the first order, not past the table");

	dir("o");
	dir("o/Blur");
	dir("o/Blur/Parklife");
	dir("o/Blur/Greatest Hits");
	dir("o/Queen");
	dir("o/Queen/Greatest Hits");
	dir("o/Radiohead");
	dir("o/Radiohead/The Bends");
	dir("o/Radiohead/amnesiac");
	touch("o/Blur/Parklife/01 Girls And Boys.mp3");
	touch("o/Blur/Greatest Hits/01 Beetlebum.mp3");
	touch("o/Queen/Greatest Hits/01 Bohemian Rhapsody.mp3");
	touch("o/Radiohead/The Bends/01 Planet Telex.mp3");
	touch("o/Radiohead/amnesiac/01 Packt.mp3");
	snprintf(sub, sizeof sub, "%s/o", root);
	CHECK(ml_scan(sub, &l) && l.nalbums == 5, "five albums: %d", l.nalbums);
	if (l.nalbums != 5) { ml_free(&l); return; }

	CHECK(ml_shelf_order(&l, ML_BY_ARTIST, false, NULL, out) == 5, "every album is music");
	for (k = 0; k < 5; k++)
		CHECK(out[k] == k, "by artist is the scan's order: card %d is album %d", k, out[k]);

	ml_shelf_order(&l, ML_BY_ALBUM, false, NULL, out);
	for (k = 0; k < 5; k++)
		CHECK(!strcmp(l.albums[out[k]].name, want[k]),
		      "by album, card %d: want %s, got %s", k, want[k], l.albums[out[k]].name);
	CHECK(!strncmp(l.tracks[l.albums[out[1]].first].path, "Blur/", 5) &&
	      !strncmp(l.tracks[l.albums[out[2]].first].path, "Queen/", 6),
	      "two of one title go in their artists' order");
	CHECK(ml_shelf_order(&l, ML_BY_ALBUM, true, NULL, out) == 0, "and none is a book");

	/* Scan order: Blur/Greatest Hits, Blur/Parklife, Queen/Greatest Hits,
	 * Radiohead/amnesiac, Radiohead/The Bends - case does not count
	 * (plorpos-xav). */
	{
		int years[5] = { 1997, 1994, 0, 2001, 1995 };
		const int want_y[5] = { 3, 0, 4, 1, 2 };

		for (k = 0; k < 5; k++)
			CHECK(!strcmp(l.albums[k].name, (const char *[]){ "Greatest Hits",
			      "Parklife", "Greatest Hits", "amnesiac", "The Bends" }[k]),
			      "scan order as assumed, album %d", k);
		ml_shelf_order(&l, ML_BY_YEAR, false, years, out);
		for (k = 0; k < 5; k++)
			CHECK(out[k] == want_y[k], "by year newest first, none last: card %d is %d, want %d",
			      k, out[k], want_y[k]);
		years[4] = 1997;
		ml_shelf_order(&l, ML_BY_YEAR, false, years, out);
		CHECK(out[1] == 0 && out[2] == 4, "one year keeps artist order");
		years[1] = -1;
		ml_shelf_order(&l, ML_BY_YEAR, false, years, out);
		CHECK(out[3] == 1 && out[4] == 2, "not known yet goes with none, in artist order");
		ml_shelf_order(&l, ML_BY_YEAR, false, NULL, out);
		for (k = 0; k < 5; k++)
			CHECK(out[k] == k, "no years at all is artist order: card %d is %d", k, out[k]);
	}
	ml_free(&l);
}

/* A card's two folders read as one library, the books marked: what Muse's
 * shelf filters on, and what the launcher resumes. Then the pictures a folder
 * carries, which is where a book's cover usually is. */
static void card(void)
{
	char c[300], img[600], want[600];
	ml_lib l;
	int out[8], dcc, hs, n;

	printf("a card's Music and Audiobooks\n");
	dir("card");
	dir("card/Music");
	dir("card/Music/Blur");
	dir("card/Music/Blur/Parklife");
	touch("card/Music/Blur/Parklife/01 Girls And Boys.mp3");
	touch("card/Music/Blur/Parklife/cover.JPG");
	dir("card/Audiobooks");
	dir("card/Audiobooks/Dungeon Crawler Carl 3");
	touch("card/Audiobooks/Dungeon Crawler Carl 3/Book 3.m4b");
	touch("card/Audiobooks/Dungeon Crawler Carl 3/Book 3.jpg");
	touch("card/Audiobooks/Dungeon Crawler Carl 3/Book 3.cue");
	dir("card/Audiobooks/Banks");
	dir("card/Audiobooks/Banks/The Hydrogen Sonata");
	touch("card/Audiobooks/Banks/The Hydrogen Sonata/disc 02.mp3");
	touch("card/Audiobooks/Banks/The Hydrogen Sonata/disc 01.mp3");
	touch("card/Audiobooks/Banks/The Hydrogen Sonata/back.jpg");
	touch("card/Audiobooks/Banks/The Hydrogen Sonata/front.jpg");
	snprintf(c, sizeof c, "%s/card", root);

	CHECK(ml_scan_card(c, &l), "a card with both folders scans");
	CHECK(l.nalbums == 3 && ml_count(&l, false) == 1 && ml_count(&l, true) == 2,
	      "one album and two books: %d, %d, %d", l.nalbums, ml_count(&l, false),
	      ml_count(&l, true));
	dcc = album_named(&l, "Dungeon Crawler Carl 3");
	hs  = album_named(&l, "The Hydrogen Sonata");
	CHECK(dcc >= 0 && hs >= 0, "books named by folder, in both shapes");
	if (dcc < 0 || hs < 0) { ml_free(&l); return; }
	CHECK(l.albums[dcc].book && l.albums[hs].book && !l.albums[album_named(&l, "Parklife")].book,
	      "the books are marked, the album is not");
	CHECK(!strcmp(l.tracks[l.albums[hs].first].path,
	              "Audiobooks/Banks/The Hydrogen Sonata/disc 01.mp3"),
	      "paths are relative to the card: %s", l.tracks[l.albums[hs].first].path);
	CHECK(ml_album_of(&l, "Audiobooks/Dungeon Crawler Carl 3/Book 3.m4b") == dcc,
	      "which book a file is in");
	n = ml_shelf_order(&l, ML_BY_ARTIST, true, NULL, out);
	CHECK(n == 2 && out[0] == hs && out[1] == dcc,
	      "the book shelf is the books alone, by author: %d", n);
	CHECK(ml_shelf_order(&l, ML_BY_ARTIST, false, NULL, out) == 1, "the music shelf the album alone");

	ml_cover_base(c, &l, dcc, img, sizeof img);
	snprintf(want, sizeof want, "%s/Audiobooks/.media/Dungeon Crawler Carl 3", c);
	CHECK(!strcmp(img, want), "a book's cover is kept like an album's: %s", img);

	printf("pictures in a folder\n");
	snprintf(want, sizeof want, "%s/Audiobooks/Dungeon Crawler Carl 3/Book 3.jpg", c);
	CHECK(ml_folder_image(c, &l, dcc, img, sizeof img) && !strcmp(img, want),
	      "the only picture, whatever it is called: %s", img);
	snprintf(want, sizeof want, "%s/Music/Blur/Parklife/cover.JPG", c);
	CHECK(ml_folder_image(c, &l, album_named(&l, "Parklife"), img, sizeof img) &&
	      !strcmp(img, want), "cover, in any case: %s", img);
	CHECK(!ml_folder_image(c, &l, hs, img, sizeof img),
	      "two pictures and neither named cover: no guess");
	ml_free(&l);

	snprintf(c, sizeof c, "%s/nothing", root);
	CHECK(!ml_scan_card(c, &l), "a card with neither folder does not scan");
	ml_free(&l);
}

int main(void)
{
	char cmd[300];

	snprintf(root, sizeof root, "/tmp/muselib-check.XXXXXX");
	if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }

	printf("muselib: the Music and Audiobooks folders\n");
	names();
	scan();
	orders();
	card();

	snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
	if (system(cmd) != 0) { }

	if (failures) {
		printf("\n%d failure%s\n", failures, failures == 1 ? "" : "s");
		return 1;
	}
	printf("ok\n");
	return 0;
}
