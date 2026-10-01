#ifndef TORTOS_GAMELIST_H
#define TORTOS_GAMELIST_H

/* EmulationStation gamelist.xml / miyoogamelist.xml, read on the device
 * (TortOS-mh0). The field mapping is tools/gamelist-import.py's, which does the
 * same job from a computer; keep the two in step - check-gamelist compares
 * them on the same file.
 *
 * Links src/db.c and NOT SDL, so it can be checked on a host. */

#include <stdbool.h>
#include <stddef.h>

#include "config.h"
#include "db.h"

/* One <game>: its ROM's file name (the basename of <path>) and its metadata. */
typedef void (*gl_game_fn)(void *ctx, const char *file, const game_meta *m);

/* Calls fn for each <game> with a usable <path>. Returns how many, or -1 when
 * the text has no <gameList> at all. */
int gl_parse(const char *xml, size_t len, gl_game_fn fn, void *ctx);

typedef struct {
	int lists;        /* gamelists found and read */
	int wrote;        /* rows written */
	int skipped;      /* games that already had a row, left alone */
	int bad;          /* rows the database refused */
	int unreadable;   /* gamelists that could not be read or were not one */
	char first_unreadable[CFG_STR];  /* a folder name, for the result panel */
} gl_result;

/* Every system's Roms/<folder>/gamelist.xml, or miyoogamelist.xml when there
 * is no gamelist.xml. A game that already has a row is skipped unless
 * overwrite, apart from a title it lacks (db_game_import). */
void gl_import(db *d, const char *roms, const systems_cfg *sys, bool overwrite,
               gl_result *r);

#endif
