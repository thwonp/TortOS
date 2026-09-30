/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef TORTOS_XFER_H
#define TORTOS_XFER_H

#include <stdbool.h>
#include <stddef.h>

/* Hare: what the browser is allowed to touch, and nothing else.
 *
 * The tortoise runs the system; the hare carries the files. This half is the
 * boring half, and it is the half that matters: a file manager reachable from
 * the network is owned through its paths, every time. So the roots live here,
 * resolving lives here, and nothing outside this file is trusted to have got
 * a path right.
 *
 * THE ORDER IS THE POINT. xfer_resolve percent-decodes and THEN validates,
 * inside one function, because a caller who decodes first and checks second
 * lets `%2e%2e%2f` through and a caller who checks first and decodes second
 * does the same thing with extra steps. There is no way to call this in the
 * wrong order because there is only one call.
 *
 * Validation is lexical - reject "..", match a root prefix - which is sound
 * only because the card is vfat and has no symlinks to follow out of a root.
 * Checked on the device 2026-08-30: `ln -s` fails with EPERM. If this ever
 * lands on a filesystem that has them, this becomes realpath on the parent
 * plus a prefix test, and that is not a small change - it is a different
 * function with different failure modes for files that do not exist yet.
 */

#define XFER_PATH_MAX 1024
#define XFER_NAME_MAX  255

typedef struct {
	char name[16];              /* what a URL calls it: "roms" */
	char label[32];             /* what a person calls it: "ROMs" */
	char path[XFER_PATH_MAX];   /* absolute, no trailing slash */
	/* Save states share a directory with two config files the launcher
	 * writes. Nothing under this root may be a .cfg - not listed, not
	 * fetched, not deleted. See xfer_init. */
	bool no_cfg;
} xfer_root;

/* Paths in, rather than platform.h out, so this links into a check without
 * dragging SDL behind it. */
void xfer_init(const char *roms_dir, const char *card_dir,
               const char *shared_dir);

int              xfer_root_count(void);
const xfer_root *xfer_root_at(int i);

/* A URL path - "roms/NES/Contra%20(USA).zip" - to an absolute one on the
 * card. False for anything that escapes a root, whatever shape the escape
 * takes, and for anything that will not fit.
 *
 * Says nothing about whether the result exists: an upload target does not,
 * and that is the caller's question, not this one's. */
bool xfer_resolve(const char *url_path, char *out, size_t outn);

/* WHAT A DELETE MAY TAKE, for a folder `abs` that resolved into a root.
 *
 * A file is always the file alone. A folder is one of three, decided here and
 * enforced by the server rather than trusted to the page:
 *
 *   NO     a root itself, even empty: TortOS's own folders, and the ones
 *          Over The Hare recreates when it starts anyway
 *   EMPTY  only once it is empty: a console's folder under ROMs, so one click
 *          cannot take every game for a system; a console's save states; and
 *          anything in BIOS or Saves, which are flat by design
 *   ALL    with everything in it: a game's own folder inside a console, an
 *          artist, an album, a book - the things a person deletes whole
 *
 * Eric's, 2026-09-29. `why`, when not NULL, is set to a line the page can show
 * for NO and EMPTY. Pure, and decided on the resolved path, so no spelling of
 * a URL reaches a different answer. */
typedef enum { XFER_DEL_NO, XFER_DEL_EMPTY, XFER_DEL_ALL } xfer_del;
xfer_del xfer_delete_rule(const char *abs, const char **why);

/* Percent-decoding on its own, for values that are NOT paths: a rename's
 * destination, a PIN. Same decoder xfer_resolve uses, exposed rather than
 * copied, because a second decoder is a second set of rules about what "%2e"
 * means and the whole point of this file is that there is one.
 *
 * False on a malformed escape, on a control byte, or on anything too long.
 * Says nothing about whether the result is a usable name - that is
 * xfer_name_ok, and a caller wanting a filename needs both. */
bool xfer_decode(const char *in, char *out, size_t outn);

/* One path component, for a rename's destination. No separators, no dot
 * entries, nothing empty. Rejects what xfer_resolve would reject, minus the
 * root lookup, because a rename names a sibling rather than a path. */
bool xfer_name_ok(const char *name);

#endif
