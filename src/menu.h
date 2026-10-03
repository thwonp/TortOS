/* SPDX-License-Identifier: MIT */
/* What a menu row is. No SDL, no renderer, nothing device-specific.
 *
 * Separated from main.c on purpose. ADR-0001 says a screen's build function
 * must be callable with no renderer and no device, so that "given this state,
 * what does this menu contain" is answerable by `make check` - and that is only
 * true if the types it needs can be included without dragging SDL in with them.
 *
 * The conventions, and the five defects that produced them: docs/menus.md.
 */
#ifndef TORTOS_MENU_H
#define TORTOS_MENU_H

#include <stdbool.h>

typedef struct {
	const char *label;
	const char *value;  /* the right column, or NULL for a row that is only a label */
	bool live;          /* false: a placeholder, drawn quiet and doing nothing */
} menu_row;

/* A row that is only a rule, separating a list from a footer that is not part
 * of it. Drawn with the same bar the heading uses, so the two cannot drift
 * apart - which is why it belongs to the menu code and not to a screen painting
 * its own line.
 *
 * Not live, so any screen that steps over dead rows skips it for free. */
#define MENU_RULE ((menu_row){ NULL, NULL, false })

/* A note under the rule: a key legend, a count, a caption. Centered, because it
 * describes the list rather than being an item in it - left-aligned it read as
 * one more row you had failed to be able to select.
 *
 * Marked by a sentinel in `value` rather than a new struct field, so every
 * existing { label, value, live } initializer stays valid.
 *
 * A BODY row is the same row LEFT-ALIGNED: one line of a wrapped paragraph
 * rather than a caption. Centering is right for a caption and wrong for prose -
 * across the fifteen-odd lines of a game synopsis every line begins somewhere
 * different and the eye has to hunt for the start of each one. Everything else
 * about the two is identical, so ROW_IS_NOTE covers both and only the draw
 * asks which it has. */
#define MENU_NOTE_MARK ((const char *)1)
#define MENU_BODY_MARK ((const char *)2)
#define MENU_NOTE(s)   ((menu_row){ (s), MENU_NOTE_MARK, false })
#define MENU_BODY(s)   ((menu_row){ (s), MENU_BODY_MARK, false })
#define ROW_IS_BODY(r) ((r).value == MENU_BODY_MARK)
#define ROW_IS_NOTE(r) ((r).value == MENU_NOTE_MARK || \
                        (r).value == MENU_BODY_MARK)

/* A rule INSIDE a body, rather than the one that divides a list from its
 * footer. Drawn identically - same bar, same width - and it exists as its own
 * kind for one reason: MENU_RULE is what tells a scrolling card where its
 * pinned footer starts, so a rule used as punctuation in the middle of some
 * prose would pin everything after it.
 *
 * What it punctuates is the seam of a synopsis that scrolls through and
 * repeats: text, air, rule, air, and the text again. */
#define MENU_HR_MARK ((const char *)3)
#define MENU_HR      ((menu_row){ NULL, MENU_HR_MARK, false })
#define ROW_IS_HR(r) ((r).label == NULL && (r).value == MENU_HR_MARK)

/* Move to the next live row in `dir` (+1 or -1), or stay put if none is.
 *
 * Bounded, deliberately. A menu can legitimately be all dead rows - "No
 * networks found" under a rule is exactly that - and the do/while this
 * replaced would spin forever on one. Pure, so tools/menu-check.c holds it
 * to that. */
int menu_step_sel(const menu_row *rows, int n, int sel, int dir);

/* Which row a scrolling list should draw first, given where it drew first last
 * time. `vis` is how many rows fit; `first` is the previous answer, which is
 * kept unless the cursor has moved out of it.
 *
 * Here rather than inside menu_draw because the rule has a case that only shows
 * up after a particular walk through a particular list - down to the bottom and
 * back - which is exactly the shape of thing a check can hold and a person
 * looking at one frame cannot. Pure, so tools/menu-check.c can walk it. */
int menu_window_first(const menu_row *rows, int n, int sel, int vis, int first);

/* Whether a row too wide for its panel SCROLLS rather than being cut short.
 *
 * Two reasons to move: the cursor is on it, or the cursor can never get to it -
 * because a row nobody can select has no other way of showing text that does
 * not fit. `visits_all` is how a caller says the second reason does not apply
 * to its list, which is true wherever `live` means something other than
 * selectable.
 *
 * Pure, and here rather than inside the renderer, because the last time this
 * rule quietly meant something else nobody could see it: the achievement list
 * scrolled a title or cut it depending on whether the player had EARNED it. */
bool menu_row_moves(menu_row row, bool selected, bool visits_all);

#endif
