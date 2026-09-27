/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* See xfer.h for why every path the browser sends comes through here. */
#include <stdio.h>
#include <string.h>

#include "xfer.h"

static xfer_root g_roots[5];
static int       g_nroots;

static void add_root(const char *name, const char *label, const char *fmt,
                     const char *base, bool no_cfg)
{
	xfer_root *r;

	if (g_nroots >= (int)(sizeof g_roots / sizeof g_roots[0])) return;
	r = &g_roots[g_nroots];
	r->no_cfg = no_cfg;
	snprintf(r->name,  sizeof r->name,  "%s", name);
	snprintf(r->label, sizeof r->label, "%s", label);
	if (snprintf(r->path, sizeof r->path, fmt, base) >= (int)sizeof r->path)
		return;                       /* a root that does not fit is no root */
	g_nroots++;
}

void xfer_init(const char *roms_dir, const char *card_dir,
               const char *shared_dir)
{
	g_nroots = 0;
	/* cores/ is deliberately not among these. A .so uploaded there
	 * is dlopen'd into the launcher's own address space on the next launch,
	 * which makes an upload form a way to run code as root and a way to break
	 * the device past the point where the launcher can fix it. The configs
	 * are out for the milder version of the same reason: a bad systems.cfg is
	 * a launcher that does not start, and the way to recover it is the card
	 * reader this feature exists to avoid needing. */
	add_root("roms",  "ROMs",  "%s", roms_dir, false);
	/* Muse reads nothing but this folder, and the card's top level is not a
	 * root, so without it music was the one thing on the card that still
	 * needed the card pulled. */
	add_root("music", "Music", "%s/Music", card_dir, false);
	add_root("bios",  "BIOS",  "%s/Bios",  card_dir, false);
	/* Two different things, both called saves in conversation.
	 *
	 * Saves/ is battery saves - the .srm a cartridge would have had - and is
	 * flat, one file per game. Contra has none, because Contra had no
	 * battery; looking there for it is how this gap was found.
	 *
	 * Save States/ is the launcher's own: 185 files on the test device, in
	 * per-system folders, and the ones whose only backup is a copy somebody
	 * made by hand over ADB. Leaving them unreachable made the feature miss
	 * the data most worth carrying off the device.
	 *
	 * That directory also holds cheevos.cfg and favorites.cfg, which the
	 * launcher writes and which are not save states. cheevos.cfg is the only
	 * record of an unlock the account has not seen yet, so it is not
	 * something a stray tap should be able to delete: no_cfg keeps every .cfg
	 * under this root out of listings and out of every path that resolves. */
	add_root("saves",  "Saves",       "%s/Saves", card_dir, false);
	add_root("states", "Save States", "%s/.tortos", shared_dir, true);
}

int xfer_root_count(void) { return g_nroots; }

const xfer_root *xfer_root_at(int i)
{
	return (i >= 0 && i < g_nroots) ? &g_roots[i] : NULL;
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* Percent-decode into `out`. False on a malformed escape, on a decoded NUL,
 * or on anything that will not fit.
 *
 * A malformed escape is refused rather than passed through as a literal '%'.
 * Passing it through is what most decoders do and it is how "%2%65" becomes
 * "%2e" becomes "." one layer later; refusing means there is exactly one
 * reading of any string that gets this far.
 *
 * Public, so a rename's destination goes through the same decoder as a path
 * rather than through a second one written to look like it. */
bool xfer_decode(const char *in, char *out, size_t outn)
{
	size_t o = 0;

	for (; *in; in++) {
		int c = (unsigned char)*in;

		if (c == '%') {
			int hi = hexval((unsigned char)in[1]);
			int lo = hi < 0 ? -1 : hexval((unsigned char)in[2]);

			if (lo < 0) return false;
			c = hi * 16 + lo;
			in += 2;
		}
		/* Control bytes never name a file anyone meant. NUL would truncate
		 * the string somewhere after this check and turn one path into
		 * another; the rest are refused with it rather than reasoned about
		 * one at a time. */
		if (c < 0x20 || c == 0x7f) return false;
		if (o + 1 >= outn) return false;
		out[o++] = (char)c;
	}
	out[o] = '\0';
	return true;
}

bool xfer_name_ok(const char *name)
{
	size_t n;

	if (!name || !*name) return false;
	if (!strcmp(name, ".") || !strcmp(name, "..")) return false;
	if (strchr(name, '/')) return false;
	n = strlen(name);
	if (n >= XFER_NAME_MAX) return false;
	/* Trailing dots and spaces are legal to ASK for and not legal to HAVE on
	 * vfat, which silently stores something else - so a rename to "x " would
	 * report success and produce a file the next listing cannot find by the
	 * name it was given. */
	if (name[n - 1] == ' ' || name[n - 1] == '.') return false;
	for (; *name; name++) {
		if ((unsigned char)*name < 0x20 || (unsigned char)*name == 0x7f)
			return false;
		/* The rest of what vfat will not store. The trailing-dot rule above
		 * was already here for this reason and stopped one character short of
		 * the actual set: a colon or a question mark reached open() and came
		 * back EINVAL, which the upload route reported as 507 "could not write
		 * that to the card" - a message that says the card is full about a
		 * file the card would never have taken under that name. Names like
		 * "Game: Subtitle.zip" are ordinary enough to arrive on the first
		 * real transfer. */
		if (strchr("\\:*?\"<>|", *name)) return false;
	}
	return true;
}

bool xfer_resolve(const char *url_path, char *out, size_t outn)
{
	char dec[XFER_PATH_MAX];
	const xfer_root *root = NULL;
	char *p, *seg;
	size_t used;
	int i;

	if (!url_path || !out || outn == 0) return false;
	if (!xfer_decode(url_path, dec, sizeof dec)) return false;

	/* An absolute path is not a request for a root-relative one that happens
	 * to start with a slash; it is a request for somewhere else. */
	if (dec[0] == '/') return false;

	p = dec;
	seg = p;
	while (*p && *p != '/') p++;
	if (*p == '/') *p++ = '\0';
	for (i = 0; i < g_nroots; i++)
		if (!strcmp(seg, g_roots[i].name)) { root = &g_roots[i]; break; }
	if (!root) return false;

	used = strlen(root->path);
	if (used >= outn) return false;
	memcpy(out, root->path, used + 1);

	/* Component by component, so "..", "." and "" are decided one at a time
	 * rather than by pattern-matching the whole string - which is the check
	 * that misses "a/../../b" because it only looked at the front. */
	while (*p) {
		size_t n;

		seg = p;
		while (*p && *p != '/') p++;
		if (*p == '/') *p++ = '\0';

		if (!*seg || !strcmp(seg, ".")) continue;   /* "a//b" and "a/./b" */
		if (!strcmp(seg, "..")) return false;
		n = strlen(seg);
		if (n >= XFER_NAME_MAX) return false;
		/* Refused, not merely hidden from listings: a name absent from a list
		 * is still a name somebody can type, and the delete route acts on
		 * whatever it is handed. Checked per COMPONENT, because by this point
		 * `dec` has been cut into pieces by the tokenizer above - testing the
		 * whole string here would have tested the root's name. */
		if (root->no_cfg && n >= 4 && !strcmp(seg + n - 4, ".cfg")) return false;
		if (used + 1 + n >= outn) return false;
		out[used++] = '/';
		memcpy(out + used, seg, n + 1);
		used += n;
	}
	return true;
}
