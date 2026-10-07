/* SPDX-License-Identifier: MIT */
#ifndef TORTOS_CONFIG_H
#define TORTOS_CONFIG_H

#include <stdbool.h>

#define CFG_MAX_SYSTEMS 24   /* seventeen shipped, plus Favorites and Muse; 16 and then 20 were nearly exact */
#define CFG_STR 256

typedef struct {
	char name[CFG_STR];   /* display name, e.g. "TurboGrafx-16" */
	char folder[CFG_STR]; /* ROM folder under Roms/            */
	char core[CFG_STR];   /* core short name, e.g. "mednafen_pce_fast" */
	char tag[8];          /* saves, states and per-game configs hang off this,
	                       * so it is also the folder name under Saves/. Three
	                       * characters at most: this array is the limit, and a
	                       * longer tag is silently truncated into it. */
	char card[CFG_STR];   /* card art filename under TortOS/cards/ */
	unsigned accent;      /* 0xRRGGBB, tints the focus glow and the rail */
	char exts[CFG_STR];   /* which extensions in that folder are games; empty
	                       * means everything, which also means save files and
	                       * stray text files show up as games */
	/* Firmware a DISC image needs, resolved against Bios/. Empty for a system
	 * that needs none. Per-system and disc-only rather than per-system alone,
	 * because a PC Engine HuCard runs with no System Card and a CD does not -
	 * declaring it for the whole shelf would refuse the cartridges too. */
	char disc_bios[CFG_STR];
	/* Where the row sits for the System Order setting (plorpos-xpt.9): row is
	 * its line in systems.cfg, which is release order; maker is its maker's
	 * place on the makers| line, or past the end for a maker not on it. */
	int row;
	int maker;
} system_cfg;

typedef struct {
	system_cfg systems[CFG_MAX_SYSTEMS];
	int count;
} systems_cfg;

/* The orders the systems shelf can take. Year is the file's own order and
 * the default; Maker groups by the makers| line, each group in file order. */
typedef enum { SYS_ORDER_YEAR, SYS_ORDER_AZ, SYS_ORDER_MAKER, SYS_ORDER_COUNT } sys_order;

typedef struct { const char *id, *name; } sys_order_name;
static const sys_order_name SYS_ORDERS[SYS_ORDER_COUNT] = {
	{ "year",  "Year"  },
	{ "az",    "A-Z"   },
	{ "maker", "Maker" },
};
#define SYS_ORDER_DEFAULT "year"

bool cfg_load_systems(const char *path, systems_cfg *out);

/* <0, 0, >0 as qsort's: which of two systems comes first in order o.
 * Never 0 for two different rows, so any sort gives the same shelf. */
int cfg_order_cmp(const system_cfg *x, const system_cfg *y, sys_order o);

/* An id from the db to its order; anything unknown is Year. */
sys_order cfg_order_index(const char *id);

#endif
