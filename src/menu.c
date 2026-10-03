/* SPDX-License-Identifier: MIT */
/* The one piece of menu behavior that is pure enough to live away from the
 * renderer: where the cursor goes next. See src/menu.h. */
#include "menu.h"

int menu_step_sel(const menu_row *rows, int n, int sel, int dir)
{
	int i, k = sel;

	if (n <= 0) return 0;
	for (i = 0; i < n; i++) {
		k = (k + dir + n) % n;
		if (rows[k].live) return k;
	}
	return sel;
}

int menu_window_first(const menu_row *rows, int n, int sel, int vis, int first)
{
	int top = sel;

	if (n <= 0 || vis <= 0) return 0;
	if (vis >= n) return 0;
	if (sel < 0) return first;
	/* The dead rows directly above the cursor come with it.
	 *
	 * A dead row cannot be selected, so it can never pull the window up to
	 * itself - and a window that only moves when the CURSOR would leave it
	 * therefore leaves those rows behind for good. On the game info screen the
	 * two rows above the first selectable one are Cheevos and Year, which is
	 * the half of the screen people open it to read: walk down to Favorite
	 * once and they scroll off the top and never come back.
	 *
	 * Reported from the device 2026-09-16, the day the reordered rows were
	 * deployed. It is not a scrolling bug so much as a consequence of the
	 * cursor being the only thing that scrolls: as soon as a screen puts
	 * unselectable rows ABOVE every selectable one, there has to be something
	 * else that speaks for them, and the nearest cursor is it.
	 *
	 * The sel clamp below still wins when the dead run is taller than the
	 * window, because a cursor off the screen is worse than a fact off it. */
	while (top > 0 && !rows[top - 1].live) top--;
	if (top < first)                first = top;
	if (sel < first)                first = sel;
	if (sel > first + vis - 1)      first = sel - vis + 1;
	if (first > n - vis)            first = n - vis;
	if (first < 0)                  first = 0;
	return first;
}

bool menu_row_moves(menu_row row, bool selected, bool visits_all)
{
	return selected || (!row.live && !visits_all);
}
