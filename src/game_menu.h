/* What the game info screen contains, as rows rather than as pixels.
 *
 * Split out of main.c under ADR-0001 so a check can ask what this screen says
 * about a game without a card, a renderer or a device. Nothing here may
 * include SDL - tools/menu-check.c links it without one.
 *
 * gi_gather stays in main.c, because reading a card is exactly the half that
 * does not belong here. */
#ifndef GAME_MENU_H
#define GAME_MENU_H

#include <stdbool.h>

#include "menu.h"
#include "library.h"

/* Save slots. Six the player picks from, plus the resume slot the exit funnel
 * owns - which is NAMED "auto" and is not one of the numbered ones.
 *
 * Here rather than in main.c so a check can see it. The carousel used to map
 * its Auto entry to a hardcoded 9, MinUI's number for the same idea, and when
 * the resume slot was renamed on 2026-08-27 every use of the CONSTANT was
 * updated and that literal was not. Loading Auto from the in-game menu asked
 * for a `.9.state` for ten days, and the state_rejected that came back was
 * misread as a dead emulator - which started a second one over the top of the
 * first and wedged the display. A number nothing could test. */
#define GM_SLOTS  6
#define SLOT_AUTO (GM_SLOTS + 1)

/* The carousel shows Auto first, then the numbered slots. This is the only
 * place that mapping is written down. */
static inline int gm_slot_at(int carousel_index)
{
	return carousel_index == 0 ? SLOT_AUTO : carousel_index;
}

/* Which slot the Save carousel opens on: the first EMPTY one, or the LAST slot
 * when every slot is taken.
 *
 * The menu is A to open Save and A to choose, so a double-tap used to
 * overwrite slot 1 every time - the one press people make without looking,
 * aimed at the slot most likely to hold something they wanted.
 *
 * The full case landed on the OLDEST save first, on the reasoning that least
 * recent means least wanted. That is backwards. A save is often old BECAUSE it
 * is deliberately kept - the finished playthrough, the one before a point of
 * no return, the thing you set aside and stopped touching. Recency measures
 * activity, not value, and position measures intent: people put what matters
 * in a low slot and let scratch pile up in high ones.
 *
 * The last slot is also PREDICTABLE, which the oldest never is. "When full it
 * reuses the last slot" is a rule you can learn and plan around; "it reuses
 * whichever is oldest" moves the target and can land on the one save you were
 * protecting. Slot 1 stays five presses away and never chosen by accident.
 *
 * `have` is indexed 1..GM_SLOTS; index 0 is the Auto slot and Save cannot aim
 * at it. Pure, so tools/menu-check.c can enumerate the cases. */
static inline int gm_save_slot(const bool *have)
{
	int i;

	for (i = 1; i <= GM_SLOTS; i++)
		if (!have[i]) return i;
	return GM_SLOTS;
}

#define GI_MAX 6          /* four facts at most, two actions */

/* The genre line, from the scrape. Sized from the card rather than guessed:
 * measured 2026-09-17 over 1,704 rows, the whole comma-separated value has a
 * median of 15 characters, a 90th percentile of 36 and a longest of 63 - one
 * game in three carries a second genre and one in forty a third. 80 holds all
 * of them with room, which is what stops this being the row that truncates. */
#define GI_GENRE_MAX 80

/* Everything worth saying about one game, gathered once.
 *
 * Gathered rather than watched: this screen is a still. It reads the card, the
 * save directory and the achievement set when it opens, and again after an
 * action changes one of them. Polling would mean a stat storm every frame for
 * numbers that only move when the player does something. */
typedef struct {
	char  cheevos[64];
	char  year[8];         /* from the scrape, empty when it said nothing */
	char  genre[GI_GENRE_MAX];   /* the same, comma separated */
	bool  has_cheevos;     /* there is a set to open, not just a count to read */
	bool  has_art;         /* chooses the action's label; no longer a row */
	/* Whether this game has a row in the card's games table at all, and
	 * whether that row carries a synopsis. Two flags rather than one, because
	 * they answer different questions: a game nobody has scraped shows neither
	 * row, and a game that was scraped and came back without prose says so
	 * rather than looking unscraped. */
	bool  scraped;
	bool  has_synopsis;
	bool  deletable;       /* a file on the card: everything but Splore */
} game_info;

/* `net` is whether there is a network, asked by the caller and passed in: the
 * two rows that need one say so instead of quietly doing nothing. */
int gi_rows(menu_row *out, const game_info *gi, bool net);

/* ---------- the in-game menu ---------------------------------------------- */

/* No Sleep row since plorpos-z0d.2: it came before a tap of POWER slept, and
 * the tap does the same thing - sleep_cycle, the game checkpointed first -
 * without opening a menu to get there. */

typedef enum {
	GM_CONTINUE, GM_SAVE, GM_LOAD, GM_DISPLAY, GM_SHADER, GM_PALETTE, GM_CHEEVOS,
	GM_HOTKEYS, GM_RESET, GM_QUIT,
	GM_ROWS
} gm_row;

/* What the in-game menu needs to know, gathered by the caller. */
typedef struct {
	const char *dmode;    /* the display mode's label */
	const char *shader;   /* the shader's name (plorpos-gkd.72.4) */
	bool        shaders;  /* a list to pick from - the GKD's, never a Brick's */
	const char *palette;  /* this game's palette; NULL: not a Game Boy game (plorpos-gkd.76) */
	int         earned;
	int         total;    /* 0: this game has no achievement set */
} gm_ui;

/* Where the Cheevos row's text lives; the caller owns it, because a row holds
 * a pointer rather than a copy. */
typedef struct { char cheevos[24]; } gm_bufs;

int gm_rows(const gm_ui *u, menu_row *out, gm_bufs *b);

/* Native PICO-8's menu (plorpos-gkd.50.16). The game is another process,
 * frozen under it, so only what can be done to a process from outside: no
 * states, and no Display - its flags are read once, at startup. */
typedef enum { GMN_CONTINUE, GMN_RESET, GMN_SPLORE, GMN_QUIT, GMN_ROWS } gmn_row;

/* Splore ends the cart and opens Splore (plorpos-gkd.50.21); greyed in
 * Splore itself, where Reset is the same thing. */
/* The rows gm_rows made, less the two that can never do anything here: Shader
 * with no list (every Brick) and Palette on a game that is not a Game Boy
 * game. Compacted in place; ids[i] is the gm_row that row i now shows, which
 * is how a selection is read back. Cheevos stays when dead: its "none" answers
 * the question the menu was opened to ask. Returns the new count. */
int gm_compact(menu_row *rows, int n, int *ids);

int gm_native_rows(bool in_splore, menu_row *out);

#endif
