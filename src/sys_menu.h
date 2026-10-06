/* SPDX-License-Identifier: MIT */
/* The two menus behind the MENU button, as rows rather than as pixels.
 *
 * MENU has two menus behind it, chosen by where it was pressed. From the
 * systems row it is about the firmware. From inside a system it is about THAT
 * system, because a menu that repeated the firmware's settings while a shelf
 * of NES games sat behind it would be answering a question nobody asked.
 *
 * Split out of main.c under ADR-0001 so that what these menus CONTAIN can be
 * checked on a build machine. Nothing here may include SDL: tools/menu-check.c
 * links this file without it, and that is the whole point.
 *
 * The caller reads the device and fills a sys_ui; this file turns that into
 * rows. Wi-Fi state in particular is asked for by the caller because asking
 * costs a fork - see menu_wifi in main.c - and this file must not care. */
#ifndef SYS_MENU_H
#define SYS_MENU_H

#include <stdbool.h>
#include <stddef.h>

#include "menu.h"
#include "config.h"
#include "wifi.h"
#include "audioout.h"

/* The plorpOS menu, in the order it is read.
 *
 * Play Time leads because it is the only row here anyone opens twice. Wi-Fi,
 * Bluetooth and Audio Output are setup: you use them when something is wrong
 * or new, and then never again. A menu ordered by what a device needs on its
 * first day puts the thing you actually come back to five rows down. */
/* Controls and About are the two rows you only read, so they sit together at
 * the end, after everything that changes something.
 *
 * The rest are grouped a level down since plorpos-z0d.1: the timers under
 * System Settings, the look under UI Settings, and Cheevos and Over The Hare
 * on the Wi-Fi Services screen with the other things that need the network. */
typedef enum {
	PM_STATS,
	PM_WIFI,
#if !defined(PLATFORM_H700)   /* Bluetooth is the H700's v2 (plorpos-7ny) */
	PM_BT,
#endif
	PM_AUDIO,
	PM_SYSTEM, PM_UI, PM_SCRAPING,
	PM_CONTROLS, PM_ABOUT, PM_ROWS
} pm_row;

/* Settings > System Settings: the three timers, the battery indicator, and
 * the side switch where there is one. */
typedef enum {
	ST_AUTO_OFF, ST_SLEEP, ST_SUSPEND, ST_BATTPCT,
#if !defined(PLATFORM_GKD) && !defined(PLATFORM_H700)   /* no switch: Muse Settings' Sleep Button Lock (gkd.34) */
	ST_MUTESW,
#endif
	ST_ROWS
} st_row;

/* Settings > UI Settings: how the shelves look, and nothing about what is on
 * them. */
typedef enum { US_THEME, US_DIR, US_ROWS } us_row;

/* Settings > Scraping (TortOS-mh0): everything that puts art and text on a
 * card, in one place - the Box Art job, the ScreenScraper account it signs in
 * with, and importing gamelist.xml metadata already on the card. */
typedef enum { SC_BOXART, SC_SS, SC_IMPORT, SC_ROWS } sc_row;

/* The system menu. Games and Core carry real values rather than invented ones,
 * because a placeholder that lies about the machine it is describing is worse
 * than no row. The rest name capabilities that already exist on the emulator
 * side - Diatom has per-system display modes and core-supplied button labels -
 * so these are hooks waiting to be wired, not wishes. */
typedef enum {
	SM_GAMES, SM_CORE, SM_SORT,
	SM_DISPLAY, /* SM_BUTTONS, */ SM_BOXART, SM_RESCAN, SM_ROWS
} sm_row;

/* What the Favorites shelf's menu is, which is two of those rows.
 *
 * Favorites is not a system: build_favorites_shelf memsets one and fills in a
 * name, a tag, a card and an accent, leaving no core, no folder and no
 * extensions. Four of the six rows have nothing to work with - Core is blank,
 * Box Art and Rescan have no folder to scrape or sweep, and Display Mode is
 * worse than blank, because launch() resolves a game's CORE through its owner
 * and its display mode through the shelf it was started from. Setting one here
 * would give a game a different aspect depending on which shelf launched it.
 *
 * Games and Sort By are both true of a shelf of favorites, so those are what
 * it gets. Eric's call, 2026-09-17. */
#define SM_FAV_ROWS 2

/* What Muse's shelf menu is: how many albums, Show - music or audiobooks,
 * the two kinds its one shelf holds - Sort By - by artist or by album, its own
 * two orders, where a console's shelf has four - Album Art to fetch covers for
 * the albums that have none or only a small one, and Rescan Folder for music
 * copied onto the card while the Brick was on - and Muse Settings, Muse's own
 * options (TortOS-28l). Muse has no core, no ROM folder and no display mode. Album order was Eric's, 2026-09-19; Show, 2026-09-27.
 *
 * On books the count is Books, Sort By is by author or title, and Album Art is
 * not there: MusicBrainz knows records. Show is there only when the card has
 * both kinds, since with one there is nothing to choose between. So the rows
 * move, and which one is where is sys_menu_muse_rows's to say. */
typedef enum { SMM_COUNT, SMM_SHOW, SMM_SORT, SMM_ART, SMM_RESCAN,
               SMM_SETTINGS } sm_muse_row;
#define SM_MUSE_ROWS 6

/* Muse's rows in order, for the kind its shelf shows and whether the card has
 * both. Returns how many. The build and the key handler both ask this, so the
 * two cannot disagree about what row 2 is. */
int sys_menu_muse_rows(bool books, bool both, sm_muse_row *out);

#define MENU_MAX_ROWS 17

/* The array every caller declares must hold every row a build can produce, and
 * on 2026-09-16 it stopped: PM_ROWS went to 13 against a cap of 12 when the
 * ScreenScraper row landed, so sys_menu_build wrote one past the end of the
 * caller's stack array. The launcher aborted on its first frame and
 * tools/menu-check.c passed, because it overflows identically and nothing
 * there was watching that memory.
 *
 * A number that has to be kept in step with an enum, by hand, in another file.
 * Now it is the compiler's job.
 *
 * _Static_assert and not #if: PM_ROWS is an enum constant, and the
 * preprocessor reads an identifier it does not know as 0 - so `#if
 * MENU_MAX_ROWS < PM_ROWS` is `13 < 0`, false forever, a guard that compiles
 * and protects nothing. Written that way first, and only caught by putting the
 * cap back to 12 and watching the build succeed. */
_Static_assert(MENU_MAX_ROWS >= PM_ROWS, "MENU_MAX_ROWS < PM_ROWS");
_Static_assert(MENU_MAX_ROWS >= SM_ROWS, "MENU_MAX_ROWS < SM_ROWS");
_Static_assert(MENU_MAX_ROWS >= SC_ROWS, "MENU_MAX_ROWS < SC_ROWS");
_Static_assert(MENU_MAX_ROWS >= ST_ROWS, "MENU_MAX_ROWS < ST_ROWS");

/* Where the built rows' text lives. A row holds pointers, not copies, so the
 * strings a build formats have to outlive the build; the caller owns this and
 * keeps it alive as long as it keeps the rows. */
typedef struct { char a[24], b[CFG_STR], c[16], d[40], e[16]; } menu_bufs;

/* Everything either menu needs to know about the device, gathered by the
 * caller. A struct rather than a dozen arguments so that adding a fact to a
 * row does not change every call site, and so a check can state a device's
 * whole situation in one initializer. */
typedef struct {
	bool games;              /* the system menu, rather than TortOS's own */
	bool fav;                /* and that shelf is Favorites; see SM_FAV_ROWS */
	bool muse;               /* or Muse's; see SM_MUSE_ROWS */
	bool muse_books;         /* and it shows books rather than music */
	bool muse_both;          /* and the card has both kinds */

	wifi_state  wifi;        /* already cached by the caller; see menu_wifi */
	const char *ssid;        /* the network's name when connected, else NULL */

	/* The plorpOS menu */
	/* The ScreenScraper account, which needs one more fact than the
	 * RetroAchievements one: whether this build can reach them at all. The
	 * developer key comes from the environment at build time and a build
	 * without it cannot sign anybody in, so the row has three states rather
	 * than two - see src/ss.h and BACKLOG 31. */
	bool        ss_have;     /* this build carries a developer key */
	bool        ss_in;
	const char *ss_name;     /* only read when ss_in */
	const char *cards;       /* the showing card set's name, from CARD_SETS */
	const char *cards_dir;   /* which way the shelves run, from CARD_DIRS */
	int         auto_off;    /* Auto Sleep, seconds, 0 for off */
	int         auto_poweroff; /* Auto Off, seconds, 0 for off - mutually
	                             * exclusive with auto_off, see PM_AUTO_OFF */
	int         suspend_timeout; /* seconds light sleep waits before real
	                              * suspend, never 0 - see PM_SUSPEND */
	bool        mute_lock;   /* the mute switch is a button lock instead */
	bool        batt_pct;    /* the battery's percentage, top right (plorpos-gkd.86.3) */
	/* Where sound goes: the policy the player set, and where it actually ends
	 * up under that policy. Both, because the row has to name a place - "Auto"
	 * on its own is a rule, not somewhere you can hear. */
	aout_policy audio_policy;
	aout_dest   audio_dest;

	/* Bluetooth: the name of the connected headset, or NULL. The row used to
	 * read "not yet" and be dead, which was true of the pairing screen and
	 * false of the feature - game audio has gone to a headset since
	 * 2026-09-03. */
	const char *bt_name;

	/* The system menu */
	const char *sys_name;
	const char *sys_core;
	/* PICO-8's shelf: the engine's label, shown in Core's place and turned
	 * over there. NULL on every other shelf, whose Core is a fact. */
	const char *engine;
	int         game_count;
	const char *dmode;
	/* The label of the shelf's sort order. A label rather than an index for
	 * the same reason dmode is one: this file must not know the table, or a
	 * check that links it would have to link the table too. */
	const char *sort;
} sys_ui;

/* Seconds to the label a row shows, in NextUI's own spelling (its
 * settings.cpp labels): seconds up to 90s, whole minutes from 2m. Pure, and
 * here rather than in main.c so the check can hold it to "never", "90s" and
 * "2m" without a device. */
void sys_menu_auto_off_label(int seconds, char *out, size_t n);

/* Build whichever menu u->games calls for. Returns the row count, so the input
 * loop never needs to know which of the two it is driving. */
int sys_menu_build(const sys_ui *u, menu_row *out, menu_bufs *b,
                   const char **heading);

/* Settings > Scraping's rows, from the same sys_ui. Returns SC_ROWS. */
int sys_menu_scraping_build(const sys_ui *u, menu_row *out, const char **heading);

/* Settings > System Settings. Returns ST_ROWS. */
int sys_menu_system_build(const sys_ui *u, menu_row *out, menu_bufs *b,
                          const char **heading);

/* Settings > UI Settings. Returns US_ROWS. */
int sys_menu_ui_build(const sys_ui *u, menu_row *out, const char **heading);

#endif
