/* SPDX-License-Identifier: MIT */
/* Does Date & Time step and read the way its rows say?
 *
 *     make check-clock
 *
 * Each row moves only itself, so the edges are the whole of it: the minute
 * after 59, the day after the month's last, January 31 moved to February, a
 * leap year, noon and midnight in 12-hour time, and the zone list's ends.
 * Run in New York, the shipped zone, so daylight saving is in play.
 *
 * No SDL, no device, and the clock is never set: time is a number this file
 * chooses.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/clock.h"

static int failures;

#define CHECK(cond, ...)                                                      \
	do {                                                                      \
		if (!(cond)) {                                                        \
			printf("  FAIL: ");                                               \
			printf(__VA_ARGS__);                                              \
			printf("\n        at %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
			failures++;                                                       \
		}                                                                     \
	} while (0)

/* Local time, from its parts */
static time_t at(int year, int mon, int day, int hour, int min)
{
	struct tm tm = { 0 };

	tm.tm_year = year - 1900; tm.tm_mon = mon - 1; tm.tm_mday = day;
	tm.tm_hour = hour; tm.tm_min = min; tm.tm_isdst = -1;
	return mktime(&tm);
}

static int part(time_t t, clock_field f)
{
	struct tm tm;

	localtime_r(&t, &tm);
	switch (f) {
	case CLK_YEAR:   return tm.tm_year + 1900;
	case CLK_MONTH:  return tm.tm_mon + 1;
	case CLK_DAY:    return tm.tm_mday;
	case CLK_HOUR:   return tm.tm_hour;
	case CLK_MINUTE: return tm.tm_min;
	default:         return tm.tm_sec;
	}
}

static int label_is(time_t t, clock_field f, const char *want)
{
	char got[32];

	clock_field_label(t, f, got, sizeof got);
	if (strcmp(got, want)) { printf("  (read \"%s\")\n", got); return 0; }
	return 1;
}

int main(void)
{
	char s[32];
	time_t t;

	setenv("TZ", "America/New_York", 1);
	tzset();

	printf("stepping:\n");
	t = clock_step(at(2026, 10, 3, 18, 59) + 30, CLK_MINUTE, +1);
	CHECK(part(t, CLK_MINUTE) == 0 && part(t, CLK_HOUR) == 18,
	      "the minute after 59 is 0 of the same hour");
	CHECK(part(t, -1) == 0, "and the seconds are zero");
	t = clock_step(at(2026, 10, 31, 9, 0), CLK_DAY, +1);
	CHECK(part(t, CLK_DAY) == 1 && part(t, CLK_MONTH) == 10,
	      "the day after the 31st is the 1st of the same month");
	t = clock_step(at(2026, 1, 31, 9, 0), CLK_MONTH, +1);
	CHECK(part(t, CLK_MONTH) == 2 && part(t, CLK_DAY) == 28,
	      "January 31 moved to February is February 28");
	t = clock_step(at(2028, 1, 31, 9, 0), CLK_MONTH, +1);
	CHECK(part(t, CLK_DAY) == 29, "and February 29 in a leap year");
	t = clock_step(at(2026, 12, 15, 9, 0), CLK_MONTH, +1);
	CHECK(part(t, CLK_MONTH) == 1 && part(t, CLK_YEAR) == 2026,
	      "December's next month is January of the same year");
	t = clock_step(at(2026, 10, 3, 23, 10), CLK_HOUR, +1);
	CHECK(part(t, CLK_HOUR) == 0 && part(t, CLK_DAY) == 3,
	      "the hour after 11 PM is midnight of the same day");
	t = clock_step(at(2026, 10, 3, 0, 10), CLK_HOUR, -1);
	CHECK(part(t, CLK_HOUR) == 23, "and back again");
	t = clock_step(at(2099, 6, 1, 9, 0), CLK_YEAR, +1);
	CHECK(part(t, CLK_YEAR) == 2099, "the year stops at 2099");
	t = clock_step(at(2000, 6, 1, 9, 0), CLK_YEAR, -1);
	CHECK(part(t, CLK_YEAR) == 2000, "and at 2000");
	t = clock_step(at(2018, 9, 4, 10, 43), CLK_YEAR, +8);
	CHECK(part(t, CLK_YEAR) == 2026 && part(t, CLK_MONTH) == 9,
	      "an unset Pixel's 2018 steps up to now");
	/* Daylight saving: July is EDT, January EST, and the hour shown stays
	 * the hour that was set */
	t = clock_step(at(2026, 7, 4, 15, 0), CLK_MONTH, +6);
	CHECK(part(t, CLK_MONTH) == 1 && part(t, CLK_HOUR) == 15,
	      "moving across daylight saving keeps the hour on the clock face");

	printf("words:\n");
	t = at(2026, 10, 3, 18, 5);
	CHECK(label_is(t, CLK_YEAR, "2026"), "the year");
	CHECK(label_is(t, CLK_MONTH, "October"), "the month by name");
	CHECK(label_is(t, CLK_DAY, "3"), "the day without a leading zero");
	CHECK(label_is(t, CLK_HOUR, "6 PM"), "the hour in 12-hour time");
	CHECK(label_is(t, CLK_MINUTE, "05"), "the minute with one");
	CHECK(label_is(at(2026, 10, 3, 0, 0), CLK_HOUR, "12 AM"), "midnight is 12 AM");
	CHECK(label_is(at(2026, 10, 3, 12, 0), CLK_HOUR, "12 PM"), "noon is 12 PM");
	clock_label(t, s, sizeof s);
	CHECK(!strcmp(s, "Oct 3, 6:05 PM"), "the menu row reads \"%s\"", s);
	clock_label(at(2026, 1, 9, 0, 30), s, sizeof s);
	CHECK(!strcmp(s, "Jan 9, 12:30 AM"), "and just after midnight \"%s\"", s);

	printf("zones:\n");
	CHECK(clock_zone_find("America/New_York") >= 0, "the shipped zone is on the list");
	CHECK(clock_zone_find("Mars/Olympus_Mons") < 0, "a zone not on it is not");
	CHECK(clock_zone_step("Mars/Olympus_Mons", +1) == clock_zone_find("America/New_York"),
	      "and steps onto it from New York");
	CHECK(clock_zone_step(CLOCK_ZONES[0].id, -1) == 0, "the west end stops");
	CHECK(clock_zone_step(CLOCK_ZONES[CLOCK_ZONE_COUNT - 1].id, +1) == CLOCK_ZONE_COUNT - 1,
	      "and so does the east");
	CHECK(!strcmp(CLOCK_ZONES[clock_zone_step("America/New_York", +1)].city, "Halifax"),
	      "one east of New York is Halifax");
	{
		/* West to east, by what each zone is on one fixed day. A zone this
		 * machine did not know would read as UTC and break the order, so
		 * this catches a misspelled id too. */
		int i;
		long prev = -86400;

		for (i = 0; i < CLOCK_ZONE_COUNT; i++) {
			struct tm tm;
			time_t noon = 1767268800;   /* 2026-01-01 12:00 UTC */

			setenv("TZ", CLOCK_ZONES[i].id, 1);
			tzset();
			localtime_r(&noon, &tm);
			CHECK(tm.tm_gmtoff >= prev, "%s is east of the zone before it",
			      CLOCK_ZONES[i].city);
			prev = tm.tm_gmtoff;
		}
	}

	if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
	printf("\nok: every check passed\n");
	return 0;
}
