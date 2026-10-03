/* SPDX-License-Identifier: MIT */
#ifndef TORTOS_ATOMIC_H
#define TORTOS_ATOMIC_H

#include <stdbool.h>
#include <stdio.h>

/* Replacing a file without a window where it is empty.
 *
 * `fopen(path, "w")` truncates first and writes second. Between those two the
 * file is zero bytes ON DISK, and a device whose normal shutdown is a button
 * press - and which is about to power itself off on a timer - lands in that
 * window far more often than a machine somebody shuts down properly. Cut power
 * there and the old contents are not recovered, they are gone.
 *
 * So: write a temporary beside the target, then rename over it. The file is
 * either wholly the old contents or wholly the new ones.
 *
 * FAT's rename is not guaranteed atomic the way a journalled filesystem's is,
 * and this ships on FAT. It is still strictly better than truncate-then-write:
 * the window shrinks from "the whole time we are writing" to "the instant the
 * directory entry changes".
 *
 *     FILE *f = atomic_open(path, 0644);
 *     if (!f) return false;
 *     fprintf(f, ...);
 *     return atomic_commit(f, path);
 *
 * `mode` is applied at creation rather than chmod'ed afterwards, so a file
 * that should be private is never briefly readable. Worth knowing what that
 * buys on the target and what it does not: /mnt/SDCARD is exfat mounted
 * fmask=0022, and exfat carries no Unix permissions, so every file on the card
 * reads back 0755 whatever mode was asked for. Measured 2026-09-06 - a fresh
 * file chmod 600 came back -rwxr-xr-x. The mode is honored on ext4 and on a
 * development host, which is why it is still passed.
 */
FILE *atomic_open(const char *path, int mode);

/* Close and rename into place. False if anything failed, and on failure the
 * original file is left exactly as it was. */
bool atomic_commit(FILE *f, const char *path);

/* Close and discard - for the case where the caller decides mid-write that
 * there is nothing worth keeping. */
void atomic_abort(FILE *f, const char *path);

#endif
