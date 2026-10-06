/* SPDX-License-Identifier: MIT AND PolyForm-Noncommercial-1.0.0 */
#ifndef TORTOS_PLATFORM_H
#define TORTOS_PLATFORM_H

#include <SDL.h>
#include <stdbool.h>

/* Everything that knows it is running on a TrimUI Brick lives here: the
 * display, the buttons that arrive on three different devices, the panel
 * backlight, the codec, the battery, and the pipe to the resident emulator.
 * The rest of TortOS talks to this file and to SDL, and to nothing else. */

/* Layout is in units of a screen at least 1024 wide and 768 tall on every
 * device; the panel is plat_scale() pixels to the unit. Whichever side the
 * panel's shape leaves longer grows - 768 tall on the Brick, 921 on the GKD's
 * 1600x1440, 1152 wide on the RG SP's 720x480 - so both are variables, set
 * once by plat_video_init, that read like constants. */
#define TORTOS_SCREEN_W plat_screen_w
#define TORTOS_SCREEN_H plat_screen_h
extern int plat_screen_w, plat_screen_h;
float plat_scale(void);
/* For plat_video_init, once the renderer knows its output size: derives the
 * scale and height from it and sets the renderer's scale to match. */
void plat_geometry_init(SDL_Renderer *r);

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

/* TG4040 rather than TG3040: the Brick Pro. Same panel, codec and PMIC; the
 * differences are its sticks, its buttons and one more ring of LEDs. */
bool plat_is_brick_pro(void);

/* The keys that change brightness, as the Controls page names them. */
const char *plat_bright_keys(void);

/* What a game's process is told about the device, which the launcher cannot
 * derive from its own paths: NULL-terminated "KEY=VALUE" pairs, and the
 * library directories searched after $P_ROOT/lib (":dir..." or empty). Both
 * from the device file; main.c's build_child_env() adds the rest. */
extern const char *const plat_child_env[];
extern const char plat_child_libpath[];
/* A library preloaded into native PICO-8 (pico8_64), or NULL. */
extern const char *const plat_pico8_preload;
/* A folder put first on native PICO-8's PATH alone, or NULL: the Brick's
 * holds a wget that can do HTTPS (sd/tortos/pico8/wget). */
extern const char *const plat_pico8_path;

/* Two sticks (the Brick Pro): the first is then labelled "L Stick" (diatom
 * ADR-0044). */
bool plat_two_sticks(void);

typedef enum {
	IN_LEFT, IN_RIGHT, IN_UP, IN_DOWN,
	IN_ACCEPT, IN_BACK, IN_X, IN_Y,
	IN_L1, IN_R1, IN_START, IN_SELECT, IN_MENU,
	IN_VOLUP, IN_VOLDN, IN_BRIGHTUP, IN_BRIGHTDN,
	IN_POWER,
	/* For the Hotkeys screen's press-to-bind (plorpos-gkd.43.3) - nothing
	 * else acts on them. The stick still drives IN_LEFT..IN_DOWN as well, so
	 * every menu scrolls with it as before; IN_SLEFT..IN_SDOWN say it was the
	 * stick, set in the same frame. Order: left, right, up, down. */
	IN_L2, IN_R2, IN_L3, IN_HOME,
	IN_SLEFT, IN_SRIGHT, IN_SUP, IN_SDOWN,
	/* The Brick Pro's right stick: its click and its directions, which
	 * drive nothing else (diatom's ADR-0044). Same order. */
	IN_R3, IN_RSLEFT, IN_RSRIGHT, IN_RSUP, IN_RSDOWN,
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
/* Video for a menu drawn while a frozen child still holds the display, and
 * the present every frame of it goes through. Only the Brick differs: no
 * compositor, so no window either (plorpos-reo.8). */
bool plat_video_init_over_child(void);
void plat_present(SDL_Renderer *r);
unsigned plat_now_ms(void);

bool plat_input_init(void);
void plat_input_quit(void);
void plat_input_poll(in_state *st);
void plat_input_flush(void);
bool in_repeat(in_state *st, in_button b);

/* Tell the input layer the process is going away. The next plat_input_poll
 * raises quit_requested, which every loop already checks. */
void plat_terminate(void);

/* What the menu over a frozen child chose. */
typedef enum { RUN_CONTINUE, RUN_RESET, RUN_QUIT } run_choice;
/* Called with the child frozen and off the screen. The display is the
 * callee's while it runs: it brings its own video up and takes it down again
 * before returning. */
typedef run_choice (*run_menu_fn)(void *ctx);
/* Called about ten times a second while the child runs, for the launcher's
 * own work during the run (Muse's queue). Says whether the child should be
 * silent - Muse is playing - which plat_run applies to its sound stream.
 * Must not block: the same loop watches the power button. */
typedef bool (*run_tick_fn)(void *ctx);

/* Run a child to completion, watching the power button while it runs - and
 * Menu too when on_menu is given, for a child with no menu a pad can reach
 * (native PICO-8). Menu freezes it and hands the screen to on_menu where the
 * device can take a frozen window off the screen (the GKD, under sway);
 * elsewhere Menu ends it, as a power hold does. Reset starts the same argv
 * again. on_tick, when given, runs throughout. envkv is a NULL-terminated
 * array of "KEY=value" strings. */
int  plat_run(char *const argv[], const char *const envkv[], const char *workdir,
              run_menu_fn on_menu, run_tick_fn on_tick, void *ctx);
bool plat_run_power_pressed(void);
/* How long the last plat_run spent asleep (a power tap), to keep out of
 * Play Time. */
unsigned plat_run_asleep_ms(void);
/* How long ago the Menu press that opened plat_run's menu was made. */
unsigned plat_run_menu_age_ms(void);
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
/* One game for the resident. `console` is a RetroAchievements console id and
 * `cheevos` a set file for Diatom to watch; 0 and NULL mean the game has no
 * achievements, which is the ordinary case (Diatom ADR-0026). `save` is the
 * game's save dir; NULL leaves it at Diatom's --save (plorpos-aev). `opts`
 * are this game's own core options, "key=value" or NULL, sent after the
 * system's so they win - a Game Boy game's palette (plorpos-gkd.76). */
typedef struct {
	const char *tag, *core, *rom;
	const char *resume, *exit_state, *preview;
	const char *save;
	int         console;
	const char *cheevos;
	const char *opts[2];
	int         disc;   /* an .m3u's disc to start on, 1-based; 0 = the core's choice (plorpos-gkd.47) */
	const char *shader; /* SETDISPLAY fields for the chain, "" for None, NULL with no shader list */
} plat_game;
bool plat_resident_send(const plat_game *g);
int  plat_resident_wait(void);

/* Called from inside plat_resident_wait when Diatom reports an achievement
 * unlocked. A callback rather than a queue because the wait blocks: anything
 * buffered would have to be sized against a play session, and this has nothing
 * to size. NULL to stop. */
void plat_resident_on_unlock(void (*fn)(int id));

/* The same, when the Screenshot hotkey's file is on the card or failed
 * (Diatom's SHOT, plorpos-gkd.86.2): whether it worked, and the file. */
void plat_resident_on_shot(void (*fn)(bool ok, const char *path));

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

/* The hotkey submenu's binding for this system - "l2:ff,r2:rewind,..." - or
 * "" for none set. Never NULL: an empty spec is itself a valid SETHOTKEYS
 * value (diatom's ADR-0035). plat_hotkey_set persists it and updates the
 * cache in the same call, for the settings screen that edits it live. */
const char *plat_hotkey_map(const char *tag);
void        plat_hotkey_set(const char *tag, const char *spec);

/* The hotkey modifier (diatom's ADR-0038): the choices this device offers as
 * parallel wire-name/label arrays (count returned, first = default), the
 * stored choice (validated, never NULL), and setting it. Global, db_dev. */
int         plat_hotkey_modifiers(const char *const **wire, const char *const **label);
const char *plat_hotkey_modifier(void);
void        plat_hotkey_modifier_set(const char *wire);

/* Where Diatom is actually drawing the game, from its DISPLAY message. False
 * until it has said, which is the standalone path and the first moments of a
 * launch. Cached rather than asked for: Diatom reports it from the one place
 * its mode or rect can change, settling included (its ADR-0022), so the last
 * one heard is current. */
bool plat_resident_rect(SDL_Rect *out);
/* Draw the paused game's preview where Diatom is drawing the game, or over the
 * whole screen until it has said. Diatom's rect is in the panel's own
 * pixels, which are layout units only where plat_scale() is 1. */
void plat_draw_paused(SDL_Renderer *r, SDL_Texture *bg);
/* Read replies for up to timeout_ms, stopping as soon as a DISPLAY arrives.
 * For changing the mode from the in-game menu: Diatom answers a SETDISPLAY with
 * the new rect, and the menu wants it now so its backdrop can redraw where the
 * game is about to be, rather than on the next wait after resuming. A missed
 * reply costs a stale backdrop, never a hang. */
bool plat_resident_sync_rect(int timeout_ms);   /* false: timed out, or ERROR */
/* Read replies for up to timeout_ms until Diatom confirms the SAVE to path
 * (its SAVED line). False on an ERROR, a timeout, or no Diatom at all. */
bool plat_resident_saved(const char *path, int timeout_ms);
/* Read replies for up to timeout_ms until Diatom's DISC line, the answer to a
 * DISC or a SETDISC (plorpos-gkd.47). index is 0-based; count 0 means the core
 * has no disc to swap. False on a timeout or no Diatom at all. */
bool plat_resident_disc(int *index, int *count, int timeout_ms);

void plat_request_poweroff(void);
void plat_leds_off(void);

/* BEGIN PolyForm-Noncommercial-1.0.0 - NextUI-derived: the sleep interface, NextUI's PWR_sleep and PWR_deepSleep. See NOTICE. */
/* Real suspend-to-RAM - NextUI's PWR_deepSleep and its suspend script,
 * reached only from plat_light_sleep's escalation. See platform.c for what
 * "supported" actually probes. plat_sleep() blocks for the whole suspend and
 * returns once the device wakes: true if it suspended, or if POWER was
 * pressed before it managed to (a wake, not a failure); false if it could not
 * (unsupported, or every attempt failed - the script's nonzero exit). Never
 * a crash, never a wrong write, when this kernel does not offer it. */
bool plat_sleep_supported(void);
bool plat_sleep(void);

/* NextUI's PWR_sleep: screen off, sound muted, CPU awake, until the power
 * button is released - or, left for the Suspend Timeout, real suspend via
 * plat_sleep(). The one way into sleep, whatever asked for it. Blocks for the
 * whole of it. Returns false only when escalation found no suspend to go to,
 * which NextUI answers by powering off; the caller does that. waited_ms is
 * time the screen has already been dark (main.c's music_dark, after the
 * music stopped), counted toward the Suspend Timeout; 0 otherwise. */
bool plat_light_sleep(unsigned waited_ms);

/* END PolyForm-Noncommercial-1.0.0 */
/* The backlight alone, off or back at the player's level - no mute, no input
 * flush, no escalation. For main.c's music_dark, the screen-off that lets an
 * album play on where light sleep would pause it (TortOS-a5k). */
void plat_screen(bool on);
/* BEGIN PolyForm-Noncommercial-1.0.0 - NextUI-derived: the sleep interface, NextUI's PWR_sleep (plat_screen above is this project's own). See NOTICE. */

/* A computer has enumerated the device - not merely a charger, which never
 * does. With charging, what keeps TortOS awake: it holds the idle clock and
 * postpones light sleep's escalation. NextUI's is_usb_connected, minus the
 * Keep Awake Over USB setting it is gated on there - a computer always counts
 * (TortOS-2pv). Logs each change. Reads sysfs; callers throttle. */
bool plat_usb_host(void);
/* END PolyForm-Noncommercial-1.0.0 */

/* The Suspend Timeout setting (main.c's PM_SUSPEND), in seconds: how long
 * light sleep waits unwoken before real suspend. Never 0. */
int plat_suspend_timeout_secs(void);

/* A tap sleeps (or powers off, when "power.tap" says to); a hold still powers off immediately, same as it always has - see
 * platform.c for the 400ms line between the two. Fed the button's current
 * level every time a caller already polls it: once a frame from the
 * shelf/menu path (a->in.down[IN_POWER]), once a tick from diatom_wait()'s
 * raw evdev watchdog during a game. One call handles both grains, because
 * only one of those two ever runs at a time - the same assumption fd_power
 * itself already makes. */
typedef enum { PWR_NONE, PWR_SLEEP, PWR_POWEROFF } pwr_action;
pwr_action plat_power_tap_or_hold(bool down);

/* True once, when the RES_PAUSED plat_resident_wait just returned answers a
 * mid-game tap's PAUSE: launch() sleeps and RESUMEs rather than opening the
 * menu. */
bool plat_resident_sleep_asked(void);

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
/* Main settings' Mute Switch row (TortOS-ib9). With lock on, the switch never
 * mutes, and plat_hold_switch answers whether it is down - read live, it is a
 * real switch - so music_dark can ignore buttons in a pocket. Loaded from db
 * "muteswitch" at plat_settings_init; the setter is for the menu. */
void plat_mute_switch_lock(bool lock);
bool plat_hold_switch(void);

/* Is a cable in the headphone jack? SW_HEADPHONE_INSERT on the codec's input
 * node, the same switch the volume ladder above already follows.
 *
 * Exposed because ADR-0029 puts the choice of OUTPUT in the launcher, and a
 * cable outranks everything else (src/audioout.c). The port keeps reading the
 * switch for its own reason - which volume window to use - so both halves ask
 * the hardware rather than one telling the other something it could get wrong. */
bool plat_headphones_present(void);
#if defined(PLATFORM_GKD)
/* PipeWire's default sink is the output, chosen in platform_gkd.c (gkd.9.3):
 * the Audio Output setting goes in, whether a Bluetooth sink exists comes
 * out. Nothing is sent to Diatom or Muse; both stay on "default". */
void plat_audio_speaker_only(bool on);
bool plat_bt_audio(void);
void plat_sink_follow(bool game);   /* a headset's own volume -> the level */
#endif
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
