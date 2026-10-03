/* SPDX-License-Identifier: MIT */
/* The in-game Shader list (plorpos-gkd.72.4): res/shaders/shaders.cfg read
 * into entries, and an entry turned into the SETDISPLAY fields diatom takes
 * (diatom's ADR-0041 - the launcher owns the list, diatom only compiles what
 * it is sent). Split out of main.c under ADR-0001 so tools/shaderlist-check.c
 * drives the real parser with no SDL. */
#ifndef TORTOS_SHADERLIST_H
#define TORTOS_SHADERLIST_H

#include <stdbool.h>
#include <stddef.h>

#define SL_MAX  32
#define SL_NAME 32

typedef struct {
	char name[SL_NAME];   /* the menu's label and what shader.<TAG> stores */
	char passes[256];     /* "file:filter:scale[,...]", files without .glsl */
	char final[8];        /* nearest | linear */
} sl_entry;

/* e[0] is always None, whatever the file says or whether it exists. */
typedef struct {
	int      count;
	sl_entry e[SL_MAX];
} sl_list;

/* Entries from `path`, after None. A malformed line is reported on stderr
 * and left out rather than taken half-understood. Returns l->count. */
int sl_load(sl_list *l, const char *path);

/* The entry named `name`; 0 (None) for NULL, "" or a name no longer listed. */
int sl_find(const sl_list *l, const char *name);

/* "shader=none", or "shader=<dir>/<file>.glsl:filter:scale,...\tfinal=<f>"
 * for entry i, ready to follow "SETDISPLAY\t". False if it does not fit. */
bool sl_fields(const sl_list *l, int i, const char *dir, char *out, size_t cap);

#endif
