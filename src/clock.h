/* SPDX-License-Identifier: MIT */
/* Date & Time: the arithmetic and the words, with no SDL and no device.
 *
 * The GKD Pixel 2 has no network to take the time from, so its clock is
 * whatever it is set to here, and the hardware clock keeps it while the
 * device is off. The Brick sets its clock over Wi-Fi, but had no way to choose
 * a time zone either, so both get the same screen.
 *
 * Split out of main.c under ADR-0001, like sys_menu.c: tools/clock-check.c
 * links this file alone. Setting the clock itself is plat_clock_set. */
#ifndef CLOCK_H
#define CLOCK_H

#include <stddef.h>
#include <time.h>

/* The screen's rows that change the time, in order. Time Zone follows them
 * and is not a field of the time. */
typedef enum {
	CLK_YEAR, CLK_MONTH, CLK_DAY, CLK_HOUR, CLK_MINUTE, CLK_FIELDS
} clock_field;

/* `t` with one field moved by `d` (local time), and the seconds at zero:
 * setting a clock by hand starts the minute it is set on. Each field wraps
 * within the one above it - the minute past 59 is 0 of the same hour - so a
 * row changes only itself. A day past the end of the new month comes back to
 * its last day. The year is the exception: it stops at 2000 and 2099. */
time_t clock_step(time_t t, clock_field f, int d);

/* What the field's row says for `t`: "2026", "October", "3", "6 AM", "05". */
void clock_field_label(time_t t, clock_field f, char *out, size_t n);

/* The menu row's value: "Oct 3, 6:12 AM". 12-hour, Eric's call 2026-10-03. */
void clock_label(time_t t, char *out, size_t n);

/* The time zones on offer, by city. A short list rather than the four hundred
 * the zone database has, because stepping through those with left and right
 * would take minutes; every hour of offset anyone lives in is here. */
typedef struct {
	const char *id;     /* the zone database's name, which TZ takes */
	const char *city;   /* what the row shows */
} clock_zone;

extern const clock_zone CLOCK_ZONES[];
extern const int CLOCK_ZONE_COUNT;

/* The list's entry for a zone id, or -1 when it is not on the list (a setting
 * from before the list, or edited by hand). */
int clock_zone_find(const char *id);

/* The entry `d` steps from `id`'s, stopping at the ends. An id not on the list
 * steps onto it from New York, the shipped default. */
int clock_zone_step(const char *id, int d);

#endif
