/* What the two MENU-button menus contain. See src/sys_menu.h for why this is
 * a file of its own, and docs/menus.md for the rules the rows follow.
 *
 * No SDL, no device reads, no forks. Everything this needs arrives in sys_ui. */
#include <stdio.h>

#include "sys_menu.h"

void sys_menu_auto_off_label(int seconds, char *out, size_t n)
{
	if (seconds <= 0)      snprintf(out, n, "never");
	else if (seconds < 60) snprintf(out, n, "%ds", seconds);
	else                   snprintf(out, n, "%dm", seconds / 60);
}

/* Three rows say the same thing when the radio is down, and they say it in
 * the value column rather than by vanishing: a row that disappears when it
 * cannot be used teaches nobody why. */
#define NEEDS_WIFI(on) ((on) ? NULL : "needs Wi-Fi")

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
			out[0] = (menu_row){ "Albums",        b->a,            false };
			out[1] = (menu_row){ "Sort By",
			                     u->sort ? u->sort : "Artist",     true  };
			out[2] = (menu_row){ "Album Art",     NEEDS_WIFI(net), net   };
			out[3] = (menu_row){ "Rescan Folder", NULL,            true  };
			return SM_MUSE_ROWS;
		}
		out[SM_GAMES]   = (menu_row){ "Games",         b->a,        false };
		out[SM_CORE]    = (menu_row){ "Core",          b->b,        false };
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
		 * the core that will run it. The filter Show promised is on the shelf
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

	*heading = "TortOS";
	/* The network's name, not its address. A settings row should say what the
	 * setting IS; the address is a fact about the machine and lives on the
	 * About page with the other ones. */
	if (net && u->ssid && u->ssid[0])
		snprintf(b->b, sizeof b->b, "%s", u->ssid);
	else
		snprintf(b->b, sizeof b->b, "%s",
		         u->wifi == WIFI_CONNECTING ? "connecting" :
		         u->wifi == WIFI_IDLE       ? "not connected" : "off");
	out[PM_WIFI]         = (menu_row){ "Wi-Fi",     b->b,      true  };
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
	/* Files onto and off the device over Wi-Fi: a small web server on the LAN
	 * that a phone or a laptop opens. Named for OTA, which is what everyone
	 * already calls this, and for the other half of the fable - the tortoise
	 * runs the system, the hare carries the files.
	 *
	 * Directly under Wi-Fi because it is useless without it, and reads as an
	 * answer to the row above rather than a separate idea. */
	out[PM_XFER]         = (menu_row){ "Over The Hare", NEEDS_WIFI(net), net };
	out[PM_STATS]        = (menu_row){ "Play Time",  NULL,      true  };
	/* Auto Sleep suspends to RAM (src/platform.c's plat_sleep) and resumes
	 * instantly; Auto Off powers down and relies on resume-into-game to put
	 * you back. Mutually exclusive, matching NextUI - see main.c's
	 * PM_SLEEP/PM_AUTO_OFF handling - so at most one of the two labels below
	 * is ever a real interval; the other always reads "never". */
	out[PM_SLEEP]        = (menu_row){ "Auto Sleep", b->c,     true  };
	sys_menu_auto_off_label(u->auto_off, b->c, sizeof b->c);
	out[PM_AUTO_OFF]     = (menu_row){ "Auto Off",  b->a,      true  };
	sys_menu_auto_off_label(u->auto_poweroff, b->a, sizeof b->a);
	/* Both change how the shelf looks and nothing about what is on it. They
	 * are what is left of that group: Text Size stood here until the band it
	 * offered turned out to be too narrow to matter - src/ui.c. */
	out[PM_THEME]        = (menu_row){ "UI Theme",  u->cards,     true };
	out[PM_DIR]          = (menu_row){ "UI Direction", u->cards_dir, true };
	out[PM_SCRAPE]       = (menu_row){ "Box Art",   NEEDS_WIFI(net), net };
	out[PM_ACHIEVEMENTS] = (menu_row){ "Cheevos",
	                                   u->ra_in ? u->ra_name : "sign in",
	                                   true };
	/* Beside Cheevos because it is the same kind of row: an account, named by
	 * the service it belongs to, reading "sign in" until there is one. Not
	 * beside Box Art, which is a job you run rather than a thing you are.
	 *
	 * Dead when the build has no developer key, because then there is nothing
	 * to sign into - and saying so is better than a row that opens a keyboard
	 * and refuses whatever is typed into it. */
	out[PM_SS]           = (menu_row){ "ScreenScraper",
	                                   !u->ss_have ? "not in this build"
	                                   : u->ss_in ? u->ss_name : "sign in",
	                                   u->ss_have };
	/* What every button does, per screen. Needs nothing of the device, which
	 * is the point: it is the page you reach when the thing you have forgotten
	 * is which button opens Muse. See src/controls.h. */
	out[PM_CONTROLS]     = (menu_row){ "Controls",    NULL,   true  };
	out[PM_ABOUT]        = (menu_row){ "About TortOS", NULL,   true  };
	return PM_ROWS;
}
