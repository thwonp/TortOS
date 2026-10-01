/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include "titles.h"

#include <stdio.h>
#include <string.h>

#include "fbneodat.h"

typedef struct {
	game_list *l;
	int hits;
} from_db;

/* Linear over the shelf for each row: a shelf is hundreds of games and a
 * folder's rows about as many, at scan time, not per frame. */
static void take(void *ctx, const char *file, const char *title)
{
	from_db *c = ctx;
	int i;

	for (i = 0; i < c->l->count; i++) {
		game_entry *g = &c->l->items[i];

		if (strcmp(g->file, file)) continue;
		snprintf(g->title, sizeof g->title, "%s", title);
		c->hits++;
		return;
	}
}

void titles_apply(game_list *l, db *d, const char *folder, const char *dat)
{
	from_db c = { l, 0 };
	int i, sets = 0;

	if (!l || l->count <= 0) return;
	for (i = 0; dat && i < l->count; i++) {
		game_entry *g = &l->items[i];
		const char *desc = fbneo_desc(dat, g->name);

		if (!desc) continue;
		lib_title(desc, g->title, sizeof g->title);
		sets++;
	}
	/* After the table, so a gamelist's name wins over it. */
	db_game_titles(d, folder, take, &c);
	if (sets || c.hits)
		fprintf(stderr, "titles: %-16s %d from a gamelist, %d from FBNeo's list\n",
		        folder, c.hits, sets);
	lib_sort(l);
}
