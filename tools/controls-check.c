/* SPDX-License-Identifier: MIT */
/* What does MENU > Controls actually say?
 *
 *     make check-controls
 *
 * The page exists to be the one place on the device that lists every button.
 * That is a property a machine can hold: every button the Brick has is named
 * on some page, no page names one twice, and no page invents one the device
 * does not have. A page that quietly dropped SELECT would still draw, still
 * look right, and be the one thing this feature was built to prevent.
 *
 * The rest is what the rows have to be to fit: a name and something it does,
 * kept short enough not to scroll.
 *
 * No SDL, no device. ADR-0001.
 */
#include <stdio.h>
#include <string.h>

#include "../src/controls.h"

static int fails;

static void ck(int cond, const char *what)
{
	if (!cond) { printf("  FAIL %s\n", what); fails++; }
}

/* Every button on the Brick that TortOS reads, by the name the pages use.
 * D-pad axes are named as pairs because that is how they are pressed, and
 * written with no air around the slash - "L1/R1", not "L1 / R1". */
static const char *const BUTTONS[] = {
	"Up/Down", "Left/Right", "A", "B", "X", "Y", "L1/R1", "X/Y/L2/R2",
	"MENU", "SELECT", "POWER", "Volume rocker", "F1/F2",
};
#define NBUTTONS ((int)(sizeof BUTTONS / sizeof BUTTONS[0]))

/* A ceiling on the value, so a page of rows sits still to be read.
 *
 * Not a measurement of these rows - a button's name is short, so they have
 * more room than the two rows this is drawn from. It is the conservative end
 * of what has been seen on the 800px panel at the shipped text size:
 * "12 KB in / 41 MB out", 20 characters beside the 11 of "Transferred", runs
 * past it and scrolls; "auto (speaker)" beside "Audio Output" sits inside it.
 * Under this and a row is safe whatever it is beside; over it and the shot is
 * what settles it. */
#define VALUE_MAX 22

/* Rows of TEXT a page can have - its buttons, any note, and the footer - before
 * the panel runs out. The rule is not counted: it is a couple of pixels and the
 * air around them rather than a row's full height.
 *
 * Six, measured 2026-09-19 and again on 2026-09-20 with the rule in place.
 * Eight drew the footer half off the panel; seven with a rule left the last
 * row faded at the window's edge, which is a page that scrolls itself while
 * somebody is reading a button off it. */
#define PAGE_MAX 6

/* A note runs the panel's width and is cut at about here: "On Genesis and
 * SNES, X and Y are the pad's own buttons" showed thirty characters of
 * fifty-four in a shot on 2026-09-19. */
#define NOTE_MAX 30

static int is_button(const char *s)
{
	int i;
	for (i = 0; i < NBUTTONS; i++) if (!strcmp(BUTTONS[i], s)) return 1;
	return 0;
}

/* How many pages name this button, across every UI direction a page can be
 * drawn in. A button belongs to as many pages as it has meanings - A opens a
 * game on the shelf and pauses a track in Muse - so what matters is that it is
 * on at least one. Written as "exactly one" first, which failed on eight
 * buttons and was the check being wrong about the feature rather than the
 * other way round. */
static int pages_naming(const char *button)
{
	int p, d, seen = 0;

	for (p = 0; p < CTL_PAGES; p++) {
		int here = 0;

		for (d = CTL_HORIZONTAL; d <= CTL_VERTICAL; d++) {
			menu_row rows[CTL_MAX_ROWS];
			int n = ctl_rows((ctl_page)p, (ctl_dir)d, rows), i;

			for (i = 0; i < n; i++)
				if (rows[i].label && !ROW_IS_NOTE(rows[i]) &&
				    !strcmp(rows[i].label, button))
					here = 1;
		}
		seen += here;
	}
	return seen;
}

static void every_button_somewhere(void)
{
	char msg[128];
	int i;

	printf("every button is somewhere\n");
	for (i = 0; i < NBUTTONS; i++) {
		int seen = pages_naming(BUTTONS[i]);

		/* X / Y is the turbo pair and X and Y are themselves: the game page
		 * naming the pair does not excuse the shelf page from naming X. */
		snprintf(msg, sizeof msg, "%s is on no page", BUTTONS[i]);
		ck(seen >= 1, msg);
	}
}

/* And no page says the same button twice, which is how a row gets edited in
 * place and its old wording left underneath. */
static void no_page_repeats(void)
{
	char msg[160];
	int p, d;

	printf("no page says a button twice\n");
	for (p = 0; p < CTL_PAGES; p++)
		for (d = CTL_HORIZONTAL; d <= CTL_VERTICAL; d++) {
			menu_row rows[CTL_MAX_ROWS];
			int n = ctl_rows((ctl_page)p, (ctl_dir)d, rows), i, j;

			for (i = 0; i < n; i++) {
				if (!rows[i].label || ROW_IS_NOTE(rows[i])) continue;
				for (j = i + 1; j < n; j++) {
					if (!rows[j].label || ROW_IS_NOTE(rows[j])) continue;
					snprintf(msg, sizeof msg, "%s names %s twice",
					         ctl_page_name((ctl_page)p), rows[i].label);
					ck(strcmp(rows[i].label, rows[j].label) != 0, msg);
				}
			}
		}
}

static void rows_fit(void)
{
	char msg[160];
	int p, d;

	printf("rows that sit still\n");
	for (p = 0; p < CTL_PAGES; p++)
		for (d = CTL_HORIZONTAL; d <= CTL_VERTICAL; d++) {
			menu_row rows[CTL_MAX_ROWS];
			int n = ctl_rows((ctl_page)p, (ctl_dir)d, rows), i;
			int rules = 0, notes = 0;

			snprintf(msg, sizeof msg, "%s: %d rows, room for %d",
			         ctl_page_name((ctl_page)p), n, CTL_MAX_ROWS);
			ck(n > 0 && n <= CTL_MAX_ROWS, msg);
			for (i = 0; i < n; i++) {
				if (!rows[i].label) { rules++; continue; }
				if (ROW_IS_NOTE(rows[i])) {
					notes++;
					snprintf(msg, sizeof msg,
					         "%s: a note of %d characters, cut after %d",
					         ctl_page_name((ctl_page)p),
					         (int)strlen(rows[i].label), NOTE_MAX);
					ck((int)strlen(rows[i].label) <= NOTE_MAX, msg);
					continue;
				}
				snprintf(msg, sizeof msg, "%s: \"%s\" is not a button",
				         ctl_page_name((ctl_page)p), rows[i].label);
				ck(is_button(rows[i].label), msg);
				snprintf(msg, sizeof msg, "%s: \"%s\" says nothing",
				         ctl_page_name((ctl_page)p), rows[i].label);
				ck(rows[i].value != NULL, msg);
				if (!rows[i].value) continue;
				snprintf(msg, sizeof msg,
				         "%s: \"%s\" value is %d characters, over %d and it scrolls",
				         ctl_page_name((ctl_page)p), rows[i].label,
				         (int)strlen(rows[i].value), VALUE_MAX);
				ck((int)strlen(rows[i].value) <= VALUE_MAX, msg);
				/* Nothing here is selectable: it is a page to read, and a
				 * highlight on a row that does nothing when pressed is the
				 * thing docs/menus.md forbids. */
				snprintf(msg, sizeof msg, "%s: \"%s\" is live",
				         ctl_page_name((ctl_page)p), rows[i].label);
				ck(!rows[i].live, msg);
			}
			snprintf(msg, sizeof msg, "%s: one rule and a footer, got %d and %d",
			         ctl_page_name((ctl_page)p), rules, notes);
			ck(rules == 1 && notes >= 1, msg);
			/* The panel's own ceiling. Over this and the page scrolls
			 * itself while somebody is reading it - measured 2026-09-19,
			 * where eight rows drew the footer half off the panel. */
			snprintf(msg, sizeof msg, "%s: %d rows of text, and %d fit the panel",
			         ctl_page_name((ctl_page)p), n - rules, PAGE_MAX);
			ck(n - rules <= PAGE_MAX, msg);
		}
}

/* UI Direction is the whole reason the shelf page takes one: the axes swap. */
static void direction_changes_the_shelf(void)
{
	menu_row h[CTL_MAX_ROWS], v[CTL_MAX_ROWS];
	int nh = ctl_rows(CTL_MOVING, CTL_HORIZONTAL, h);
	int nv = ctl_rows(CTL_MOVING, CTL_VERTICAL, v);

	printf("the moving page follows UI Direction\n");
	ck(nh == nv, "horizontal and vertical list the same buttons");
	ck(!strcmp(h[0].label, "Left/Right") && !strcmp(h[0].value, "Move"),
	   "horizontal moves along the row");
	ck(!strcmp(v[0].label, "Up/Down") && !strcmp(v[0].value, "Move"),
	   "vertical moves up and down");
	ck(!strcmp(h[1].value, "Jump by letter") && !strcmp(v[1].value, "Jump by letter"),
	   "and the other axis jumps by letter");
}

static void pages_are_named(void)
{
	int p, q;

	printf("the pages\n");
	ck(CTL_PAGES == 5, "five pages");
	for (p = 0; p < CTL_PAGES; p++) {
		ck(ctl_page_name((ctl_page)p) && ctl_page_name((ctl_page)p)[0],
		   "every page is named");
		for (q = p + 1; q < CTL_PAGES; q++)
			ck(strcmp(ctl_page_name((ctl_page)p), ctl_page_name((ctl_page)q)) != 0,
			   "two pages share a name");
	}
	/* Out of range reads as the first page rather than past the table. */
	ck(!strcmp(ctl_page_name((ctl_page)CTL_PAGES), ctl_page_name(CTL_MOVING)),
	   "a page number out of range is the first one");
}

int main(void)
{
	printf("controls: what MENU > Controls says\n");
	pages_are_named();
	every_button_somewhere();
	no_page_repeats();
	rows_fit();
	direction_changes_the_shelf();

	if (fails) {
		printf("\n%d failure%s\n", fails, fails == 1 ? "" : "s");
		return 1;
	}
	printf("ok\n");
	return 0;
}
