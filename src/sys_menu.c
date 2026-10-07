/* SPDX-License-Identifier: MIT */
/* What the two MENU-button menus contain. See src/sys_menu.h for why this is
 * a file of its own, and docs/menus.md for the rules the rows follow.
 *
 * No SDL, no device reads, no forks. Everything this needs arrives in sys_ui. */
#include <stdio.h>

#include "sys_menu.h"

void sys_menu_auto_off_label(int seconds, char *out, size_t n)
{
	if (seconds <= 0)       snprintf(out, n, "never");
	else if (seconds < 120) snprintf(out, n, "%ds", seconds);
	else                    snprintf(out, n, "%dm", seconds / 60);
}

/* Three rows say the same thing when the radio is down, and they say it in
 * the value column rather than by vanishing: a row that disappears when it
 * cannot be used teaches nobody why. */
#define NEEDS_WIFI(on) ((on) ? NULL : "needs Wi-Fi")

int sys_menu_muse_rows(bool books, bool both, sm_muse_row *out)
{
	int n = 0;

	out[n++] = SMM_COUNT;
	if (both) out[n++] = SMM_SHOW;
	out[n++] = SMM_SORT;
	if (!books) out[n++] = SMM_ART;
	out[n++] = SMM_RESCAN;
	out[n++] = SMM_SETTINGS;
	return n;
}

int sys_menu_build(const sys_ui *u, menu_row *out, menu_bufs *b,
                   const char **heading)
{
	bool net = u->wifi == WIFI_CONNECTED;

	if (u->games) {
		snprintf(b->a, sizeof b->a, "%d", u->game_count);
		snprintf(b->b, sizeof b->b, "%s", u->sys_core ? u->sys_core : "");
		*heading = u->sys_name;
		/* A shelf of favorites gets the two rows that are true of it and
		 * nothing else - not even the enum's order, since Core sits between
		 * them and it has none. See SM_FAV_ROWS. */
		if (u->fav) {
			out[0] = (menu_row){ "Games",   b->a,                      false };
			out[1] = (menu_row){ "Sort By", u->sort ? u->sort : "Name", true };
			return SM_FAV_ROWS;
		}
		if (u->muse) {
			sm_muse_row ids[SM_MUSE_ROWS];
			int k, n = sys_menu_muse_rows(u->muse_books, u->muse_both, ids);

			for (k = 0; k < n; k++)
				switch (ids[k]) {
				case SMM_COUNT:
					out[k] = (menu_row){ u->muse_books ? "Books" : "Albums",
					                     b->a, false };
					break;
				case SMM_SHOW:
					out[k] = (menu_row){ "Show",
					                     u->muse_books ? "Audiobooks" : "Music", true };
					break;
				case SMM_SORT:
					out[k] = (menu_row){ "Sort By", u->sort ? u->sort
					                     : u->muse_books ? "Author" : "Artist", true };
					break;
				case SMM_ART:
					out[k] = (menu_row){ "Album Art", NEEDS_WIFI(net), net };
					break;
				case SMM_RESCAN:
					out[k] = (menu_row){ "Rescan Folder", NULL, true };
					break;
				case SMM_SETTINGS:
					out[k] = (menu_row){ "Muse Settings", NULL, true };
					break;
				}
			return n;
		}
		out[SM_GAMES]   = (menu_row){ "Games",         b->a,        false };
		out[SM_CORE]    = (menu_row){ "Core", u->engine ? u->engine : b->b,
		                              u->engine != NULL };
		out[SM_SORT]    = (menu_row){ "Sort By",
		                              u->sort ? u->sort : "Name", true  };
		out[SM_DISPLAY] = (menu_row){ "Display Mode",  u->dmode,    true  };
		/* Button Mapping is out until there is something behind it. Diatom
		 * supplies core button labels, so the hook is real - but a dead row
		 * in a menu of live ones is a promise the launcher is not keeping,
		 * and it has sat there unwired longer than it was ever going to be
		 * worth. Put the enum entry back with it when it is built.
		 *
		 * Show went with it on 2026-09-17, and it was the weaker of the two:
		 * Button Mapping at least named something the emulator can do, where
		 * Show read "All games" because that string was written here and
		 * nothing could ever change it. Games and Core are dead rows too and
		 * they stay, because what they report is true - the shelf's count and
		 * the core that will run it. Except on PICO-8's shelf, where Core is
		 * the switch between fake08 and the owner's pico8_64. The filter Show promised is on the shelf
		 * already: Y marks a favorite and Favorites is its own shelf. */
		/* out[SM_BUTTONS] = (menu_row){ "Button Mapping", NULL,    false }; */
		/* Just this system. Needs the network like its counterpart in the
		 * TortOS menu, and says so rather than opening a screen that can only
		 * report the same thing. */
		out[SM_BOXART]  = (menu_row){ "Box Art",       NEEDS_WIFI(net), net };
		/* Live now that there is something behind it. It rescans the whole
		 * card rather than this one folder - the work is three directory reads
		 * and the shared parts (hiding a system that emptied, rebuilding
		 * Favorites) have to run anyway - but the folder you are standing in
		 * is the one you came here to refresh, so the name still describes
		 * what you asked for. */
		out[SM_RESCAN]  = (menu_row){ "Rescan Folder", NULL,        true  };
		return SM_ROWS;
	}

	*heading = "plorpOS";
	/* The network's name, not its address. A settings row should say what the
	 * setting IS; the address is a fact about the machine and lives on the
	 * About page with the other ones. */
	if (net && u->ssid && u->ssid[0])
		snprintf(b->b, sizeof b->b, "%s", u->ssid);
	else
		snprintf(b->b, sizeof b->b, "%s",
		         u->wifi == WIFI_CONNECTING ? "connecting" :
		         u->wifi == WIFI_IDLE       ? "not connected" : "off");
	/* The Wi-Fi screen, named for everything on it now: the radio, and the
	 * rows that need it - Cheevos and Over The Hare (plorpos-z0d.1). The
	 * value is still the radio's, since that is what decides the rest. */
	out[PM_WIFI]         = (menu_row){ "Wi-Fi Services", b->b, true };
	out[PM_BT]           = (menu_row){ "Bluetooth",
	                                   u->bt_name ? u->bt_name : "not connected",
	                                   true  };
	/* Where the system's sound goes - not Diatom's, which is why the label says
	 * neither "game" nor "emulator": the audiobook and music player will read
	 * the same setting. Diatom's ADR-0029, and src/audioout.c for the rule.
	 *
	 * Auto names what it resolved to, because a row that reads only "Auto"
	 * makes the player guess which of three places they are about to hear. */
	if (u->audio_policy == AOUT_AUTO)
		snprintf(b->d, sizeof b->d, "auto (%s)", aout_dest_name(u->audio_dest));
	else
		snprintf(b->d, sizeof b->d, "%s", aout_dest_name(u->audio_dest));
	out[PM_AUDIO]        = (menu_row){ "Audio Output", b->d, true };
	out[PM_STATS]        = (menu_row){ "Play Time",  NULL,      true  };
	out[PM_SYSTEM]       = (menu_row){ "System Settings", NULL, true  };
	out[PM_UI]           = (menu_row){ "UI Settings", NULL,     true  };
	/* Box Art and the ScreenScraper account moved under here with gamelist
	 * import (TortOS-mh0) - see sys_menu_scraping_build. Always live: the
	 * import needs no network, whatever the other two do. */
	out[PM_SCRAPING]     = (menu_row){ "Scraping",  NULL,      true  };
	/* What every button does, per screen. Needs nothing of the device, which
	 * is the point: it is the page you reach when the thing you have forgotten
	 * is which button opens Muse. See src/controls.h. */
	out[PM_CONTROLS]     = (menu_row){ "Controls",    NULL,   true  };
	out[PM_ABOUT]        = (menu_row){ "About",       NULL,   true  };
	return PM_ROWS;
}

int sys_menu_system_build(const sys_ui *u, menu_row *out, menu_bufs *b,
                          const char **heading)
{
	*heading = "System Settings";
	/* NextUI's two sleep rows, same meaning: Auto Sleep is its "Screen
	 * timeout" (idle until light sleep - screen off, CPU awake), Suspend
	 * Timeout its "Suspend timeout" (how long light sleep waits unwoken
	 * before real suspend, whether a tap or idle began it). Auto Off is
	 * TortOS's own: idle until a full shutdown, resume-into-game putting you
	 * back. It and Auto Sleep are mutually exclusive - main.c's
	 * ST_SLEEP/ST_AUTO_OFF handling - so at most one of those two labels is
	 * ever a real interval, and why the two sit together, Auto Off on top. */
	/* Date & Time leads: setup, where the timers are settings (upstream
	 * b3b4483, which put it in the top menu; ours is regrouped - z0d.1). */
	out[ST_CLOCK]    = (menu_row){ "Date & Time", u->clock, true };
	sys_menu_auto_off_label(u->auto_poweroff, b->a, sizeof b->a);
	out[ST_AUTO_OFF] = (menu_row){ "Auto Off",        b->a, true };
	sys_menu_auto_off_label(u->auto_off, b->c, sizeof b->c);
	out[ST_SLEEP]    = (menu_row){ "Auto Sleep",      b->c, true };
	sys_menu_auto_off_label(u->suspend_timeout, b->e, sizeof b->e);
	out[ST_SUSPEND]  = (menu_row){ "Suspend Timeout", b->e, true };
	/* plorpos-gkd.86.3: the percentage in a disc, top right, on every
	 * screen but the game's. Off, the disc is only the low-battery dot. */
	out[ST_BATTPCT]  = (menu_row){ "Battery Percentage", u->batt_pct ? "on" : "off", true };
	/* TortOS-ib9: what the side switch does. Button Lock is an iPod's hold
	 * switch, and only while music plays with the screen off - music_dark.
	 * "muse" in the value says so (TortOS-mhw). */
#if !defined(PLATFORM_GKD) && !defined(PLATFORM_H700)   /* no switch on the H700s */
	out[ST_MUTESW]   = (menu_row){ "Mute Switch",
	                               u->mute_lock ? "muse button lock" : "mute", true };
#endif
	return ST_ROWS;
}

int sys_menu_ui_build(const sys_ui *u, menu_row *out, const char **heading)
{
	*heading = "UI Settings";
	/* Both change how the shelf looks and nothing about what is on it. Text
	 * Size stood here until the band it offered turned out to be too narrow
	 * to matter - src/ui.c. */
	out[US_THEME] = (menu_row){ "UI Theme",     u->cards,     true };
	out[US_DIR]   = (menu_row){ "UI Direction", u->cards_dir, true };
	return US_ROWS;
}

int sys_menu_scraping_build(const sys_ui *u, menu_row *out, const char **heading)
{
	bool net = u->wifi == WIFI_CONNECTED;

	*heading = "Scraping";
	out[SC_BOXART] = (menu_row){ "Box Art", NEEDS_WIFI(net), net };
	/* An account, named by the service it belongs to, reading "sign in"
	 * until there is one - as Cheevos does in the menu above.
	 *
	 * Dead when the build has no developer key, because then there is nothing
	 * to sign into - and saying so is better than a row that opens a keyboard
	 * and refuses whatever is typed into it. */
	out[SC_SS]     = (menu_row){ "ScreenScraper",
	                             !u->ss_have ? "not in this build"
	                             : u->ss_in ? u->ss_name : "sign in",
	                             u->ss_have };
	/* Reads Roms/<system>/gamelist.xml off the card: no network, no account. */
	out[SC_IMPORT] = (menu_row){ "Import gamelist.xml metadata", NULL, true };
	return SC_ROWS;
}
