/* SPDX-License-Identifier: MIT */
#ifndef TORTOS_PLATFORM_H
#define TORTOS_PLATFORM_H

#include <SDL.h>
#include <stdbool.h>

/* Everything that knows it is running on a TrimUI Brick lives here: the
 * display, the buttons that arrive on three different devices, the panel
 * backlight, the codec, the battery, and the pipe to the resident emulator.
 * The rest of TortOS talks to this file and to SDL, and to nothing else. */

#define TORTOS_SCREEN_W 1024
#define TORTOS_SCREEN_H 768

/* launch.sh polls for this and powers the device down when it appears. */
#define TORTOS_POWEROFF_FLAG "/tmp/tortos_poweroff"

/* Overridable at runtime so the launcher can be pointed at a test tree. */
extern const char *P_ROOT;     /* /mnt/SDCARD/TortOS      */
extern const char *P_CARD;     /* /mnt/SDCARD             */
extern const char *P_ROMS;     /* /mnt/SDCARD/Roms        */
extern const char *P_USERDATA; /* /mnt/SDCARD/.userdata/tg3040 */
extern const char *P_SHARED;   /* /mnt/SDCARD/.userdata/shared */
extern const char *P_WEB;      /* /mnt/SDCARD/TortOS/res/web - Hare's page */
extern const char *P_FONT;     /* the UI typeface          */
void paths_init(void);

typedef enum {
	IN_LEFT, IN_RIGHT, IN_UP, IN_DOWN,
	IN_ACCEPT, IN_BACK, IN_X, IN_Y,
	IN_L1, IN_R1, IN_START, IN_SELECT, IN_MENU,
	IN_VOLUP, IN_VOLDN, IN_BRIGHTUP, IN_BRIGHTDN,
	IN_POWER,
	IN_COUNT,
	IN_NONE = -1,
} in_button;

typedef struct {
	bool   down[IN_COUNT];
	bool   pressed[IN_COUNT];   /* this frame only */
	Uint32 down_since[IN_COUNT];
	Uint32 last_repeat[IN_COUNT];
	bool   quit_requested;
} in_state;

bool plat_video_init(void);
void plat_video_quit(void);
SDL_Renderer *plat_renderer(void);
unsigned plat_now_ms(void);

bool plat_input_init(void);
void plat_input_quit(void);
void plat_input_poll(in_state *st);
void plat_input_flush(void);
bool in_repeat(in_state *st, in_button b);

/* Tell the input layer the process is going away. The next plat_input_poll
 * raises quit_requested, which every loop already checks. */
void plat_terminate(void);

/* Run a child to completion, watching the power button while it runs.
 * envkv is a NULL-terminated array of "KEY=value" strings. */
int  plat_run(char *const argv[], const char *const envkv[], const char *workdir);
bool plat_run_power_pressed(void);
/* Say that something equivalent to a power press has happened, for the paths
 * the evdev watchdog cannot see: a screen the launcher is drawing over a
 * paused game, where plat_resident_wait is not running and so nothing is
 * reading the power key. Auto Off expiring in the in-game menu is exactly
 * that. Cleared with the flag, at the start of the next game. */
void plat_note_power_pressed(void);

/* Start a child and forget it: it outlives this process and leaves no zombie
 * behind. This is how a resident emulator that has died gets started again. */
bool plat_spawn_detached(char *const argv[], const char *const envkv[],
                         const char *workdir);

/* The resident emulator. plat_resident_send() hands over a game and returns
 * at once - the launcher draws nothing over the load, because it and the
 * emulator presenting together wedges the display (see launch() in main.c),
 * and a warm launch leaves nothing to cover anyway; plat_resident_wait()
 * blocks until the game is over - or, on the Diatom transport, until the
 * player opens the in-game menu, which the launcher draws (the emulator hands
 * the display over rather than drawing its own).
 *
 * One transport: Diatom's socket protocol, where peer death is EOF, the stop
 * signal is a message, and the reply is a line saying what actually happened.
 * TORTOS_DIATOM_SOCKET overrides the path for tests. */
#define RES_DEAD   0   /* emulator missing, dead, or the game never started */
#define RES_EXIT   1   /* the game ran and is over */
#define RES_PAUSED 2   /* Diatom only: menu open, the launcher owns the display */
const char *plat_resident_socket(void);
bool plat_resident_ready(void);
/* `console` is a RetroAchievements console id and `cheevos` a set file for
 * Diatom to watch; 0 and NULL mean the game has no achievements, which is the
 * ordinary case. Diatom ADR-0026.
 *
 * Eight positional arguments, five of them paths, is one past comfortable -
 * the call already passes the same state path twice in a row. A struct is the
 * next change to this function, not a further parameter. */
bool plat_resident_send(const char *tag, const char *core, const char *rom,
                        const char *resume, const char *exit_state,
                        const char *preview,
                        int console, const char *cheevos);
int  plat_resident_wait(void);

/* Called from inside plat_resident_wait when Diatom reports an achievement
 * unlocked. A callback rather than a queue because the wait blocks: anything
 * buffered would have to be sized against a play session, and this has nothing
 * to size. NULL to stop. */
void plat_resident_on_unlock(void (*fn)(int id));

/* Called from inside plat_resident_wait roughly ten times a second, which is
 * the rate its socket poll already runs at. For work the launcher wants to do
 * WHILE a game is running and cannot do anywhere else, because this process is
 * blocked here for the whole session.
 *
 * Must not block. This loop is also the power button's watchdog, and the one
 * control that always has to work stops working for as long as anything here
 * takes. NULL to stop. */
void plat_resident_on_tick(void (*fn)(void));
/* Diatom only: one protocol line (RESUME, STOP, SAVE\tpath=...), newline added. */
bool plat_resident_line(const char *fmt, ...);
/* Whether the game should be heard - Diatom's QUIET, its ADR-0032. Sent when it
 * changes, and before every RUN whether it changed or not, so it is safe to
 * call as often as the answer might have moved. */
void plat_resident_quiet(bool on);
/* Where Diatom says its sound actually is (its ADR-0029), which may not be
 * where it was asked to put it - a sink that will not open, or one that died,
 * makes the port fall back and report the fallback. False until it has said. */
bool plat_resident_audio(char *out, size_t cap);
/* Forget that report: a SETAUDIO has just gone, and until Diatom answers it -
 * its audio_set always does - what it said last describes the moment before. */
void plat_resident_audio_asked(void);
/* The volume Diatom last reported while a game runs - an index on its own
 * ladder, with the ladder's length in *count - or -1 when it has said nothing
 * since the game started. It is applied to this side only when the game ends
 * (see d_apply_levels), so a headset that has to follow the keys DURING a game
 * reads it here. */
int  plat_resident_volume(int *count);
/* Increments each time the launcher connects to a resident emulator. Anything
 * the launcher pushed into the last one has to be pushed into a new one. */
unsigned plat_resident_generation(void);
/* Path from the most recent PREVIEW message, or "" - the menu's backdrop. */
const char *plat_resident_last_preview(void);
/* Core options the launcher wants applied to every game, read once from
 * the coreopt. namespace. Diatom keeps no per-core knowledge (its ADR-0019
 * and register section 12), so the per-core opinions live here. Each entry is
 * a whole "key=value" string; a core that does not declare the key ignores it.
 * Applied BEFORE the game loads, because options marked (Restart) are read at
 * retro_load_game and setting one afterwards does nothing until next launch. */
int         plat_coreopt_count(const char *tag);
const char *plat_coreopt(const char *tag, int i);
/* The turbo map for this system, or NULL. Diatom's ADR-0028, docs/turbo.md. */
const char *plat_turbo_map(const char *tag);

/* Where Diatom is actually drawing the game, from its DISPLAY message. False
 * until it has said, which is the standalone path and the first moments of a
 * launch. Cached rather than asked for: Diatom reports it from the one place
 * its mode or rect can change, settling included (its ADR-0022), so the last
 * one heard is current. */
bool plat_resident_rect(SDL_Rect *out);
/* Read replies for up to timeout_ms, stopping as soon as a DISPLAY arrives.
 * For changing the mode from the in-game menu: Diatom answers a SETDISPLAY with
 * the new rect, and the menu wants it now so its backdrop can redraw where the
 * game is about to be, rather than on the next wait after resuming. A missed
 * reply costs a stale backdrop, never a hang. */
bool plat_resident_sync_rect(int timeout_ms);

void plat_request_poweroff(void);
void plat_leds_off(void);

/* Real suspend-to-RAM, distinct from Auto Off's power-off (sys_menu.c). See
 * platform.c for what "supported" actually probes and why sleep needs no
 * protocol message to Diatom. plat_sleep() blocks for the whole suspend and
 * returns once the device wakes; a caller resumes exactly where it called
 * from, with nothing to poll for in between. Both are safe no-ops - never
 * a crash, never a wrong write - when this kernel does not offer it. */
bool plat_sleep_supported(void);
void plat_sleep(void);

/* The two level scales, stated once. TortOS shares them verbatim with
 * launch.sh and with Diatom, so a level crossing the socket needs no
 * conversion - which only holds while every place that rescales a level agrees
 * on the top of the range, so they live here rather than beside the ladder
 * table that only the settings code can see. */
#define PLAT_VOL_MAX     20   /* 21 positions, 0..20 */
#define PLAT_BRIGHT_MAX  11   /* a 12-rung geometric ladder, 0..11 */

/* Levels come from the settings database: seeded from the shipped defaults on
 * first run, then overwritten by every nudge of the rocker. There is no longer
 * a saved value and a fallback to reconcile, which is why this takes nothing -
 * it used to be handed the config defaults so the precedence rule lived in one
 * place, and now the precedence has nowhere to disagree with itself.
 *
 * launch.sh applies the same level to the boot animation before this process
 * exists, from the boot.env the launcher exports. See src/db.h. */
void plat_settings_init(void);
int  plat_volume_get(void);
int  plat_brightness_get(void);
void plat_volume_nudge(int delta);
void plat_brightness_nudge(int delta);
void plat_volume_set_pct(int pct);
void plat_brightness_set(int level);
/* Headphones and the speaker want different volume ladders - the jack covers
 * 61 dB where the speaker covers 45 - so the level has to be re-applied when a
 * plug goes in or comes out, not merely at the next volume press. Call from a
 * periodic path; it is one ioctl and writes nothing unless the state changed.
 * A no-op on the host. */
void plat_audio_jack_poll(void);

/* The hardware mute switch. plat_mute_poll reads it and returns true on the
 * frame it CHANGED, so a caller can tell a running game; plat_muted answers
 * the current position. Muting the launcher's own audio is handled inside. */
/* `own_volume` is false while a game is running: Diatom owns the level then,
 * so only the speaker switch may be touched and never the gain. */
bool plat_mute_poll(bool own_volume);
bool plat_muted(void);

/* Is a cable in the headphone jack? SW_HEADPHONE_INSERT on the codec's input
 * node, the same switch the volume ladder above already follows.
 *
 * Exposed because ADR-0029 puts the choice of OUTPUT in the launcher, and a
 * cable outranks everything else (src/audioout.c). The port keeps reading the
 * switch for its own reason - which volume window to use - so both halves ask
 * the hardware rather than one telling the other something it could get wrong. */
bool plat_headphones_present(void);
/* kind: 1 = brightness, 2 = volume */
void plat_osd_show(int kind, int val, int max);
void plat_draw_osd(SDL_Renderer *r);
/* When the volume or brightness line will next change on its own - the moment
 * it is due to disappear - or UINT32_MAX while none is showing.
 *
 * It does not fade: it is drawn solid until OSD_WINDOW_MS after the last press
 * and then simply not drawn. So a launcher that only redraws when something
 * changes needs exactly one more frame, at that moment, or the line would
 * stay on screen until the next unrelated redraw. */
Uint32 plat_osd_until(void);

bool plat_battery(int *pct, bool *charging);

#endif
