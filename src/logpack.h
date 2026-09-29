/* SPDX-License-Identifier: MIT */
#ifndef TORTOS_LOGPACK_H
#define TORTOS_LOGPACK_H

#include <stdbool.h>
#include <stddef.h>

/* The logs, packed to send to whoever is helping: every boot's tortos.log, an
 * about.txt the caller writes, gzipped into one tar by the device's own tar.
 * Over The Hare's Download logs button is the one caller.
 *
 * MASKED ON THE WAY OUT, never on the card. A log names the Wi-Fi network,
 * the headset, and the RetroAchievements and ScreenScraper accounts - nothing
 * secret, nothing a stranger needs either. The caller hands over the names it
 * knows (every saved network, every paired headset, both account names) and
 * each is cut to its first and last character, "ReinFi" to "R...i". Bluetooth
 * addresses are found by their shape instead, since a log can name a device
 * nobody paired, and keep only their last byte. The files on the card are
 * copied, not touched: they are what the device's owner reads.
 *
 * No SDL, so tools/logpack-check.c can hand it a folder and look. */

/* `word` as it is shown: first and last character around "...", or "***"
 * for one too short to keep two characters of. */
void logpack_mask_word(const char *word, char *out, size_t n);

/* One line, masked in place: each of the `n` secrets wherever it appears, and
 * every Bluetooth address, AA:BB:CC:DD:EE:FF or AA_BB_CC_DD_EE_FF as BlueZ's
 * object paths write them. A secret shorter than three characters is left
 * alone - masking every "TV" in a log would hide more than it protects.
 * `cap` is the buffer's size; a line that would outgrow it is cut. */
void logpack_mask_line(char *line, size_t cap, const char *const *secrets, int n);

/* Pack `logs_dir`'s tortos.log files and `about` into `out` (a .tar.gz), with
 * everything inside one folder named `folder`. The staging happens under
 * /tmp and is removed again. False with a reason in `err` when it could not. */
bool logpack_build(const char *logs_dir, const char *about,
                   const char *const *secrets, int n,
                   const char *folder, const char *out, char *err, size_t en);

#endif
