/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* Can the browser get out of the roots?
 *
 *     make check-xfer
 *
 * Hare puts a file manager on the LAN. Everything else in it is a
 * convenience; this is the part that decides whether a stranger on the same
 * Wi-Fi can write to /mnt/SDCARD/TortOS/tortos.elf. So the escapes get
 * written down as cases, including the ones that only work against a
 * plausible-looking implementation:
 *
 *   - "..", in front, in the middle, at the end
 *   - "..", percent-encoded, which beats a check that runs before decoding
 *   - a decoder that passes malformed escapes through as literals, which
 *     turns "%2%65" into "." for whoever decodes next
 *   - "a/../../b", which beats a check that only looks at the first component
 *   - an embedded NUL, which truncates the path after it has been approved
 *
 * No device, no network, no filesystem. Every answer is a string.
 */
#include <stdio.h>
#include <string.h>

#include "../src/xfer.h"

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

/* Refused. The message says what would have happened if it were not. */
static void deny(const char *req, const char *why)
{
	char out[XFER_PATH_MAX];

	if (xfer_resolve(req, out, sizeof out)) {
		printf("  FAIL: \"%s\" was allowed, and resolved to %s\n", req, out);
		printf("        %s\n", why);
		failures++;
	}
}

/* Allowed, and lands exactly where it should. */
static void allow(const char *req, const char *want)
{
	char out[XFER_PATH_MAX];

	if (!xfer_resolve(req, out, sizeof out)) {
		printf("  FAIL: \"%s\" was refused and should not have been\n", req);
		failures++;
		return;
	}
	if (strcmp(out, want)) {
		printf("  FAIL: \"%s\"\n        landed on %s\n        wanted   %s\n",
		       req, out, want);
		failures++;
	}
}

int main(void)
{
	char out[XFER_PATH_MAX];

	xfer_init("/mnt/SDCARD/Roms", "/mnt/SDCARD",
	          "/mnt/SDCARD/.userdata/shared");

	printf("xfer: what the browser can reach\n");

	printf("  the roots:\n");
	CHECK(xfer_root_count() == 6, "expected 6 roots, got %d", xfer_root_count());
	CHECK(xfer_root_at(0) && !strcmp(xfer_root_at(0)->path, "/mnt/SDCARD/Roms"),
	      "roms root wrong: %s", xfer_root_at(0) ? xfer_root_at(0)->path : "(none)");
	CHECK(xfer_root_at(1) && !strcmp(xfer_root_at(1)->path, "/mnt/SDCARD/Music"),
	      "music root wrong: %s", xfer_root_at(1) ? xfer_root_at(1)->path : "(none)");
	CHECK(xfer_root_at(2) && !strcmp(xfer_root_at(2)->path, "/mnt/SDCARD/Audiobooks"),
	      "books root wrong: %s", xfer_root_at(2) ? xfer_root_at(2)->path : "(none)");
	CHECK(xfer_root_at(3) && !strcmp(xfer_root_at(3)->path, "/mnt/SDCARD/Bios"),
	      "bios root wrong: %s", xfer_root_at(3) ? xfer_root_at(3)->path : "(none)");
	CHECK(xfer_root_at(4) && !strcmp(xfer_root_at(4)->path, "/mnt/SDCARD/Saves"),
	      "saves root wrong: %s", xfer_root_at(4) ? xfer_root_at(4)->path : "(none)");
	CHECK(xfer_root_at(5) &&
	      !strcmp(xfer_root_at(5)->path, "/mnt/SDCARD/.userdata/shared/.tortos"),
	      "states root wrong: %s", xfer_root_at(5) ? xfer_root_at(5)->path : "(none)");
	CHECK(xfer_root_at(6) == NULL, "a seventh root appeared");

	printf("  ordinary paths land where they should:\n");
	allow("roms", "/mnt/SDCARD/Roms");
	allow("roms/NES", "/mnt/SDCARD/Roms/NES");
	allow("roms/NES/Contra%20(USA).zip", "/mnt/SDCARD/Roms/NES/Contra (USA).zip");
	allow("music/Radiohead/The%20Bends/01%20Planet%20Telex.mp3",
	      "/mnt/SDCARD/Music/Radiohead/The Bends/01 Planet Telex.mp3");
	allow("books/Some%20Book/01.m4b", "/mnt/SDCARD/Audiobooks/Some Book/01.m4b");
	allow("bios", "/mnt/SDCARD/Bios");
	allow("saves/NES/Contra.srm", "/mnt/SDCARD/Saves/NES/Contra.srm");
	/* Box art lives in a dot directory, so dot-leading names are ordinary
	 * here and only the two dot ENTRIES are special. */
	allow("roms/NES/.media/Contra.png", "/mnt/SDCARD/Roms/NES/.media/Contra.png");
	allow("roms/NES/.cheevos", "/mnt/SDCARD/Roms/NES/.cheevos");
	/* Tidied rather than refused: a browser that joins paths sloppily is not
	 * an attacker, and "a//b" means what it looks like it means. */
	allow("roms//NES///Contra.zip", "/mnt/SDCARD/Roms/NES/Contra.zip");
	allow("roms/./NES/./Contra.zip", "/mnt/SDCARD/Roms/NES/Contra.zip");
	allow("roms/NES/", "/mnt/SDCARD/Roms/NES");
	/* Three dots is a filename, not a climb, and this case is here because it
	 * LOOKS like one: "..%2e" decodes to "..." and the first version of this
	 * file asserted it should be refused. It resolves inside the root and
	 * simply will not exist. Refusing it would be a check that had learned
	 * the shape of an attack rather than its meaning. */
	allow("roms/..%2e/x", "/mnt/SDCARD/Roms/.../x");

	printf("  climbing out, in every shape:\n");
	deny("roms/../TortOS/tortos.elf", "the launcher, overwritable from a phone");
	deny("roms/..", "one level is all it takes");
	deny("music/../TortOS", "up out of the Music root");
	deny("..", "no root at all, just up");
	deny("../TortOS", "up, before any root");
	deny("roms/NES/../../TortOS", "past the root from inside a subfolder");
	deny("roms/a/../../b", "a check that only reads the first component");
	deny("roms/NES/..%2f..%2fTortOS", "a slash that only exists after decoding");
	deny("roms/%2e%2e/TortOS", "dots that only exist after decoding");
	deny("roms/%2E%2E/TortOS", "the same, in upper case");
	deny("roms/.%2e/TortOS", "half encoded, half not - '.' plus '%2e' is '..'");
	deny("/mnt/SDCARD/TortOS/tortos.elf", "an absolute path, asked for plainly");
	deny("/roms/NES", "absolute, wearing a root's name");
	deny("cores/fceumm_libretro.so", "a root that is deliberately not offered");
	deny("tortos/tortos.elf", "a root that does not exist");
	deny("", "nothing at all");
	deny("roms%2fNES%2f..%2f..%2fTortOS", "the whole path encoded");

	printf("  malformed escapes are refused, not passed through:\n");
	/* A decoder that emits a literal '%' for a bad escape hands the next
	 * layer a string that decodes again. Refusing means there is one
	 * reading of anything that gets past here. */
	deny("roms/%2", "a truncated escape");
	deny("roms/%zz", "not hex");
	deny("roms/%2%65/x", "'%2' then 'e' - '.' to whoever decodes twice");
	deny("roms/%", "a bare percent");

	printf("  bytes that would change the path after it was approved:\n");
	deny("roms/NES/Contra.zip%00.png", "a NUL, truncating past the check");
	deny("roms/NES/a%0d%0ab", "a newline, for whatever reads this next");
	deny("roms/NES/a%09b", "a tab");

	printf("  and things too long to fit:\n");
	{
		char big[XFER_PATH_MAX * 2];
		int n = snprintf(big, sizeof big, "roms/");
		memset(big + n, 'a', sizeof big - (size_t)n - 1);
		big[sizeof big - 1] = '\0';
		deny(big, "one component longer than any name");

		/* Exactly at the caller's buffer, rather than at ours. */
		CHECK(!xfer_resolve("roms/NES/Contra.zip", out, 8),
		      "a path was written into a buffer too small to hold it");
	}

	printf("  the launcher's own config is not a save state:\n");
	{
		/* Save states share a directory with cheevos.cfg and favorites.cfg.
		 * cheevos.cfg is the only record of an unlock the account has not
		 * seen, so it must not be reachable by a route that deletes. */
		allow("states/NES/Contra%20(USA).zip.auto.state",
		      "/mnt/SDCARD/.userdata/shared/.tortos/NES/Contra (USA).zip.auto.state");
		deny("states/cheevos.cfg", "unlocks the account has not seen yet");
		deny("states/favorites.cfg", "the favorites list");
		deny("states/NES/anything.cfg", "a config anywhere under the root");
		deny("states/%63heevos.cfg", "the same, spelled with an escape");
		/* Only under that root - a .cfg elsewhere is an ordinary file, and
		 * over-blocking would be a rule that had learned a filename rather
		 * than a reason. */
		allow("roms/NES/notes.cfg", "/mnt/SDCARD/Roms/NES/notes.cfg");
	}

	printf("  a rename's destination is one name, not a path:\n");
	CHECK(xfer_name_ok("Contra (USA).zip"), "an ordinary name was refused");
	CHECK(xfer_name_ok(".media"), "a dot-leading name was refused");
	CHECK(!xfer_name_ok(""), "an empty name was allowed");
	CHECK(!xfer_name_ok("."), "'.' was allowed");
	CHECK(!xfer_name_ok(".."), "'..' was allowed");
	CHECK(!xfer_name_ok("../x"), "a name climbed a directory");
	CHECK(!xfer_name_ok("a/b"), "a name carried a separator");
	CHECK(!xfer_name_ok("/etc/passwd"), "a name was an absolute path");
	CHECK(!xfer_name_ok("a\nb"), "a name carried a newline");
	/* vfat drops these silently, so the rename would report success and
	 * produce a file the next listing cannot find under the name given. */
	CHECK(!xfer_name_ok("trailing "), "a trailing space was allowed");
	CHECK(!xfer_name_ok("trailing."), "a trailing dot was allowed");

	/* The rest of what vfat will not store. These reached open() and came back
	 * EINVAL, which the upload route then reported as 507 "could not write
	 * that to the card" - a full-disk message about a legal-looking name. The
	 * colon is the one that turns up on a real card: plenty of dumps are
	 * named "Game: Subtitle". */
	printf("  and the characters vfat will not store:\n");
	CHECK(!xfer_name_ok("Game: Subtitle.zip"), "a colon was allowed");
	CHECK(!xfer_name_ok("what?.zip"),          "a question mark was allowed");
	CHECK(!xfer_name_ok("star*.zip"),          "an asterisk was allowed");
	CHECK(!xfer_name_ok("pipe|x.zip"),         "a pipe was allowed");
	CHECK(!xfer_name_ok("a<b.zip"),            "a less-than was allowed");
	CHECK(!xfer_name_ok("a>b.zip"),            "a greater-than was allowed");
	CHECK(!xfer_name_ok("say\"hi\".zip"),      "a double quote was allowed");
	CHECK(!xfer_name_ok("back\\slash.zip"),    "a backslash was allowed");
	/* Still legal, and the reason the check is a set and not "punctuation":
	 * these turn up in real ROM names constantly. */
	CHECK(xfer_name_ok("Pokemon - Emerald (USA, Europe).zip"),
	      "an ordinary ROM name was refused");
	CHECK(xfer_name_ok("Marvel vs. Capcom [!].zip"),
	      "brackets and dots were refused");
	CHECK(xfer_name_ok("apostrophe's & ampersand.zip"),
	      "an apostrophe or ampersand was refused");

	if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
	printf("\nok: every check passed\n");
	return 0;
}
