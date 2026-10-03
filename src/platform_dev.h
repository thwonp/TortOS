/* SPDX-License-Identifier: MIT */
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
#include <sys/types.h>

#include "platform.h"

/* Defined in platform.c, opened and read by the device file too. */
extern int fd_power;                         /* -1 when the device has none */
extern volatile sig_atomic_t g_terminating;
/* One line to the resident emulator; false when it is not connected. */
bool dsend(const char *fmt, ...);

/* Press or release one button: `pressed` on the edge, `down` while held. */
void set_btn(in_state *st, in_button b, bool down);

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
/* capacity + status from a /sys/class/power_supply/<node> directory. */
/* true: a resident game is about to own the screen; false: it handed it back.
 * Under a compositor the launcher's window has to get out of its way. */
void screen_yield(bool to_game);
bool battery_read(const char *dir, int *pct, bool *charging);
/* Up to ms for a POWER press (1) or release (0) on fd_power; platform.c. */
bool power_key_within(int ms, int value);
/* The node Menu arrives on and its key code, for plat_run's on_menu;
 * device file. -1 when the device does not offer it there. */
int  menu_key(int *code);
/* The node the volume keys (KEY_VOLUMEUP/DOWN) arrive on, for plat_run;
 * -1 when the device does not offer it there. */
int  levels_fd(void);
/* Whether Home is held now, which makes the volume keys brightness keys. */
bool levels_alt(void);
/* Take a frozen child's window off the screen for plat_run's menu. While it
 * is hidden the pad is the launcher's alone, so what is pressed in the menu
 * is not replayed to the child when it thaws. false: this device cannot, and
 * Menu ends the child instead. */
bool child_hide(pid_t pid);
/* Give the pad back, and with `show` put the window back fullscreen. */
void child_restore(pid_t pid, bool show);
/* Silence a child's sound stream, or let it be heard: plat_run's on_tick.
 * Cheap to call every tick - it acts on a change, and on the first call for
 * a pid whatever it asks, because the sound server remembers a mute by
 * program name and a new run can start out muted. */
void child_quiet(pid_t pid, bool on);

#endif
