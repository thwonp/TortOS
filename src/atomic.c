/* SPDX-License-Identifier: MIT */
/* See atomic.h. No SDL and no platform header, deliberately: the two files
 * that most need this are cheevos.c, which is kept SDL-free so its checks can
 * link without a window, and rafetch.c, which is checked the same way. */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "atomic.h"

/* Beside the target, not in /tmp: rename cannot cross filesystems, and on this
 * device /tmp is a RAM overlay while everything here is on the card. */
static void tmp_path(const char *path, char *out, size_t n)
{
	snprintf(out, n, "%s.new", path);
}

FILE *atomic_open(const char *path, int mode)
{
	char tmp[1024];
	FILE *f;
	int fd;

	if (!path || !*path) return NULL;
	tmp_path(path, tmp, sizeof tmp);

	/* O_CREAT with the mode rather than chmod after: a file created readable
	 * and tightened afterwards has a window where it is not. */
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, (mode_t)mode);
	if (fd < 0) return NULL;
	f = fdopen(fd, "w");
	if (!f) { close(fd); unlink(tmp); return NULL; }
	return f;
}

bool atomic_commit(FILE *f, const char *path)
{
	char tmp[1024];
	bool ok;

	if (!f) return false;
	tmp_path(path, tmp, sizeof tmp);

	/* Flush before renaming. Without this the rename can land while the
	 * contents are still in a buffer, which is the same lost-data window in a
	 * different place. */
	ok = fflush(f) == 0;
	if (ok) ok = fsync(fileno(f)) == 0;
	if (fclose(f) != 0) ok = false;

	if (!ok || rename(tmp, path) != 0) {
		unlink(tmp);
		return false;
	}
	/* Without this the rename reaches the card only with writeback, up to
	 * 30 s later, and a hard power-off inside that window brings back the
	 * previous file. It likely also narrows the window in which a reset
	 * mid-writeback can leave exFAT's bitmap and directory disagreeing, the
	 * suspected cause of cross-linked files (TortOS-pq0). About 25 ms on the
	 * card. */
	sync();
	return true;
}

void atomic_abort(FILE *f, const char *path)
{
	char tmp[1024];

	if (f) fclose(f);
	tmp_path(path, tmp, sizeof tmp);
	unlink(tmp);
}
