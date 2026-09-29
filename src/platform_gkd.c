/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The GKD 350H Ultra on vendor ROCKNIX, under sway: the device file for
 * PLATFORM=gkd, as platform_brick.c is for the Brick.
 *
 * Stubs for now (plorpos-gkd.3.2): every function a device file owes, each
 * answering "not here", so the GKD build links against the device's own
 * libraries and starts. Video, input and paths come next (gkd.3.3), then
 * levels and battery (gkd.3.4). diatom's port/gkd.c is the reference for all
 * of it. */
#include "platform.h"
#include "platform_dev.h"

#include <stdio.h>

/* What a game's process is told about the device - see build_child_env(). */
const char *const plat_child_env[] = {
	"PLATFORM=gkd350h", "DEVICE=gkd350h",
	"SDCARD_PATH=/storage/games-external",
	"BIOS_PATH=/storage/games-external/Bios",
	"CHEATS_PATH=/storage/games-external/Cheats",
	"SAVES_PATH=/storage/games-external/Saves",
	NULL
};
const char plat_child_libpath[] = "";

bool plat_is_brick_pro(void) { return false; }

SDL_Renderer *plat_renderer(void) { return NULL; }
bool plat_video_init(void)
{
	fprintf(stderr, "gkd: no video yet (plorpos-gkd.3.3)\n");
	return false;
}
void plat_video_quit(void) { }

bool plat_input_init(void) { return false; }
void plat_input_flush(void) { }
void plat_input_quit(void) { }
void plat_input_poll(in_state *st) { (void)st; }

void plat_leds_off(void) { }

/* No levels yet: -1 reads as "no mixer, no display", so the nudges do
 * nothing rather than moving a number that controls nothing. */
int mixer_fd = -1, disp_fd = -1;
int cur_vol = -1, cur_bright = -1;
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
void levels_save(void) { }
void apply_volume(int v) { (void)v; }
void apply_brightness(int b) { (void)b; }
void backlight_off(void) { }
void jack_forget(void) { }
void mute_forget(void) { }
void plat_settings_init(void) { }

void plat_audio_jack_poll(void) { }
bool plat_headphones_present(void) { return false; }
bool plat_mute_poll(bool own_volume) { (void)own_volume; return false; }
bool plat_muted(void) { return false; }
void plat_mute_switch_lock(bool lock) { (void)lock; }
bool plat_hold_switch(void) { return false; }

bool plat_sleep_supported(void) { return false; }
bool plat_sleep(void) { return false; }
bool plat_light_sleep(unsigned waited_ms) { (void)waited_ms; return false; }
bool plat_usb_host(void) { return false; }
int plat_suspend_timeout_secs(void) { return 30; }   /* never 0: platform.h */
pwr_action plat_power_tap_or_hold(bool down) { (void)down; return PWR_NONE; }

bool plat_battery(int *pct, bool *charging)
{
	(void)pct; (void)charging;
	return false;
}
