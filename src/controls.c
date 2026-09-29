/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* See controls.h. The pages, and nothing else. */
#include "controls.h"

#include <stddef.h>

static const char *const NAMES[CTL_PAGES] = {
	[CTL_MOVING]   = "Moving",
	[CTL_SHELF]    = "On the shelf",
	[CTL_GAME]     = "In a game",
	[CTL_MUSE]     = "In Muse",
	[CTL_ANYWHERE] = "Anywhere",
};

const char *ctl_page_name(ctl_page p)
{
	return NAMES[(unsigned)p < CTL_PAGES ? p : CTL_MOVING];
}

/* A PAIR OF BUTTONS IS WRITTEN "L1/R1", with no air around the slash. Eric's,
 * 2026-09-20: spaced out it reads as two separate things rather than one
 * control with two ends, and it is not how he writes them.
 *
 * EVERY PAGE FITS, so none of them moves while it is being read.
 *
 * The panel holds seven rows under a one-line heading, and the last of the
 * seven is the footer. Six lines of content, measured against the shipped text
 * size 2026-09-19: at eight the footer was drawn half off the panel, and a
 * cursorless list that does not fit scrolls itself, which is right for a
 * synopsis and wrong for a table you are trying to read a button off.
 *
 * That ceiling is why the shelf is two pages. Its seven buttons split where
 * the hand does: the pad and the shoulders get you around, and the four face
 * buttons do something to what you have landed on. */
static int moving(ctl_dir dir, menu_row *out)
{
	int n = 0;

	/* Which axis does what is the whole of UI Direction. Cubic takes both -
	 * systems on one, games on the other - so it has no letter jump. */
	if (dir == CTL_CUBIC) {
		out[n++] = (menu_row){ "Up/Down",    "Console",        false };
		out[n++] = (menu_row){ "Left/Right", "Game",           false };
	} else if (dir == CTL_VERTICAL) {
		out[n++] = (menu_row){ "Up/Down",    "Move",           false };
		out[n++] = (menu_row){ "Left/Right", "Jump by letter", false };
	} else {
		out[n++] = (menu_row){ "Left/Right", "Move",           false };
		out[n++] = (menu_row){ "Up/Down",    "Jump by letter", false };
	}
	out[n++] = (menu_row){ "L1/R1", "A screenful", false };
	return n;
}

static int shelf(ctl_dir dir, menu_row *out)
{
	int n = 0;

	out[n++] = (menu_row){ "A", dir == CTL_CUBIC ? "Start the game"
	                                             : "Open or play",   false };
	/* On the cube there is nowhere to go back to - the cube IS the shelf - so
	 * B is the console's menu there instead. */
	out[n++] = (menu_row){ "B", dir == CTL_CUBIC ? "Console's menu"
	                                             : "Back",           false };
	out[n++] = (menu_row){ "X", "Game details", false };
	out[n++] = (menu_row){ "Y", "Favorite",     false };
	return n;
}

static int game(menu_row *out)
{
	int n = 0;

	out[n++] = (menu_row){ "MENU",   "Pause and menu",     false };
	out[n++] = (menu_row){ "SELECT", "Muse, in that menu", false };
	out[n++] = (menu_row){ "X/Y",  "Turbo A/B *",        false };
	out[n++] = (menu_row){ "POWER",  "Tap sleep, hold off", false };
	/* The exception as a note rather than a longer row, because it is true of
	 * two systems out of eleven: on those two, X and Y are the pad's own
	 * buttons. Kept short - a note runs the width of the panel and is cut at
	 * about thirty characters. An asterisk on both ends, the row's value and
	 * this line, so the two read as a footnote and its mark rather than as two
	 * separate claims. Eric's, 2026-09-20. */
	out[n++] = MENU_NOTE("* Genesis and SNES: not turbo");
	return n;
}

static int muse(menu_row *out)
{
	int n = 0;

	/* A on Muse's SHELF opens an album, which the shelf page already says.
	 * These are the player's own buttons, the ones a shelf does not have. */
	out[n++] = (menu_row){ "A",          "Play or pause", false };
	out[n++] = (menu_row){ "L1/R1",      "Track",         false };
	/* Ten seconds a tap and further held - see seek_step in main.c. Said as
	 * what it does, since the numbers do not fit in a value. */
	out[n++] = (menu_row){ "Left/Right", "Seek, faster held", false };
	/* "Mode" rather than "Mode, on Now Playing": you press it on Now Playing,
	 * where the mark beside the track count changes in front of you. Eric's,
	 * 2026-09-20 - the rest of that sentence was saying what the screen was
	 * about to show anyway. "Music" since audiobooks, 2026-09-29: a book
	 * plays in order and Y does nothing there. */
	out[n++] = (menu_row){ "Y",          "Mode, music",   false };
	/* B is not here. It goes back in Muse exactly as it does everywhere else,
	 * and the shelf's page already says so - and with the rule above the
	 * footer this page has room for five, not six. */
	out[n++] = (menu_row){ "SELECT",     "Close Muse",    false };
	return n;
}

/* What the two brightness keys are printed as. main sets the Brick Pro's. */
const char *ctl_bright_keys = "F1/F2";

static int anywhere(menu_row *out)
{
	int n = 0;

	out[n++] = (menu_row){ "SELECT",        "Muse",       false };
	/* MENU is here rather than on a shelf page because it is the same promise
	 * wherever you are: the settings for whatever is in front of you. In a
	 * game it is a different menu, and that page says so. */
	out[n++] = (menu_row){ "MENU",          "Settings",   false };
	out[n++] = (menu_row){ "Volume rocker", "Sound",      false };
	out[n++] = (menu_row){ ctl_bright_keys, "Brightness", false };
	out[n++] = (menu_row){ "POWER",         "Tap sleep, hold off", false };
	return n;
}

int ctl_rows(ctl_page p, ctl_dir dir, menu_row *out)
{
	int n;

	switch (p) {
	case CTL_SHELF:    n = shelf(dir, out);  break;
	case CTL_GAME:     n = game(out);        break;
	case CTL_MUSE:     n = muse(out);        break;
	case CTL_ANYWHERE: n = anywhere(out);    break;
	default:           n = moving(dir, out); break;
	}
	/* The page is one of several and says so, the way Play Time's footer names
	 * the keys that change its window, under the same rule that divides every
	 * other list from its footer. Eric's, 2026-09-20. The rule is a row here
	 * but a thin one - a few pixels and the air around them, against a row's
	 * full height - which is what lets these pages keep it and still fit. */
	out[n++] = MENU_RULE;
	out[n++] = MENU_NOTE("Left/Right: more");
	return n;
}
