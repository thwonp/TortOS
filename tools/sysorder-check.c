/* SPDX-License-Identifier: MIT */
/* Does System Order put the shipped systems.cfg in the orders it names?
 *
 *     make check-sysorder
 *
 * Reads config/systems.cfg itself, so a row added without a maker, or a
 * maker missing from the makers| line, fails here rather than landing at the
 * end of the Maker shelf on a device (plorpos-xpt.9).
 *
 * No SDL, no device: the shelf is just the config sorted by cfg_order_cmp.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/config.h"

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

static systems_cfg g_cfg;
static sys_order g_o;

static int cmp(const void *x, const void *y)
{
	return cfg_order_cmp(x, y, g_o);
}

/* The tags in order o, space-separated */
static void tags(sys_order o, char *out, size_t n)
{
	static systems_cfg c;

	c = g_cfg;
	g_o = o;
	qsort(c.systems, (size_t)c.count, sizeof c.systems[0], cmp);
	out[0] = '\0';
	for (int i = 0; i < c.count; i++)
		snprintf(out + strlen(out), n - strlen(out), "%s%s", i ? " " : "", c.systems[i].tag);
}

int main(void)
{
	char got[1024];

	if (!cfg_load_systems("config/systems.cfg", &g_cfg)) {
		printf("  FAIL: cannot read config/systems.cfg\n");
		return 1;
	}

	tags(SYS_ORDER_YEAR, got, sizeof got);
	CHECK(!strcmp(got, "PICO8 ARCADE NES SMS GB MD PCE GG NEOGEO SFC SEGACD 32X PS NGP GBC NGPC GBA"),
	      "Year is the file's order: %s", got);
	tags(SYS_ORDER_AZ, got, sizeof got);
	CHECK(!strcmp(got, "ARCADE GB GBA GBC GG MD SMS NEOGEO NGP NGPC NES PICO8 PS 32X SEGACD SFC PCE"),
	      "A-Z is by display name, case ignored: %s", got);
	tags(SYS_ORDER_MAKER, got, sizeof got);
	CHECK(!strcmp(got, "ARCADE NES GB SFC GBC GBA SMS MD GG SEGACD 32X PCE NEOGEO NGP NGPC PS PICO8"),
	      "Maker groups by the makers line, release order inside: %s", got);

	/* Every row's maker is on the line: the seven named there rank 0-6, and
	 * one that is not ranks past them. */
	for (int i = 0; i < g_cfg.count; i++)
		CHECK(g_cfg.systems[i].maker < 7, "%s has a maker on the makers line",
		      g_cfg.systems[i].tag);

	CHECK(cfg_order_index("maker") == SYS_ORDER_MAKER, "maker reads back");
	CHECK(cfg_order_index("bogus") == SYS_ORDER_YEAR, "an unknown id is Year");
	CHECK(cfg_order_index(NULL) == SYS_ORDER_YEAR, "no id is Year");

	if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
	printf("\nok: every check passed\n");
	return 0;
}
