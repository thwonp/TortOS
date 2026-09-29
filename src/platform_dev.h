/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* Private to platform.c and the device file built with it (platform_brick.c).
 * Not for the rest of the launcher: that talks to platform.h.
 *
 * platform.c holds what every device shares - the diatom socket, the level
 * state behind the OSD and its nudges, the power key while a game runs. The
 * device file drives the hardware under it. These are the names that cross
 * between the two. */
#ifndef TORTOS_PLATFORM_DEV_H
#define TORTOS_PLATFORM_DEV_H

#include <signal.h>
#include <stdbool.h>

/* Defined in platform.c, opened and read by the device file too. */
extern int fd_power;                         /* -1 when the device has none */
extern volatile sig_atomic_t g_terminating;
/* One line to the resident emulator; false when it is not connected. */
bool dsend(const char *fmt, ...);

/* The level state and its hardware, defined by the device file. mixer_fd and
 * disp_fd read >= 0 when the levels can be changed at all. */
extern int mixer_fd, disp_fd;
extern int cur_vol, cur_bright;
int  clampi(int v, int lo, int hi);
void levels_save(void);
void apply_volume(int v);
void apply_brightness(int b);
void backlight_off(void);
/* Forget what was remembered about the jack and the mute switch - called when
 * input ownership comes back from a game (plat_resident_wait). */
void jack_forget(void);
void mute_forget(void);

#endif
