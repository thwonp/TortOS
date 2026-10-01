/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include "library.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* Disc-image extensions, in launch preference. A folder holding one of these
 * is a disc-based game -- a PC Engine CD title, typically -- and becomes one
 * entry launching the image inside it. .m3u wins so a multi-disc set is a
 * single entry with disc switching. */
static const char *DISC_EXTS[] = {
	"m3u", "chd", "cue", "ccd", "toc", "iso", "img", "pbp", "bin", NULL
};

static const char *ext_of(const char *name)
{
	const char *dot = strrchr(name, '.');
	return (dot && dot != name) ? dot + 1 : NULL;
}

/* Is ext one of the (comma or space separated) extensions in list? An empty
 * list means everything is allowed. */
static bool ext_allowed(const char *ext, const char *list)
{
	const char *p = list;
	size_t n;

	if (!list || !*list) return true;
	if (!ext) return false;
	n = strlen(ext);
	while (*p) {
		const char *start = p;
		while (*p && *p != ',' && *p != ' ') p++;
		if ((size_t)(p - start) == n && strncasecmp(start, ext, n) == 0) return true;
		while (*p == ',' || *p == ' ') p++;
	}
	return false;
}

/* Is this filename a disc image? The shelf needs to know because a disc may
 * need firmware a cartridge on the same shelf does not - see disc_bios in
 * config.h. Same table the disc-folder ranking uses, so the two cannot drift. */
bool lib_is_disc(const char *name)
{
	const char *ext = ext_of(name);
	int i;

	if (!ext) return false;
	for (i = 0; DISC_EXTS[i]; i++)
		if (strcasecmp(ext, DISC_EXTS[i]) == 0) return true;
	return false;
}

static int disc_rank(const char *name)
{
	const char *ext = ext_of(name);
	if (!ext) return -1;
	for (int i = 0; DISC_EXTS[i]; i++)
		if (strcasecmp(ext, DISC_EXTS[i]) == 0) return i;
	return -1;
}

/* Pick the file to launch inside a game folder: the best-ranked disc image,
 * ties broken alphabetically so a discs-only folder lands on Disc 1. */
static bool folder_launch_file(const char *dirpath, char *out, size_t outsz)
{
	DIR *d = opendir(dirpath);
	struct dirent *e;
	int best = 9999;
	char pick[LIB_NAME] = "";

	if (!d) return false;
	while ((e = readdir(d))) {
		int r;
		if (e->d_name[0] == '.') continue;
		r = disc_rank(e->d_name);
		if (r < 0) continue;
		if (r < best || (r == best && strcasecmp(e->d_name, pick) < 0)) {
			best = r;
			snprintf(pick, sizeof pick, "%s", e->d_name);
		}
	}
	closedir(d);
	if (!pick[0]) return false;
	snprintf(out, outsz, "%s", pick);
	return true;
}

static int game_cmp(const void *pa, const void *pb)
{
	return lib_order(pa, pb);
}

void lib_sort(game_list *l)
{
	if (l->count > 1) qsort(l->items, (size_t)l->count, sizeof *l->items, game_cmp);
}

/* The display title: `name` up to the first bracketed group that follows a
 * space. ROM sets carry their cataloging in the filename - "(USA)", "(Rev 1)",
 * "(En,Fr,De)", "[!]", "[T+Eng]" - and a shelf is not a catalog, so the card
 * shows the game and the file keeps the provenance.
 *
 * The space before the bracket is what makes this safe to do blindly: a name
 * that OPENS with a bracket, which is how BIOS and disc-set folders are often
 * marked, has nothing before it to cut and survives whole. Cutting to nothing
 * falls back to the full name for the same reason - an empty card is worse
 * than a noisy one.
 *
 * Box-art lookup deliberately keeps using `name`: two dumps of one game share
 * a title but not a filename, and the title would point both at the same
 * .media file. Sorting goes by the title with `name` breaking the tie
 * (lib_order), which keeps the two in a fixed order. */
void lib_title(const char *name, char *out, size_t n)
{
	const char *cut = NULL, *p;

	/* Searching from the second character, never the first: a name that OPENS
	 * with a bracket is how BIOS images and disc sets are usually marked, and
	 * it has nothing in front of the bracket to keep, so it survives whole
	 * instead of becoming nothing. */
	for (p = name; *p; p++) {
		if (p != name && (*p == '(' || *p == '[')) { cut = p; break; }
	}
	if (!cut) { snprintf(out, n, "%s", name); return; }

	/* Take any run of spaces before the bracket with it. */
	while (cut > name && (cut[-1] == ' ' || cut[-1] == '\t')) cut--;
	if (cut == name) { snprintf(out, n, "%s", name); return; }

	snprintf(out, n, "%.*s", (int)(cut - name), name);
}

void lib_free(game_list *l)
{
	free(l->items);
	l->items = NULL;
	l->count = 0;
	l->scanned = false;
}

/* readdir tells us the type on ext4; on the FAT32 card it usually does not,
 * and then a stat is the only way. Asking d_type first saves one syscall per
 * entry on the filesystems that answer. */
static bool is_dir(const char *full, const struct dirent *e)
{
#ifdef DT_DIR
	if (e->d_type == DT_DIR) return true;
	if (e->d_type != DT_UNKNOWN) return false;
#else
	(void)e;
#endif
	{
		struct stat st;
		return stat(full, &st) == 0 && S_ISDIR(st.st_mode);
	}
}

static bool is_file(const char *full, const struct dirent *e)
{
#ifdef DT_REG
	if (e->d_type == DT_REG) return true;
	if (e->d_type != DT_UNKNOWN) return false;
#else
	(void)e;
#endif
	{
		struct stat st;
		return stat(full, &st) == 0 && S_ISREG(st.st_mode);
	}
}

static void note_skip(game_list *out, const char *name)
{
	if (out->skipped < LIB_SKIPS_SHOWN)
		snprintf(out->skipped_eg[out->skipped], LIB_NAME, "%s", name);
	out->skipped++;
}

bool lib_scan(const char *roms_root, const char *folder, const char *exts,
              game_list *out)
{
	char dirpath[LIB_PATH * 2];
	DIR *d;
	struct dirent *e;
	int cap = 64, n = 0, files_n;
	game_entry *list;

	lib_free(out);
	out->scanned = true;
	out->skipped = 0;
	snprintf(dirpath, sizeof dirpath, "%s/%s", roms_root, folder);
	d = opendir(dirpath);
	if (!d) return false;
	list = malloc((size_t)cap * sizeof *list);
	if (!list) { closedir(d); return false; }

	/* Pass 1: a file at the top level is a game, launched directly. */
	while ((e = readdir(d))) {
		char full[LIB_PATH * 3];
		char *dot;
		if (e->d_name[0] == '.') continue;
		snprintf(full, sizeof full, "%s/%s", dirpath, e->d_name);
		if (!ext_allowed(ext_of(e->d_name), exts)) {
			/* A folder is the second pass's to judge. */
			if (is_file(full, e)) note_skip(out, e->d_name);
			continue;
		}
		if (!is_file(full, e)) continue;
		if (n == cap) {
			/* Grow cap only once the memory is actually there. Doubling it
			 * first and then bailing would leave the second pass believing
			 * there is room in a buffer that never grew. */
			game_entry *bigger = realloc(list, (size_t)cap * 2 * sizeof *list);
			if (!bigger) break;
			list = bigger;
			cap *= 2;
		}
		memset(&list[n], 0, sizeof list[n]);
		snprintf(list[n].file, sizeof list[n].file, "%s", e->d_name);
		snprintf(list[n].name, sizeof list[n].name, "%s", e->d_name);
		{   /* For the recently-added order. A file that cannot be statted
		     * keeps 0 and sorts to the bottom of that order, which is where
		     * "we do not know when this arrived" belongs. */
			struct stat st;
			if (stat(full, &st) == 0) list[n].added = (long)st.st_mtime;
		}
		dot = strrchr(list[n].name, '.');
		if (dot && dot != list[n].name) *dot = '\0';
		lib_title(list[n].name, list[n].title, sizeof list[n].title);
		n++;
	}

	/* Pass 2: a subfolder holding a disc image is a game too -- one entry
	 * named after the folder, launching the image inside it. Skip a folder
	 * shadowed by a same-named top-level file so the two layouts cannot
	 * double up. */
	files_n = n;
	rewinddir(d);
	while ((e = readdir(d))) {
		char full[LIB_PATH * 3], inside[LIB_NAME];
		bool shadowed = false;
		if (e->d_name[0] == '.') continue;
		snprintf(full, sizeof full, "%s/%s", dirpath, e->d_name);
		if (!is_dir(full, e)) continue;
		for (int k = 0; k < files_n; k++)
			if (strcasecmp(list[k].name, e->d_name) == 0) { shadowed = true; break; }
		if (shadowed) continue;
		if (!folder_launch_file(full, inside, sizeof inside)) {
			char shown[LIB_NAME];

			snprintf(shown, sizeof shown, "%.*s/", LIB_NAME - 2, e->d_name);
			note_skip(out, shown);
			continue;
		}
		if (n == cap) {
			/* Grow cap only once the memory is actually there. Doubling it
			 * first and then bailing would leave the second pass believing
			 * there is room in a buffer that never grew. */
			game_entry *bigger = realloc(list, (size_t)cap * 2 * sizeof *list);
			if (!bigger) break;
			list = bigger;
			cap *= 2;
		}
		memset(&list[n], 0, sizeof list[n]);
		snprintf(list[n].file, sizeof list[n].file, "%s/%s", e->d_name, inside);
		snprintf(list[n].name, sizeof list[n].name, "%s", e->d_name);
		{   /* The FOLDER's mtime, not the disc image's. The folder is the
		     * game here, and copying a disc into it touches the folder. */
			struct stat st;
			char dirfull[LIB_PATH * 3];

			snprintf(dirfull, sizeof dirfull, "%s/%s", dirpath, e->d_name);
			if (stat(dirfull, &st) == 0) list[n].added = (long)st.st_mtime;
		}
		lib_title(list[n].name, list[n].title, sizeof list[n].title);
		n++;
	}
	closedir(d);

	if (n == 0) { free(list); return false; }
	qsort(list, (size_t)n, sizeof *list, game_cmp);
	out->items = list;
	out->count = n;
	return true;
}
