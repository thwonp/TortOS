/* SPDX-License-Identifier: MIT AND PolyForm-Noncommercial-1.0.0
 *
 * TortOS - a custom firmware for the TrimUI Brick that plays eleven
 * 8-, 16- and 32-bit consoles.
 *
 * The whole design metric is speed. The launcher starts behind the boot
 * animation rather than after it, hands games to an emulator that is already
 * running rather than starting one, and never tears its own display down - so
 * coming back from a game is a frame, not a second and a half. Around that sit
 * RetroAchievements, box art the device fetches itself, a file server over
 * Wi-Fi, Bluetooth audio and play time - each one row in one menu, and none of
 * it in the way of starting a game.
 */
#include "atomic.h"
#include "cheevos.h"
#include "config.h"
#include "bt.h"
#include "btvol.h"
#include "bt_menu.h"
#include "db.h"
#include "gamelist.h"
#include "stats.h"
#include "sort.h"
#include "titles.h"
#include "coverflow.h"
#include "chdread.h"
#include "library.h"
#include "platform.h"
#include "texload.h"
#include "artshrink.h"
#include "artscrape.h"
#include "favorites.h"
#include "hare.h"
#include "idle.h"
#include "notice.h"
#include "rafetch.h"
#include "rahash.h"
#include "net.h"
#include "keyboard.h"
#include "logpack.h"
#include "wifi.h"
#include "audioout.h"
#include "menu.h"
#include "musec.h"
#include "museart.h"
#include "muselib.h"
#include "ss.h"
#include "ssrun.h"
#include "sys_menu.h"
#include "controls.h"
#include "cards.h"
#include "game_menu.h"
#include "hkbind.h"
#include "shaderlist.h"
#include "gbpal.h"
#include "ui.h"
#include "wifi_menu.h"

#include <SDL.h>
#include <SDL_image.h>
#include <dirent.h>
#include <errno.h>
#include <ftw.h>
#include <ctype.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define BATT_LOW_PCT   10   /* show the low-battery dot at or below this */
#define TEX_KEEP_NEAR   8   /* card textures kept around the cursor, in view  */
#define TEX_KEEP_FAR    4   /* ...and around the cursor of a system you left  */

/* The row is allowed to glide only across cards this policy keeps decoded. If
 * TEX_KEEP_NEAR ever drops below it, a long move would animate over cold cards
 * and stall exactly as it did before CF_WARM_CARDS existed. */
_Static_assert(TEX_KEEP_NEAR >= CF_WARM_CARDS,
               "the row glides across cards the texture policy must keep");

typedef enum { SCREEN_SYSTEMS, SCREEN_GAMES } screen_id;

typedef struct {
	game_list list;
	SDL_Texture **tex;
	int *tw, *th;
	float *cb;      /* where each card's art stops, for its reflection */
	int cursor;
	/* Index into DMODES. Zero-initialized like the rest of this struct, so
	 * whatever sits at index 0 is what an untouched card plays at - see the
	 * note on DMODES itself. */
	int dmode;
	/* Index into app.shaders, the GKD's in-game Shader list (plorpos-gkd.72.4).
	 * Zero is None, so an untouched system draws the plain picture. */
	int shader;
	/* PICO-8's shelf only: run its carts with the owner's pico8_64 rather
	 * than fake08. Zero is fake08, so a card nobody has touched - a new
	 * install included - plays in diatom. */
	bool native;
	/* Index into SORTS. Zero is by name, which is what every shelf was
	 * before this existed and what an untouched one still is. On Muse's
	 * shelf an ml_order instead, whose zero is by artist. */
	int sort;
	coverflow cf;
	/* NULL on a real shelf, where every game belongs to the system whose
	 * shelf it is. Favorites is a shelf of games drawn from many systems, so
	 * each entry has to carry its own - the core to load, the folder the ROM
	 * is under, the accent, and where the save state lives all follow from
	 * it, and every one of them would be wrong if taken from the shelf. */
	int *owner;
	/* Muse's shelf only, NULL on every other: the album card k is. A card's
	 * cover, a fetched cover landing on it, and A on it all go by the album,
	 * and with two orders a card's place is no longer its album's number. */
	int *album;
	/* The letter jump groups by `name` instead of `title`. Muse's shelf in
	 * artist order has the artist in `name`: grouped by album title there,
	 * up and down would land somewhere that looks arbitrary. In album order
	 * the title is the order, and the jump goes by it as a games shelf's
	 * does. */
	bool jump_by_name;
} sysview;

/* Diatom's display modes, in the order TortOS offers them: the sensible
 * default first, then the shape the core asks for, then whole pixels. Seven
 * once; Integer tall, Overscale, Fill and Native 1:1 went with diatom's
 * ADR-0040 (plorpos-gkd.62), and a shelf saved on one of them loads as
 * Aspect - see display_load.
 *
 * The names are Diatom's protocol strings and have to match its own table in
 * src/scale.c exactly - it answers an unknown one with ERROR code=bad_display
 * and changes nothing. The labels are ours, and are what the menu shows. */
/* STRETCH IS FIRST, AND FIRST IS THE DEFAULT. A sysview is zero-initialized
 * and no display mode is seeded, so index 0 is what every system plays at
 * nobody has configured. Filling the panel is the right default on a handheld
 * whose screen is the whole device: the alternative spends a fifth of a
 * 1024x768 panel on black bars for the Game Boys before anyone has been given
 * a reason to choose. The rest keep their order. */
static const struct { const char *name, *label; } DMODES[] = {
	{ "stretch", "Stretch" },
	{ "aspect",  "Aspect"  },
	{ "integer", "Integer" },
};
#define DMODE_COUNT ((int)(sizeof DMODES / sizeof DMODES[0]))
#define DMODE_ASPECT 1

typedef struct {
	systems_cfg sys;
	/* TortOS/shaders/shaders.cfg; None alone where there is no such file,
	 * which is every Brick - and then nothing about shaders is ever sent. */
	sl_list shaders;

	SDL_Texture *sys_tex[CFG_MAX_SYSTEMS];
	int sys_w[CFG_MAX_SYSTEMS], sys_h[CFG_MAX_SYSTEMS];
	float sys_cb[CFG_MAX_SYSTEMS];
	sysview view[CFG_MAX_SYSTEMS];

	screen_id screen;
	int sys_cursor;
	int menu_w;             /* cached shelf-menu content width; 0 = unmeasured */
	coverflow cf_sys;
	unsigned tint;          /* eased toward the focused system's accent */
	in_state in;
	bool running;
	/* Set for one launch only: the game that comes back after a shutdown
	 * opens with the menu up. Cleared as it is used, so quitting to the
	 * shelf and launching the same game again behaves normally. */
	bool resume_menu;
	int  auto_off;              /* Auto Sleep, seconds without input, 0 off */
	int  auto_poweroff;         /* Auto Off, seconds without input, 0 off -
	                             * mutually exclusive with auto_off */
	idle_clock idle;            /* Auto Off's clock - src/idle.h */
	bool game_on;               /* a game is loaded in Diatom: launch()'s wait
	                             * loop, the game menu and its screens */
	bool pico8_splore;          /* the native PICO-8 running is Splore */
	bool to_splore;             /* its menu asked for Splore next (.50.21) */
	SDL_Renderer *r;
} app;

/* Defined down with the shelf building it belongs to, declared here because
 * the input loop calls it the moment a favorite changes. */
static void refresh_favorites_shelf(app *a);
static void build_muse_shelf(app *a);
/* How a Muse screen was left: back one level, Muse closed altogether, or the
 * library rebuilt under it by Rescan Folder, so every album index it held is
 * stale and the shelf has to find itself again. */
typedef enum { MUSE_BACK, MUSE_CLOSE, MUSE_REBUILT } muse_exit;
static muse_exit muse_tracks(app *a, int album, bool now);
/* Muse's shelf borrows the games shelf's cards and its letter jump, both of
 * which live down with the shelf code. */
static void muse_shelf_screen(app *a, bool now);
static int  muse_artist_of(int al);
static int  shelf_letter_jump(sysview *v, int dir);
/* Whether a shelf is Muse's; sorting needs to know, well before Muse's code.
 * And sorting puts Muse's shelf in order by its own. */
static bool is_muse(const system_cfg *s);
static void muse_order_view(sysview *v);
/* Muse's menu opens it, and the menu is up here. */
static void album_art_screen(app *a);
/* Same reason: Over The Hare is a screen up here and the scan is down there. */
static void rescan_all(app *a);
static bool is_pico8(const system_cfg *s);
static bool is_splore(const system_cfg *s, const char *file);
/* Wi-Fi Services opens both, and they are written after it. */
static void ra_signin_screen(app *a);
static void xfer_screen(app *a);
/* And again: Play Time can send you to a game's shelf, which is the input
 * loop's job and lives with it. */
static void enter_system(app *a);
static int  menu_std_width(app *a);

static volatile sig_atomic_t want_quit;
static void on_sigterm(int sig) { (void)sig; want_quit = 1; plat_terminate(); }

/* Boot time is the one number a launcher cannot be careless about, so the
 * phases are timed and logged rather than guessed at. On the project this
 * grew out of, the phase everyone assumed was expensive turned out to be 8%
 * of startup -- which is only knowable by measuring. */
static unsigned t_boot0;
static void t_mark(const char *what)
{
	fprintf(stderr, "boot: %-14s %5u ms\n", what, plat_now_ms() - t_boot0);
}

/* One number for the way back, because this path keeps being asked how fast it
 * is and a number is how the answer stays true. The five phase marks that took
 * it from 213ms to here are in the history if it ever needs breaking down
 * again; the budget they found was 5ms to black, 4ms of bookkeeping, and the
 * rest decoding the focused card's autosave preview off the SD card. */
static unsigned t_back0;

/* Get off the game's last frame before doing anything slow.
 *
 * Everything between EXIT and the first frame of the fade blocks and none of it
 * presents, so the panel holds whatever the launcher drew last - which after a
 * quit is the in-game menu, over a frame of the game that has ended. Measured
 * on the device: 123ms of it, 52 in plat_leds_off's seventy-nine sysfs writes
 * and 69 rebuilding the focused card off the SD card.
 *
 * One present, black, because the fade that follows starts from black and the
 * two should agree. This is not the buffer-establishing that was tried and
 * reverted: that guarded a stale back buffer, which a swap cannot show. This
 * gets a stale FRONT frame off the glass, which is measurable and was measured.
 */
static void present_black(app *a)
{
	SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_NONE);
	SDL_SetRenderDrawColor(a->r, 0, 0, 0, 255);
	SDL_RenderClear(a->r);
	plat_present(a->r);
	SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
}

/* The boot animation plays in a background process while everything below
 * runs -- the card scan, GL init, font and asset decode, the lot. Both it and
 * this process draw to /dev/fb0, so presenting now would fight it: last
 * writer wins, at 60fps against its 30. Wait for it to clear the marker, then
 * draw.
 *
 * Bounded, because a boot that hangs behind a stuck decoder would be a far
 * worse bug than a seam in an animation. An absent marker means no animation
 * is playing -- every launcher restart after this one -- and this returns at
 * once. */
static void wait_for_boot_anim(void)
{
	const char *flag = getenv("TORTOS_ANIM_FLAG");
	unsigned start;

	if (!flag || !*flag || access(flag, F_OK) != 0) return;
	start = plat_now_ms();
	while (access(flag, F_OK) == 0) {
		if (plat_now_ms() - start > 8000u) {
			fprintf(stderr, "boot: animation flag stuck, drawing anyway\n");
			break;
		}
		SDL_Delay(8);
	}
	t_mark("anim wait");
}

/* ---------- per-system display mode --------------------------------------- */

/* Keyed on the system's tag rather than its folder or its place in the list,
 * for the same reason saves are: renaming a ROM folder or reordering
 * systems.cfg then cannot quietly hand a system somebody else's setting. */
static void display_load(app *a)
{
	char key[CFG_STR + 16], name[CFG_STR];
	int i, k;

	for (i = 0; i < a->sys.count; i++) {
		snprintf(key, sizeof key, "display.%s", a->sys.systems[i].tag);
		if (!db_get_str(db_dev(), key, name, sizeof name, NULL)) continue;
		/* A name no longer in the table is a mode diatom dropped
		 * (ADR-0040): Aspect, the kept mode closest to all four - the
		 * same picture as Fill wherever Fill did not crop. */
		a->view[i].dmode = DMODE_ASPECT;
		for (k = 0; k < DMODE_COUNT; k++)
			if (!strcmp(DMODES[k].name, name)) { a->view[i].dmode = k; break; }
	}
}

/* One row per visible system, and nothing else touched.
 *
 * The file this replaces was rewritten whole, so it had to read itself back
 * first and carry forward the modes of systems that are not on the shelf right
 * now. That was not tidiness: empty systems are hidden, so "every system" meant
 * "every system with games in it today", and a plain rewrite would quietly
 * erase the mode of any system whose ROMs happened to be off the card. Take
 * the ROMs out, change one unrelated setting, put them back, and the mode you
 * chose is gone with no sign it existed.
 *
 * A row write cannot touch another row, so that whole pass is gone rather than
 * ported. It is the clearest thing the database bought. */
static void display_save(app *a)
{
	char key[CFG_STR + 16];
	int i;

	for (i = 0; i < a->sys.count; i++) {
		snprintf(key, sizeof key, "display.%s", a->sys.systems[i].tag);
		db_set_str(db_dev(), key, DMODES[a->view[i].dmode].name);
	}
}

/* shader.<TAG>, beside display.<TAG> and keyed the same way. The list is read
 * here too: its names are what the rows hold, so the two belong together. A
 * name the list no longer has is None (sl_find) - an entry renamed or dropped
 * from shaders.cfg sends its systems back to the plain picture. */
static void shader_load(app *a)
{
	char path[CFG_STR * 2], key[CFG_STR + 16], name[SL_NAME];
	int i;

	snprintf(path, sizeof path, "%s/shaders/shaders.cfg", P_ROOT);
	sl_load(&a->shaders, path);
	for (i = 0; i < a->sys.count; i++) {
		snprintf(key, sizeof key, "shader.%s", a->sys.systems[i].tag);
		a->view[i].shader = db_get_str(db_dev(), key, name, sizeof name, NULL)
		                    ? sl_find(&a->shaders, name) : 0;
	}
}

/* One row, the system's whose choice changed - see display_save on why rows. */
static void shader_save(app *a, int sys)
{
	char key[CFG_STR + 16];

	snprintf(key, sizeof key, "shader.%s", a->sys.systems[sys].tag);
	db_set_str(db_dev(), key, a->shaders.e[a->view[sys].shader].name);
}

/* Which shader a launch tells the game to draw (diatom's ADR-0041), as
 * SETDISPLAY fields: "" for None, NULL when there is no list to choose from.
 * Diatom keeps a chain across RUN, as it keeps the mode, so a launch always
 * says one; plat_resident_send sends it before RUN, with None first. */
static const char *shader_fields(app *a, int sys, char *f, size_t cap)
{
	char dir[CFG_STR * 2];
	int i = a->view[sys].shader;

	if (a->shaders.count <= 1) return NULL;
	snprintf(dir, sizeof dir, "%s/shaders", P_ROOT);
	if (i == 0 || !sl_fields(&a->shaders, i, dir, f, cap)) return "";
	return f;
}

/* palette.GB.<file>, in the library database: a choice per game rather than
 * per system (plorpos-gkd.76), so it travels with the card the way favorites
 * do. Keyed on the file name, as save states are, so renaming a ROM loses it.
 * -1 for a game that is not a Game Boy game; no row is Auto. */
static int palette_of(app *a, int sys, const game_entry *g)
{
	char key[LIB_PATH + 32], name[CFG_STR];
	const char *base;

	if (strcmp(a->sys.systems[sys].tag, "GB")) return -1;
	base = strrchr(g->file, '/');
	snprintf(key, sizeof key, "palette.GB.%s", base ? base + 1 : g->file);
	return db_get_str(db_lib(), key, name, sizeof name, NULL) ? gbpal_find(name) : 0;
}

static void palette_save(const game_entry *g, int i)
{
	char key[LIB_PATH + 32];
	const char *base = strrchr(g->file, '/');

	snprintf(key, sizeof key, "palette.GB.%s", base ? base + 1 : g->file);
	db_set_str(db_lib(), key, gbpal_label(i));
}

/* disc.<TAG>.<file>, the disc an .m3u game was last on (plorpos-gkd.47),
 * 0-based as diatom counts them. Sent on RUN so a game resumed from its
 * auto-state finds the disc it was saved on in the drive. -1: not an .m3u,
 * or never swapped. */
static bool is_m3u(const char *file)
{
	size_t n = strlen(file);
	return n > 4 && !strcasecmp(file + n - 4, ".m3u");
}

static void disc_key(char *key, size_t n, const char *tag, const game_entry *g)
{
	const char *base = strrchr(g->file, '/');
	snprintf(key, n, "disc.%s.%s", tag, base ? base + 1 : g->file);
}

static int disc_of(app *a, int sys, const game_entry *g)
{
	char key[LIB_PATH + CFG_STR + 16];

	if (!is_m3u(g->file)) return -1;
	disc_key(key, sizeof key, a->sys.systems[sys].tag, g);
	return db_get_int(db_lib(), key, -1);
}

static void disc_save(app *a, int sys, const game_entry *g, int i)
{
	char key[LIB_PATH + CFG_STR + 16];

	disc_key(key, sizeof key, a->sys.systems[sys].tag, g);
	db_set_int(db_lib(), key, i);
}

/* What diatom said when the in-game menu opened: count 0 = no Disc row. */
static struct { int index, count; char label[24]; } g_disc;

/* engine.<TAG>, PICO-8's shelf only, beside display.<TAG>. No row means
 * fake08: only a shelf someone has turned to native has one. */
static void engine_load(app *a)
{
	char key[CFG_STR + 16], name[CFG_STR];
	int i;

	for (i = 0; i < a->sys.count; i++) {
		if (!is_pico8(&a->sys.systems[i])) continue;
		snprintf(key, sizeof key, "engine.%s", a->sys.systems[i].tag);
		if (db_get_str(db_dev(), key, name, sizeof name, NULL))
			a->view[i].native = !strcmp(name, "native");
	}
}

static void engine_save(app *a, int sys)
{
	char key[CFG_STR + 16];

	snprintf(key, sizeof key, "engine.%s", a->sys.systems[sys].tag);
	db_set_str(db_dev(), key, a->view[sys].native ? "native" : "fake08");
}

#define ENGINE_LABEL(native) ((native) ? "Pico-8 Native" : "fake-08")

/* ---------- per-system sort order ----------------------------------------- */

/* Keyed on the tag for the same reason display.<TAG> is, and stored beside it.
 * Loading does NOT re-sort: it runs before the scan on a rescan and after it
 * at startup, and the shelf is put in order by sort_all once both are done.
 *
 * Muse's shelf has orders of its own, by artist and by album, stored by their
 * own names under the same key - sort.MUSE. */
static void sort_load(app *a)
{
	char key[CFG_STR + 16], name[CFG_STR];
	int i;

	for (i = 0; i < a->sys.count; i++) {
		snprintf(key, sizeof key, "sort.%s", a->sys.systems[i].tag);
		if (!db_get_str(db_dev(), key, name, sizeof name, NULL)) continue;
		a->view[i].sort = is_muse(&a->sys.systems[i]) ? (int)ml_order_index(name)
		                                              : sort_index(name);
	}
}

static void sort_save(app *a)
{
	char key[CFG_STR + 16];
	int i;

	for (i = 0; i < a->sys.count; i++) {
		snprintf(key, sizeof key, "sort.%s", a->sys.systems[i].tag);
		db_set_str(db_dev(), key,
		           is_muse(&a->sys.systems[i]) ? ml_order_name((ml_order)a->view[i].sort)
		                                       : SORTS[a->view[i].sort].name);
	}
}

/* Put every shelf in its own order. One pass over the systems, and the
 * session rows are folded once per system that needs them rather than once
 * per game - which is why this is a loop here and not a call inside scan_all.
 *
 * The favorites shelf is built elsewhere and keeps its own order: it is a
 * shelf of games from many systems and has no single tag to hang a setting
 * off. */
/* One shelf in its own order.
 *
 * Favorites through sort_apply_owned: its games are several systems', and each
 * one's owner has to come with it and its play time has to be read under its
 * own system's tag. This ran the plain sort on Favorites as well, which only
 * held because Favorites was always sorted by name, the order it is built in -
 * any other order would have launched some favorites under another system's
 * core.
 *
 * Muse by its own orders, in muse_order_view: its cards are albums, and which
 * album each card is has to move with it. */
static void sort_shelf(app *a, int sys)
{
	sysview *v = &a->view[sys];

	if (is_muse(&a->sys.systems[sys])) { muse_order_view(v); return; }
	if (v->owner) {
		const char *tags[CFG_MAX_SYSTEMS];
		int i;

		for (i = 0; i < a->sys.count && i < CFG_MAX_SYSTEMS; i++)
			tags[i] = a->sys.systems[i].tag;
		sort_apply_owned(v->list.items, v->owner, v->list.count, v->sort, tags);
		return;
	}
	/* Splore stays first in every order: the carts sort behind it. */
	if (v->list.count && is_splore(&a->sys.systems[sys], v->list.items[0].file))
		sort_apply(v->list.items + 1, v->list.count - 1, v->sort,
		           a->sys.systems[sys].tag);
	else
		sort_apply(v->list.items, v->list.count, v->sort, a->sys.systems[sys].tag);
}

static void sort_all(app *a)
{
	int i;

	for (i = 0; i < a->sys.count; i++) sort_shelf(a, i);
}

/* ---------- textures ----------------------------------------------------- */

/* Every card texture ends up with an alpha channel, whatever the file had.
 *
 * IMG_LoadTexture keeps the source format, so art saved as PNG color type 2 -
 * RGB, no alpha - becomes an RGB texture. Drawn straight to the screen that is
 * fine, which is why nothing was ever wrong with the shelves. Drawn through
 * SDL_RenderGeometry into an RGBA render target it is wrong on this driver:
 * cards came out missing entirely on a cube face, and system art came out
 * garbled. Measured 2026-09-07 against Toy Story and Uncharted Waters, which
 * carry alpha and drew correctly, and Dr. Franken and Donkey Kong Land III,
 * which do not and did not.
 *
 * Converted here rather than at the call sites: there is one loader, the cost
 * is one format conversion per card at load time, and a texture whose behavior
 * depends on what a scraper happened to save is a trap for whatever gets drawn
 * next. */
/* How far down the surface its opaque pixels reach, as a fraction of height.
 *
 * Art here is squared and centered, so a console photographed low in its frame
 * carries a band of transparency underneath - 102 of 384 rows for the Genesis,
 * against 10 for a Game Boy. Reflecting from the card's edge mirrors that
 * emptiness too, which is why the same reflection setting produced a flush
 * reflection on one card and a gap of about 196px on another.
 *
 * Scanned bottom-up and stopping at the first row with anything in it, so the
 * cost is the padding rather than the image. Alpha over 8 rather than over 0:
 * a PNG's fully transparent region is not always exactly zero. */
static float content_bottom(SDL_Surface *s)
{
	int y, x;

	if (!s || s->h <= 0 || s->format->BytesPerPixel != 4) return 1.0f;
	for (y = s->h - 1; y >= 0; y--) {
		const Uint32 *row = (const Uint32 *)((Uint8 *)s->pixels + y * s->pitch);

		for (x = 0; x < s->w; x++)
			if ((row[x] >> 24) > 8)
				return (float)(y + 1) / (float)s->h;
	}
	return 1.0f;
}

static SDL_Texture *load_image(SDL_Renderer *r, const char *path, int *w, int *h,
                               float *cb)
{
	SDL_Surface *raw = IMG_Load(path), *conv;
	SDL_Texture *t;

	if (cb) *cb = 1.0f;
	if (!raw) return NULL;
	conv = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_ARGB8888, 0);
	SDL_FreeSurface(raw);
	if (!conv) return NULL;
	if (cb) *cb = content_bottom(conv);
	t = SDL_CreateTextureFromSurface(r, conv);
	SDL_FreeSurface(conv);
	if (t) SDL_QueryTexture(t, NULL, NULL, w, h);
	return t;
}

/* Which set is showing. Read once at boot and kept here, because sys_get_tex
 * runs per card per frame and the shelf should not ask the database how to
 * draw itself. */
static int g_cards;
static int g_dir;

/* Muse: what the Music folder holds, read at boot and on every rescan, and
 * the green its card is drawn in - the iPod's own, sampled from the photograph
 * (tools/genmusecard.py draws both cards from the same value).
 *
 * RECOGNIZED BY ITS TAG, not by what it lacks. Favorites is the shelf with no
 * core and no folder, and the system menu tells them apart that way; Muse has
 * neither either, but its music is not a ROM folder at all, so an empty folder
 * is the honest value and the tag is what says which of the two it is. */
#define MUSE_ACCENT 0x9CD345u
static ml_lib g_muse;
/* Where the scan read it from: the card, which holds Music and Audiobooks. */
static char   g_muse_root[CFG_STR * 2];
/* Bumped by every build of the library, so a Muse screen can tell that MENU's
 * Rescan Folder replaced it while its menu was open - see muse_menu. */
static unsigned g_muse_gen;

static bool is_muse(const system_cfg *s)
{
	return s && !strcmp(s->tag, "MUSE");
}

/* What is known about each album's cover: one per album in g_muse, rebuilt
 * with it. ASKED carries when, because an answer can be lost with the daemon,
 * and an album is then asked about again rather than waited on for good. */
enum { COV_UNKNOWN, COV_ASKED, COV_JPG, COV_PNG, COV_FOLDER, COV_NONE };
typedef struct { unsigned char st; unsigned asked_ms; } cover_state;
static cover_state *g_cov;

/* ---- Muse: books ------------------------------------------------------------ */

/* Which kind Muse's shelf shows, music or books. Eric's, 2026-09-27: one Muse
 * card and a Show row in its menu, rather than a card for books beside it -
 * two ways into one player read as two players. Kept across restarts. */
static bool g_muse_books;

/* Whether each album is a book listened to the end: one per album in g_muse,
 * rebuilt with it, like g_cov. A finished book is marked and not forgotten,
 * and A on it starts it over. Eric's, 2026-09-27. */
static bool *g_book_done;

/* The kind the shelf really shows: the one chosen, unless there is none of it
 * and some of the other. */
static bool muse_books_shown(void)
{
	int books = ml_count(&g_muse, true), music = ml_count(&g_muse, false);

	if (!books) return false;
	if (!music) return true;
	return g_muse_books;
}

/* Where book `al`'s place is kept: the settings, under its folder. */
static void book_key(int al, char *out, size_t n)
{
	const char *p = g_muse.tracks[g_muse.albums[al].first].path;
	const char *slash = strrchr(p, '/');

	snprintf(out, n, "book.%.*s", slash ? (int)(slash - p) : 0, p);
}

/* A place is "<seconds>\t<file>", the file by its name rather than its number
 * so a file added to the folder later moves nothing, or "finished". */
static void book_write(int al, const char *track, double at)
{
	char key[LIB_PATH + 8], val[LIB_PATH + 32];
	const char *file = strrchr(track, '/');

	book_key(al, key, sizeof key);
	snprintf(val, sizeof val, "%.1f\t%s", at, file ? file + 1 : track);
	db_set_str(db_dev(), key, val);
	g_book_done[al] = false;
}

static void book_finish(int al)
{
	char key[LIB_PATH + 8];

	book_key(al, key, sizeof key);
	db_set_str(db_dev(), key, "finished");
	g_book_done[al] = true;
	fprintf(stderr, "muse: finished %s\n", g_muse.albums[al].name);
}

/* Where book `al` starts: its file, and `at` seconds into it. The beginning
 * for a book never started, or finished. */
static int book_place(int al, double *at)
{
	const ml_album *b = &g_muse.albums[al];
	char key[LIB_PATH + 8], val[LIB_PATH + 32];
	const char *tab;
	int i;

	*at = 0;
	book_key(al, key, sizeof key);
	db_get_str(db_dev(), key, val, sizeof val, "");
	if (!(tab = strchr(val, '\t'))) return 0;
	for (i = 0; i < b->n; i++) {
		const char *p = g_muse.tracks[b->first + i].path;
		const char *f = strrchr(p, '/');

		if (!strcmp(f ? f + 1 : p, tab + 1)) { *at = atof(val); return i; }
	}
	return 0;
}

/* KEEPING A BOOK'S PLACE, wherever Muse is polled: on its screens, over the
 * shelf and the menus, and through a game, which is where a book is likely to
 * be heard.
 *
 * Written when the file changes, when it pauses or stops, and every thirty
 * seconds of listening in between, so a Brick that loses its battery goes back
 * half a minute at most. `now` writes whatever there is, for the power button
 * and for a new queue about to replace this one. */
static struct { int al, index; mu_state st; double at; } g_bk = { -1, -1, MU_OFF, 0 };

static void book_keep(bool now)
{
	const mu_now *mn = musec_now();
	const char *track;
	bool ran_out = musec_take_ran_out();
	int al;

	if (!musec_is_book() || !g_book_done) { g_bk.al = -1; return; }
	track = musec_track(mn->index);
	al = ml_album_of(&g_muse, track);
	if (al < 0 || !g_muse.albums[al].book) { g_bk.al = -1; return; }
	if (ran_out) { book_finish(al); g_bk.al = -1; return; }
	if (mn->state != MU_PLAYING && mn->state != MU_PAUSED) return;
	if (now || al != g_bk.al || mn->index != g_bk.index || mn->state != g_bk.st ||
	    fabs(mn->at - g_bk.at) >= 30) {
		book_write(al, track, mn->at);
		g_bk.al = al;
		g_bk.index = mn->index;
		g_bk.st = mn->state;
		g_bk.at = mn->at;
	}
}

/* Muse, polled: what the daemon said, and a book's place kept. Everything that
 * polls Muse comes through here. */
static void muse_poll(void)
{
	musec_poll();
	book_keep(false);
}

/* A book's speed, kept beside its place: the last one chosen on it, 1x for a
 * book never changed. Its own key rather than a third field in the place, so
 * a place written before speeds existed still reads. */
static void book_speed_key(int al, char *out, size_t n)
{
	const char *p = g_muse.tracks[g_muse.albums[al].first].path;
	const char *slash = strrchr(p, '/');

	snprintf(out, n, "bookspeed.%.*s", slash ? (int)(slash - p) : 0, p);
}

static double book_speed(int al)
{
	char key[LIB_PATH + 16], val[16];
	double x;

	book_speed_key(al, key, sizeof key);
	db_get_str(db_dev(), key, val, sizeof val, "1");
	x = atof(val);
	return x >= 0.5 && x <= 2.0 ? x : 1.0;
}

/* Y on a book's Now Playing: the next speed, 1x to 2x in quarters and then
 * 0.75x before coming round - the speeds a listener reaches for, in the order
 * they reach for them. Set on the book as it plays and kept for it. */
static void book_cycle_speed(void)
{
	static const double STEPS[] = { 1.0, 1.25, 1.5, 1.75, 2.0, 0.75 };
	int al = ml_album_of(&g_muse, musec_track(musec_now()->index));
	double now = musec_speed(), next = STEPS[0];
	char key[LIB_PATH + 16], val[16];
	size_t k;

	for (k = 0; k < sizeof STEPS / sizeof STEPS[0]; k++)
		if (fabs(STEPS[k] - now) < 0.01) {
			next = STEPS[(k + 1) % (sizeof STEPS / sizeof STEPS[0])];
			break;
		}
	musec_set_speed(next);
	if (al < 0 || !g_muse.albums[al].book) return;
	book_speed_key(al, key, sizeof key);
	snprintf(val, sizeof val, "%.2f", next);
	db_set_str(db_dev(), key, val);
}

/* A new queue. Whatever book it replaces has its place written first, since
 * the queue is the only record of where that was. */
static void muse_play(const char *const *paths, int n, int start, double at,
                      bool book, double speed, const char *artist, const char *album)
{
	book_keep(true);
	g_bk.al = -1;
	musec_play(paths, n, start, at, book, speed, artist, album);
}

/* The play modes as the player meets them: the name saved in the settings and
 * the mark. In order has no mark: it is what plays when nothing has been asked
 * for, and a mark meaning "nothing special" is noise.
 *
 * The name each mode was SHOWN by went with the notice that showed it - see
 * muse_cycle_mode. Nothing spells a mode out now; the mark beside the track
 * count on Now Playing is the whole of it. */
static const struct { const char *key; int glyph; } MUSE_MODES[MUQ_MODES] = {
	[MUQ_IN_ORDER]   = { "in order",   -1 },
	[MUQ_REPEAT_ALL] = { "repeat all", UI_GLYPH_REPEAT },
	[MUQ_REPEAT_ONE] = { "repeat one", UI_GLYPH_REPEAT_ONE },
	[MUQ_SHUFFLE]    = { "shuffle",    UI_GLYPH_SHUFFLE },
};

/* The cover on the Now Playing screen, kept between visits so that opening it
 * is not a decode: the album it is for and its texture. `done` means this is
 * what the album will show - its art, or the card made for one with none - and
 * nothing more needs asking. */
static struct {
	int          album;
	bool         done;
	SDL_Texture *tex;
} g_np = { .album = -1 };

static void np_forget(void)
{
	if (g_np.tex) SDL_DestroyTexture(g_np.tex);
	g_np.tex = NULL;
	g_np.album = -1;
	g_np.done = false;
}

static SDL_Texture *sys_get_tex(void *ctx, int i, int *w, int *h, float *cb)
{
	app *a = ctx;
	if (!a->sys_tex[i]) {
		char path[CFG_STR * 2];

		snprintf(path, sizeof path, "%s/cards/%s/%s", P_ROOT,
		         CARD_SETS[g_cards].dir, a->sys.systems[i].card);
		a->sys_tex[i] = load_image(a->r, path, &a->sys_w[i], &a->sys_h[i],
		                           &a->sys_cb[i]);
		/* A set may be incomplete and still be worth showing. Favorites is
		 * the standing example: it is a shelf, not a console, so a set of
		 * hardware photographs has nothing to put there and borrows the
		 * default's card rather than falling all the way through to the
		 * generated one. */
		if (!a->sys_tex[i] && g_cards != 0) {
			snprintf(path, sizeof path, "%s/cards/%s/%s", P_ROOT,
			         CARDS_DEFAULT, a->sys.systems[i].card);
			a->sys_tex[i] = load_image(a->r, path, &a->sys_w[i], &a->sys_h[i],
		                           &a->sys_cb[i]);
		}
		if (!a->sys_tex[i])
			a->sys_tex[i] = ui_make_card(a->r, a->sys.systems[i].name,
			                             a->sys.systems[i].accent,
			                             &a->sys_w[i], &a->sys_h[i]);
	}
	*w = a->sys_w[i];
	*h = a->sys_h[i];
	if (cb) *cb = a->sys_cb[i];
	return a->sys_tex[i];
}

/* Where a game's autosave preview lives: the frame the player was looking at
 * when they stopped. The directory is named after the ROM's folder under
 * Roms/.
 *
 * The directory and the slot names are TortOS's own. Both were briefly
 * borrowed from the emulator TortOS replaced, and the borrowing cost more than
 * it saved: a name that describes another project invites a reader to treat
 * the contents as dead, and on 2026-08-28 the live autosave tree was deleted
 * by someone auditing the card for exactly that reason. Nothing was lost only
 * because states were being cleared for testing the same hour.
 *
 * Renamed with no migration path, deliberately - nothing has shipped, so there
 * are no cards in the world to be kind to. */
/* Six manual save slots and one autosave.
 *
 * Six because it is enough and it fits the carousel's dot rail without
 * crowding, not because any particular number is correct. An earlier eight was
 * copied from another launcher and had no reasoning of its own behind it.
 *
 * The resume slot is named rather than numbered on disk: `<game>.auto.state`
 * explains itself, where a number needs a reader to already know which slot
 * means auto. Its value is only an internal sentinel and must not collide with
 * 1..GM_SLOTS, so it is derived rather than picked. Both live in game_menu.h
 * now, with the carousel mapping beside them, so a check can reach all three.
 */

static const char *slot_name(int slot)
{
	static char buf[8];
	if (slot == SLOT_AUTO) return "auto";
	snprintf(buf, sizeof buf, "%d", slot);
	return buf;
}

static void preview_path(app *a, int s, const game_entry *g, char *out, size_t n)
{
	const char *base = strrchr(g->file, '/');
	base = base ? base + 1 : g->file;
	snprintf(out, n, "%s/.tortos/%s/%s.%s.bmp",
	         P_SHARED, a->sys.systems[s].folder, base, slot_name(SLOT_AUTO));
}

/* Where Diatom writes the frame it hands over, which is NOT the Auto card's
 * picture.
 *
 * Diatom writes its preview twice: before PAUSED, so the launcher has a frame
 * to dim and draw the menu over, and again at exit beside the state. Pointing
 * both at <rom>.auto.bmp meant every menu open overwrote the Auto card with
 * the current moment, while the card's timestamp still came from the state -
 * so the card contradicted itself and the picture was the convincing half.
 * Reported 2026-09-07: died in Contra, opened the menu, and the Auto card
 * showed the death while loading it went back two levels.
 *
 * One scratch file for the whole device rather than one per game: nothing
 * reads it after the launch that wrote it, and a per-game copy would leave
 * litter beside every save. */
static void pause_preview_path(char *out, size_t n)
{
	snprintf(out, n, "%s/pause.bmp", P_USERDATA);
}

/* The Diatom transport takes explicit paths rather than a slot number, so the
 * state lives beside the preview it belongs to, named the same way.
 *
 * The autosave is named `auto`, not a number. Slots 1..GM_SLOTS are the
 * player's own,
 * reachable from the in-game menu, and are genuinely numbered; the resume slot
 * is not one of them and calling it 9 only meant something to someone who knew
 * which emulator picked that number. Each slot is a state and a preview named
 * alike, so a slot that has a picture has a game behind it. */
static void slot_state_path(app *a, int s, const game_entry *g, int slot,
                            char *out, size_t n)
{
	const char *base = strrchr(g->file, '/');
	base = base ? base + 1 : g->file;
	snprintf(out, n, "%s/.tortos/%s/%s.%s.state",
	         P_SHARED, a->sys.systems[s].folder, base, slot_name(slot));
}

static void slot_preview_path(app *a, int s, const game_entry *g, int slot,
                              char *out, size_t n)
{
	const char *base = strrchr(g->file, '/');
	base = base ? base + 1 : g->file;
	snprintf(out, n, "%s/.tortos/%s/%s.%s.bmp",
	         P_SHARED, a->sys.systems[s].folder, base, slot_name(slot));
}

static void state_path(app *a, int s, const game_entry *g, char *out, size_t n)
{
	slot_state_path(a, s, g, SLOT_AUTO, out, n);
}

/* The launcher owns the paths, so the launcher makes the directories - the
 * emulator writes where it is told and fails where it cannot. A missing mkdir
 * here once cost every preview on the card, silently, because writing to a
 * path in a directory that does not exist fails per-write and looks like
 * nothing happening. */
/* Where the two databases live, and the directories they need. Both callers
 * are in main(): --dump, which runs before video, and the boot path. Having
 * one of them create the directory and the other not is how --dump failed on a
 * fresh tree the first time it was tried. */
static bool db_paths_ready(char *dev, size_t ndev, char *lib, size_t nlib,
                           char *env, size_t nenv)
{
	char d[CFG_STR * 2];

	mkdir(P_USERDATA, 0755);
	mkdir(P_SHARED, 0755);
	snprintf(d, sizeof d, "%s/.tortos", P_SHARED);
	mkdir(d, 0755);

	snprintf(dev, ndev, "%s/tortos.db", P_USERDATA);
	snprintf(lib, nlib, "%s/.tortos/library.db", P_SHARED);
	if (env) snprintf(env, nenv, "%s/boot.env", P_USERDATA);
	return true;
}

static void persist_dir_ensure(app *a, int s)
{
	char d[LIB_PATH * 2];
	snprintf(d, sizeof d, "%s/.tortos", P_SHARED);
	mkdir(d, 0755);
	snprintf(d, sizeof d, "%s/.tortos/%s", P_SHARED, a->sys.systems[s].folder);
	mkdir(d, 0755);
}

/* Card art, in order of preference: the box art the user put in .media/, then
 * the autosave preview -- the last frame they saw, which for a game in
 * progress is a better card than any box -- then a generated slab. */
/* The system an entry on a shelf belongs to. Real shelves answer with
 * themselves; Favorites answers per entry. */
static int shelf_owner(app *a, int sysidx, int item)
{
	sysview *v = &a->view[sysidx];
	if (v->owner && item >= 0 && item < v->list.count) return v->owner[item];
	return sysidx;
}

/* The view whose SYSTEM settings the game under the cursor belongs to.
 *
 * Favorites is a shelf of other systems' games, so the two kinds of setting
 * have to be told apart: a property of the SYSTEM - the core, the ROM folder,
 * the BIOS, the display mode - resolves through the game's owner, while a
 * property of the SHELF - its sort order, its cursor, its own card - belongs
 * to the shelf you are standing on.
 *
 * launch() had that right for three of the four and wrong for the display
 * mode, which it read off the shelf: a Genesis game set to integer on its own
 * shelf played stretched when it was started from Favorites, because the mode
 * it used was the one filed under FAV. Nothing showed it on this card, where
 * every system reads stretch and all the answers agree. Named as a function
 * rather than repeated at each site so the next per-system setting has one
 * obvious thing to call. */
static sysview *owner_view(app *a)
{
	sysview *v = &a->view[a->sys_cursor];

	return &a->view[shelf_owner(a, a->sys_cursor, v->cursor)];
}

/* Where a game's box art lives.
 *
 * One definition because four places need the same file - the card, the info
 * screen, the replace action, and the return from a game - and the last of
 * those has to test exactly the file the card loads. Four hand-written copies
 * of the format string only have to drift once for it to ask about one file
 * and draw another. */
static void box_art_path(const char *folder, const char *name, char *out, size_t n)
{
	snprintf(out, n, "%s/%s/.media/%s.png", P_ROMS, folder, name);
}

/* Whether it is there. An empty file is not art - the rule the info screen
 * already used, now the rule for everyone. */
static bool has_box_art(const char *folder, const char *name)
{
	char p[LIB_PATH * 3];
	struct stat st;

	box_art_path(folder, name, p, sizeof p);
	return stat(p, &st) == 0 && st.st_size > 0;
}

/* Where a card's own picture comes from: the box art, or failing that a
 * PICO-8 image cart, which IS its box art - nobody publishes covers for them
 * (plorpos-gkd.50.14) - or for Splore, which is no file at all, the gold cart
 * the launcher ships (plorpos-gkd.50.24). Reading only: the replace action
 * keeps writing to box_art_path, so a chosen picture wins and a cart is never
 * written over. */
static bool card_art_path(const system_cfg *s, const game_entry *g, char *out, size_t n)
{
	const char *folder = s->folder;
	size_t fl = strlen(g->file);
	bool box = has_box_art(folder, g->name);

	if (!box && is_splore(s, g->file)) {
		snprintf(out, n, "%s/cards/splore.png", P_ROOT);
		return true;
	}

	if (!box && fl > 7 && !strcasecmp(g->file + fl - 7, ".p8.png")) {
		snprintf(out, n, "%s/%s/%s", P_ROMS, folder, g->file);
		return true;
	}
	box_art_path(folder, g->name, out, n);
	return box;
}

/* Muse's half of the card machinery, defined with the rest of Muse. */
static void muse_card_want(app *a, int s, int i);
static void cover_shape(SDL_Surface **s);

static SDL_Texture *game_get_tex(void *ctx, int i, int *w, int *h, float *cb)
{
	app *a = ctx;
	int s = a->sys_cursor;
	sysview *v = &a->view[s];
	/* The game's own system, not the shelf's: on Favorites those differ, and
	 * every one of these three lookups would otherwise go to the wrong
	 * folder, the wrong state directory and the wrong accent. */
	int o = shelf_owner(a, s, i);

	if (!v->tex[i] && is_muse(&a->sys.systems[s])) {
		muse_card_want(a, s, i);
	} else if (!v->tex[i]) {
		char art[LIB_PATH * 3], prev[LIB_PATH * 3];

		card_art_path(&a->sys.systems[o], &v->list.items[i],
		              art, sizeof art);
		preview_path(a, o, &v->list.items[i], prev, sizeof prev);
		/* Hand it to the worker and draw nothing this frame. Asking is cheap
		 * and ignores a card already in hand, so callers ask every frame. */
		if (!texload_want(s, i, art, prev)) {
			/* No worker. Exactly what this did before, on the frame. */
			v->tex[i] = load_image(a->r, art, &v->tw[i], &v->th[i], &v->cb[i]);
			if (!v->tex[i])
				v->tex[i] = load_image(a->r, prev, &v->tw[i], &v->th[i], &v->cb[i]);
			if (!v->tex[i])
				v->tex[i] = ui_make_card(a->r, v->list.items[i].title,
				                         a->sys.systems[o].accent,
				                         &v->tw[i], &v->th[i]);
		}
	}
	if (!v->tex[i]) {
		/* Still coming. cf_draw skips a card with no texture, so the slot is
		 * simply empty for a few frames instead of the shelf stopping. */
		*w = *h = 0;
		if (cb) *cb = 1.0f;
		return NULL;
	}
	*w = v->tw[i];
	*h = v->th[i];
	if (cb) *cb = v->cb[i];
	return v->tex[i];
}

/* Install everything the worker finished, on the thread that owns the
 * renderer. SDL_CreateTextureFromSurface is the only part that needs it and
 * costs 1-2ms; content_bottom is computed here too, because it stops at the
 * first row with anything in it and so costs the padding, not the image. */
static void texload_drain(app *a)
{
	SDL_Surface *surf;
	int s, i;

	while (texload_take(&s, &i, &surf)) {
		sysview *v;

		if (s < 0 || s >= a->sys.count) { if (surf) SDL_FreeSurface(surf); continue; }
		v = &a->view[s];
		/* The shelf may have moved on, or something else may have filled this
		 * slot while the decode was in the air. */
		if (i < 0 || i >= v->list.count || v->tex[i]) {
			if (surf) SDL_FreeSurface(surf);
			continue;
		}
		/* A cover is squared and given a card's corners, the way Now
		 * Playing draws it, so one album looks the same in both places. */
		if (surf && is_muse(&a->sys.systems[s])) cover_shape(&surf);
		if (surf) {
			v->cb[i] = content_bottom(surf);
			v->tex[i] = SDL_CreateTextureFromSurface(a->r, surf);
			if (v->tex[i])
				SDL_QueryTexture(v->tex[i], NULL, NULL, &v->tw[i], &v->th[i]);
			SDL_FreeSurface(surf);
		}
		if (!v->tex[i]) {
			/* Neither file was there, so the card is the generated slab - and
			 * that needs the renderer, which is why it is made here and not
			 * on the worker. */
			int o = shelf_owner(a, s, i);
			v->tex[i] = is_muse(&a->sys.systems[s])
			          ? ui_make_cover(a->r, v->list.items[i].title,
			                          a->sys.systems[o].accent, &v->tw[i], &v->th[i])
			          : ui_make_card(a->r, v->list.items[i].title,
			                         a->sys.systems[o].accent,
			                         &v->tw[i], &v->th[i]);
			v->cb[i] = 1.0f;
		}
	}
}

static void evict_far(sysview *v, int keep)
{
	for (int i = 0; i < v->list.count; i++) {
		int d = abs(i - v->cursor);
		if (v->list.count >= CF_WINDOW)
			d = d > v->list.count / 2 ? v->list.count - d : d;
		if (v->tex[i] && d > keep) {
			SDL_DestroyTexture(v->tex[i]);
			v->tex[i] = NULL;
		}
	}
}

#define PRIME_HALF_MAX (CF_HALF_WINDOW + 2)

/* Ask for the cards on shelf `s` around `center`, out to `half` either side.
 *
 * FARTHEST FIRST. The workers take the newest request first (texload.c), so
 * they decode in the reverse of the order asked, and the center has to be asked
 * for last to be decoded first. Both callers used to ask for it first or in the
 * middle, on the reasoning that the order asked is the order decoded - true of
 * the queue they were written against, and backwards since it went newest-first.
 * Measured 2026-09-14, cold after a reboot: entering a system put the focused
 * card up 70-160ms after A, only 15-35ms ahead of the rest of its row, when it
 * should have been the first to arrive.
 *
 * Each card is asked for once. On a shelf shorter than the window, wrapping
 * offers the same card from both sides, and the second ask would be refused as
 * already queued - leaving the card wherever the first ask put it.
 *
 * game_get_tex reads a->sys_cursor rather than taking a shelf, so it is swapped
 * around the calls. */
static void prime_around(app *a, int s, int center, int half)
{
	sysview *v = &a->view[s];
	int n = v->list.count, idx[2 * PRIME_HALF_MAX + 1], m = 0;
	int k, j, save = a->sys_cursor;

	if (n <= 0) return;
	if (half > PRIME_HALF_MAX) half = PRIME_HALF_MAX;
	for (k = 0; k <= half; k++) {
		int side;
		for (side = 1; side >= (k ? -1 : 1); side -= 2) {
			int i = center + k * side;
			if (n >= 2) {
				i %= n;
				if (i < 0) i += n;
			} else if (i != 0) {
				continue;
			}
			for (j = 0; j < m && idx[j] != i; j++) ;
			if (j == m) idx[m++] = i;
		}
	}
	a->sys_cursor = s;
	while (m-- > 0) {
		int w, h;
		if (!v->tex[idx[m]]) game_get_tex(a, idx[m], &w, &h, NULL);
	}
	a->sys_cursor = save;
}

/* The visible row and two more either side, so the first scroll finds its
 * neighbors already there instead of sliding in blank. */
static void prime_window(app *a, int s)
{
	prime_around(a, s, a->view[s].cursor, PRIME_HALF_MAX);
}

/* Ask for the whole window around `center`.
 *
 * This used to take a budget and spend one card per frame, because asking
 * meant decoding and a card was 7-27ms against a 16.7ms frame. Since
 * texload.c, asking only queues: game_get_tex hands the worker two paths and
 * returns, a card already in hand is ignored, so the whole window costs about
 * nothing and dribbling it out a frame at a time only made it land later. */
static void prime_toward(app *a, int s, int center)
{
	prime_around(a, s, center, CF_HALF_WINDOW);
}

static void prime_sys_window(app *a)
{
	for (int i = 0; i < a->sys.count; i++) {
		int w, h;
		sys_get_tex(a, i, &w, &h, NULL);
	}
}

/* One shelf's card art, dropped.
 *
 * Every one of these arrays is indexed by POSITION in the list, so reordering
 * the list moves the games and leaves the art behind - each card would wear
 * the picture of whatever used to sit where it now sits.
 *
 * Decodes in flight are keyed by position too, and dropping the textures does
 * not reach them. Without the bump, one asked for under the old order landed
 * after the reorder and was installed at its old slot - another game's cover
 * on the card - and then stuck, because a slot with a texture is never asked
 * for again. free_all_textures always bumped; this, its only other way of
 * dropping art, did not, and a Sort By change was the one path through it. */
static void free_view_textures(sysview *v)
{
	int k;

	texload_bump();
	for (k = 0; k < v->list.count; k++) {
		if (v->tex && v->tex[k]) {
			SDL_DestroyTexture(v->tex[k]);
			v->tex[k] = NULL;
		}
		if (v->tw) v->tw[k] = 0;
		if (v->th) v->th[k] = 0;
		if (v->cb) v->cb[k] = 0.0f;
	}
}

/* Muse's shelf onto the other kind: music or books, from the first card. The
 * textures go first, while the count is still the old kind's, since every card
 * is about to be a different album. */
static void muse_show(sysview *v, bool books)
{
	g_muse_books = books;
	db_set_str(db_dev(), "muse.show", books ? "books" : "music");
	free_view_textures(v);
	muse_order_view(v);
	v->cursor = 0;
	cf_reset(&v->cf, 0);
}

static SDL_Texture *g_backdrop;   /* backdrop_cached */

static void free_all_textures(app *a)
{
	/* Anything in flight was asked for against the shelf as it was. */
	texload_bump();
	for (int i = 0; i < a->sys.count; i++) {
		if (a->sys_tex[i]) { SDL_DestroyTexture(a->sys_tex[i]); a->sys_tex[i] = NULL; }
		for (int k = 0; k < a->view[i].list.count; k++)
			if (a->view[i].tex[k]) {
				SDL_DestroyTexture(a->view[i].tex[k]);
				a->view[i].tex[k] = NULL;
			}
	}
	/* The Now Playing cover too. Native PICO-8's menu and run_alone drop the
	 * renderer after this, and a cover kept across that was the old
	 * renderer's texture, still marked done, so it was never loaded again:
	 * Now Playing without its art until the album changed (plorpos-gkd.65).
	 * Loading it again is one decode, about 15 ms. */
	np_forget();
	if (g_backdrop) { SDL_DestroyTexture(g_backdrop); g_backdrop = NULL; }
}


/* Whether the radio should come up at boot.
 *
 * Per-device rather than per-card, on the same split as brightness: it is what
 * THIS handheld was last doing. The shipped default seeds it once, so there is
 * no fallback chain left for launch.sh to walk.
 *
 * Written the moment it changes rather than at shutdown, because a handheld
 * is switched off by holding a button or by running the battery flat, and
 * neither of those is a chance to save anything. */
static void wifi_pref_save(bool on)
{
	db_set_int(db_dev(), "wifi", on ? 1 : 0);
	/* launch.sh reads this one before the launcher exists, so the export has
	 * to follow the write rather than wait for a tidy shutdown. */
	db_write_boot_env();
}



/* Diatom watches this file, and it is rewritten per launch: it is the set
 * minus what has already been earned, which only this side knows. Runtime
 * scratch, so per-device rather than shared. */
static void chv_active_path(char *out, size_t n)
{
	snprintf(out, n, "%s/cheevos-active.set", P_USERDATA);
}

/* Auto Sleep: how long without input before light sleep (screen off, CPU
 * awake - platform.c's plat_light_sleep). NextUI's "Screen timeout" exactly:
 * its ladder and its default (settings.cpp's screen_timeout_secs,
 * CFG_DEFAULT_SCREENTIMEOUTSECS). Seconds, 0 for never. The array and db key
 * keep their old auto_off/autooff names from when this row meant power-off. */
static const int AUTO_OFF[] = { 0, 30, 60, 120, 300, 600 };
#define AUTO_OFF_COUNT ((int)(sizeof AUTO_OFF / sizeof AUTO_OFF[0]))

static int auto_off_load(void)
{
	int v = db_get_int(db_dev(), "autooff", -1);
	return v >= 0 ? v : 60;                   /* NextUI's default: a minute */
}

static void auto_off_save(int seconds)
{
	db_set_int(db_dev(), "autooff", seconds);
}

/* Auto Off: the real, resume-into-game power-down, mirroring AUTO_OFF[]'s
 * shape rather than sharing it - mutually exclusive with Auto Sleep above,
 * matching NextUI, so the two get independent ladders and db keys even
 * though today they hold the same values. */
static const int AUTO_POWEROFF[] = { 0, 30, 60, 120, 300, 600 };
#define AUTO_POWEROFF_COUNT ((int)(sizeof AUTO_POWEROFF / sizeof AUTO_POWEROFF[0]))

static int auto_poweroff_load(void)
{
	int v = db_get_int(db_dev(), "autopoweroff", -1);
	return v >= 0 ? v : 0;                    /* the default: never */
}

static void auto_poweroff_save(int seconds)
{
	db_set_int(db_dev(), "autopoweroff", seconds);
}

/* Suspend Timeout: how long light sleep waits for the power button before
 * escalating into real suspend. NextUI's "Suspend timeout" exactly - its
 * sleep_timeout_secs ladder and CFG_DEFAULT_SUSPENDTIMEOUTSECS. No 0 on
 * purpose, as in NextUI: once the screen is off some escalation always
 * applies. Independent of both rows above. platform.c reads the same key. */
static const int SUSPEND_TIMEOUT[] = { 30, 60, 120, 300, 600 };
#define SUSPEND_TIMEOUT_COUNT ((int)(sizeof SUSPEND_TIMEOUT / sizeof SUSPEND_TIMEOUT[0]))


/* Defined with the Wi-Fi screen it began in, and used here because signing in
 * and syncing are the other two things that make the player wait. */
static void wait_panel(app *a, const char *heading, const char *msg);

/* Pull down what the account already holds for the loaded game, and mark
 * those earned locally so they are not watched, not re-announced, and not
 * submitted again.
 *
 * The account wins on what EXISTS; the local store wins on what is still
 * owed, because it is the only record of anything earned offline. Neither is
 * discarded. Silent when offline or not signed in - that is the ordinary
 * case, and it leaves the device working from what it knows. */
static int ra_merge_unlocks(const int *ids, int n)
{
	int i, added = 0;

	if (n <= 0 || chv_game() <= 0) return 0;

	/* Only ids the set actually has. The account carries entries the set does
	 * not - RetroAchievements' "Unknown Emulator" notice is one, and it went
	 * straight into the store the first time this ran. Recording those adds a
	 * row that can never be displayed and could later be submitted as a
	 * duplicate of something that was never an achievement. */
	for (i = 0; i < n; i++) {
		int k;

		for (k = 0; k < chv_count(); k++)
			if (chv_at(k)->id == ids[i]) break;
		if (k == chv_count()) continue;
		if (chv_note_earned(chv_game(), ids[i], true)) added++;
	}

	if (added) {
		chv_earned_save();
	}
	return added;
}

/* Send what is owed. Called once the game is over and the launcher has the
 * screen back - never from the wait loop.
 *
 * Anything that will not send stays pending and is tried again next time,
 * which is the offline queueing RA's own requirements ask for and the right
 * shape regardless: an unlock earned on a plane is still earned. */
static void ra_flush_unlocks(const char *rom, const char *tag, int cur)
{
	char hash[33] = "";
	int game, id, sent = 0, settled = 0;

	if (!ra_signed_in() || chv_pending_count() == 0 || !net_online()) return;

	/* The hash is what a real client sends alongside an award, and it is the
	 * one for the game just played - so only its own unlocks carry it. */
	ra_hash_rom(rom, tag, hash);

	/* Index 0 every time: a successful send marks the row synced, so the next
	 * pending one becomes index 0. A failure stops the loop rather than
	 * spinning on it - if one will not go, the rest will not either. */
	while (chv_pending_at(0, &game, &id)) {
		int rc = ra_submit_unlock(id, game == cur ? hash : NULL);

		/* 0 is "the account already had it", which settles the row just as
		 * surely as sending it. Only a real failure stops the loop - if one
		 * will not go, the rest will not either. */
		if (rc < 0) break;
		chv_mark_synced(game, id);
		settled++;
		if (rc > 0) sent++;
	}
	/* On `settled`, not on `sent`. A row the account already had is settled
	 * without being sent, and writing only when something was sent left that
	 * mark in memory only - so it came back pending on the next boot and was
	 * retried, forever. Seen on the device: one unlock submitted by hand
	 * outside the launcher, refused as already-held, and asked again at the
	 * end of every game after that. */
	if (settled) {
		chv_earned_save();
		fprintf(stderr, "ra: %d sent, %d already held, %d still queued\n",
		        sent, settled - sent, chv_pending_count());
	}
}

/* ---------- the unlock queue, sent behind the shelf ---------------------- */

/* ra_flush_unlocks, above, on a thread of its own.
 *
 * Quitting a game with something earned used to wait here, on the main thread,
 * between the game's wait loop and the shelf's - so the power button was read
 * by neither. It hashed the ROM, then sent each pending unlock one at a time,
 * each through a request with a 20s timeout: a second or two on a black screen
 * on a good network, and on a bad one a handheld ignoring its own power button
 * for minutes. Now the shelf comes straight back and the sending happens behind
 * it.
 *
 * The worker never touches the cheevos store. The main thread takes a snapshot
 * of what is owed and a copy of the account, hands both over, and later applies
 * the answers itself - chv_mark_synced and chv_earned_save only ever run here.
 * No lock guards the store because nothing but the main thread can reach it.
 *
 * Two things had to be made safe before this could exist. net_post_buf wrote
 * every request's config and response to one file per PROCESS, so a request
 * from here overlapping one from the main thread would have swapped them; it
 * is per call now. And ra_submit_unlock read the account straight from the
 * globals a sign-in rewrites, so the worker sends as a copy instead.
 *
 * If the thread cannot be started, ra_flush_unlocks runs as it always did. */

/* Most unlocks one pass carries. More than that owed - a long spell offline -
 * is sent by the pass that follows, which starts by itself once this one has
 * settled something. */
#define FLUSH_MAX 256

static SDL_Thread *g_fl_thread;
static SDL_mutex  *g_fl_lock;
static SDL_cond   *g_fl_wake;
static bool        g_fl_stop;

/* The job, written here and taken by the worker. */
static bool g_fl_job;                    /* one is waiting */
static bool g_fl_busy;                   /* the worker has one in hand */
static char g_fl_rom[LIB_PATH * 2], g_fl_tag[16];
static char g_fl_user[RA_USER_MAX], g_fl_token[RA_TOKEN_MAX];
static int  g_fl_cur;                    /* whose unlocks carry the ROM hash */
static int  g_fl_game[FLUSH_MAX], g_fl_id[FLUSH_MAX], g_fl_n;

/* The answers, written by the worker and applied here. */
static int  g_fl_rgame[FLUSH_MAX], g_fl_rid[FLUSH_MAX], g_fl_rrc[FLUSH_MAX];
static int  g_fl_rn;
static bool g_fl_finished;               /* the job in hand is done */
static bool g_fl_failed;                 /* it stopped on a real failure */

/* Main thread only. The latest game quit, for the pass that follows; and
 * whether the job in hand has settled anything, which is what guarantees a
 * follow-on pass makes progress rather than spinning.
 *
 * The game is carried, not read from chv_game() when the pass starts. A pass
 * can start from the shelf, where the info screen loads whichever set it is
 * showing - so chv_game() could name another game and hand it this ROM's
 * hash. */
static char g_fl_next_rom[LIB_PATH * 2], g_fl_next_tag[16];
static int  g_fl_next_cur;
static bool g_fl_again;
static int  g_fl_settled;

static int flush_worker(void *unused)
{
	(void)unused;
	SDL_LockMutex(g_fl_lock);
	for (;;) {
		char rom[LIB_PATH * 2], tag[16], user[RA_USER_MAX], token[RA_TOKEN_MAX];
		char hash[33] = "";
		int  game[FLUSH_MAX], id[FLUSH_MAX], n, cur, i;
		bool failed = false, hashed = false;

		while (!g_fl_stop && !g_fl_job) SDL_CondWait(g_fl_wake, g_fl_lock);
		if (g_fl_stop) break;
		memcpy(rom, g_fl_rom, sizeof rom);
		memcpy(tag, g_fl_tag, sizeof tag);
		memcpy(user, g_fl_user, sizeof user);
		memcpy(token, g_fl_token, sizeof token);
		n = g_fl_n;
		cur = g_fl_cur;
		memcpy(game, g_fl_game, (size_t)n * sizeof game[0]);
		memcpy(id, g_fl_id, (size_t)n * sizeof id[0]);
		g_fl_job = false;
		g_fl_busy = true;
		SDL_UnlockMutex(g_fl_lock);

		for (i = 0; i < n; i++) {
			int rc;

			/* Checked before each request, not only between jobs: a launcher
			 * on its way out should not start a fresh 20s wait. */
			SDL_LockMutex(g_fl_lock);
			if (g_fl_stop) { SDL_UnlockMutex(g_fl_lock); break; }
			SDL_UnlockMutex(g_fl_lock);

			/* The hash only for the game just played, and only once, since
			 * it reads the whole ROM. */
			if (game[i] == cur && !hashed) {
				ra_hash_rom(rom, tag, hash);
				hashed = true;
			}
			rc = ra_submit_unlock_as(user, token, id[i],
			                         game[i] == cur ? hash : NULL);

			/* Bounded although it cannot fill: a pass never starts while the
			 * answers to the last one are waiting (see ra_flush_start), so
			 * this holds at most this pass's n. An answer dropped here would
			 * only mean that unlock is sent again next time and settled as
			 * already held. */
			SDL_LockMutex(g_fl_lock);
			if (g_fl_rn < FLUSH_MAX) {
				g_fl_rgame[g_fl_rn] = game[i];
				g_fl_rid[g_fl_rn]   = id[i];
				g_fl_rrc[g_fl_rn]   = rc;
				g_fl_rn++;
			}
			SDL_UnlockMutex(g_fl_lock);

			/* A real failure stops the pass - if one will not go, the rest
			 * will not either. 0, "the account already had it", does not. */
			if (rc < 0) { failed = true; break; }
		}

		SDL_LockMutex(g_fl_lock);
		g_fl_busy = false;
		g_fl_finished = true;
		g_fl_failed = failed;
	}
	SDL_UnlockMutex(g_fl_lock);
	return 0;
}

static bool flush_started(void)
{
	if (g_fl_thread) return true;
	if (!(g_fl_lock = SDL_CreateMutex())) return false;
	if (!(g_fl_wake = SDL_CreateCond())) {
		SDL_DestroyMutex(g_fl_lock);
		g_fl_lock = NULL;
		return false;
	}
	if (!(g_fl_thread = SDL_CreateThread(flush_worker, "tortos-raflush", NULL))) {
		SDL_DestroyCond(g_fl_wake);  g_fl_wake = NULL;
		SDL_DestroyMutex(g_fl_lock); g_fl_lock = NULL;
		return false;
	}
	return true;
}

/* Hand what is owed to the worker and return at once. Main thread. */
static void ra_flush_start(const char *rom, const char *tag, int cur)
{
	int n = 0, game, id;

	if (!ra_signed_in() || chv_pending_count() == 0 || !net_online()) return;
	if (!flush_started()) { ra_flush_unlocks(rom, tag, cur); return; }

	/* Not copied when they already are these buffers: the follow-on pass hands
	 * them straight back in, and snprintf from a buffer into itself is
	 * undefined. */
	if (rom != g_fl_next_rom)
		snprintf(g_fl_next_rom, sizeof g_fl_next_rom, "%s", rom);
	if (tag != g_fl_next_tag)
		snprintf(g_fl_next_tag, sizeof g_fl_next_tag, "%s", tag);
	g_fl_next_cur = cur;

	SDL_LockMutex(g_fl_lock);
	/* Busy until its answers have been APPLIED, not merely until the worker
	 * finishes. A pass can finish during a game, and its answers then wait
	 * for the main loop. Starting another before they are applied would
	 * snapshot those same unlocks as still owed and send them twice, and would
	 * append a second pass's answers onto the first's. */
	if (g_fl_job || g_fl_busy || g_fl_finished || g_fl_rn > 0) {
		/* Note it instead; the pass that follows picks up everything still
		 * owed once these answers are in. */
		g_fl_again = true;
		SDL_UnlockMutex(g_fl_lock);
		return;
	}
	while (n < FLUSH_MAX && chv_pending_at(n, &game, &id)) {
		g_fl_game[n] = game;
		g_fl_id[n]   = id;
		n++;
	}
	snprintf(g_fl_rom, sizeof g_fl_rom, "%s", rom);
	snprintf(g_fl_tag, sizeof g_fl_tag, "%s", tag);
	ra_creds_copy(g_fl_user, sizeof g_fl_user, g_fl_token, sizeof g_fl_token);
	g_fl_cur = cur;
	g_fl_n = n;
	g_fl_settled = 0;
	g_fl_job = true;
	SDL_CondSignal(g_fl_wake);
	SDL_UnlockMutex(g_fl_lock);
}

/* Apply whatever the worker has answered, and start the next pass when one is
 * owed. Main thread, every pass of the main loop - it is a lock and a count
 * when there is nothing to do. Nothing here is drawn, so it asks for no frame. */
static void ra_flush_poll(void)
{
	int  rgame[FLUSH_MAX], rid[FLUSH_MAX], rrc[FLUSH_MAX], rn, i, sent = 0, settled = 0;
	bool finished, failed, again;

	if (!g_fl_thread) return;
	SDL_LockMutex(g_fl_lock);
	rn = g_fl_rn;
	memcpy(rgame, g_fl_rgame, (size_t)rn * sizeof rgame[0]);
	memcpy(rid, g_fl_rid, (size_t)rn * sizeof rid[0]);
	memcpy(rrc, g_fl_rrc, (size_t)rn * sizeof rrc[0]);
	g_fl_rn = 0;
	finished = g_fl_finished;
	failed = g_fl_failed;
	g_fl_finished = false;
	again = g_fl_again;
	if (finished) g_fl_again = false;
	SDL_UnlockMutex(g_fl_lock);

	for (i = 0; i < rn; i++) {
		if (rrc[i] < 0) continue;
		chv_mark_synced(rgame[i], rid[i]);
		settled++;
		if (rrc[i] > 0) sent++;
	}
	/* On settled, not on sent, for the reason ra_flush_unlocks gives. */
	if (settled) {
		g_fl_settled += settled;
		chv_earned_save();
		fprintf(stderr, "ra: %d sent, %d already held, %d still queued\n",
		        sent, settled - sent, chv_pending_count());
	}

	if (!finished) return;
	/* Another pass when a later game was quit during this one, or when this one
	 * ran out of room with more still owed. The second only if it settled
	 * something, so a pass that achieved nothing cannot keep starting itself. */
	if (again || (!failed && g_fl_settled > 0 && chv_pending_count() > 0))
		ra_flush_start(g_fl_next_rom, g_fl_next_tag, g_fl_next_cur);
}

/* Tell the worker to stop, and do not wait for it.
 *
 * Waiting could mean sitting out a request's 20s timeout on the way to exit or
 * power off. It needs no waiting: the worker touches nothing that shutdown
 * frees - no SDL object is destroyed here, it never reaches the store or the
 * database - so the process ending takes it cleanly. What it had not answered
 * stays owed and is sent next time; one it had sent but not yet reported is
 * sent again, and the account's "already has this achievement" settles it. */
static void ra_flush_stop(void)
{
	if (!g_fl_thread) return;
	SDL_LockMutex(g_fl_lock);
	g_fl_stop = true;
	SDL_CondSignal(g_fl_wake);
	SDL_UnlockMutex(g_fl_lock);
	SDL_DetachThread(g_fl_thread);
	g_fl_thread = NULL;
}

/* ---------- what the account already has, read while the game runs ------- */

/* The answer to "which of this game's achievements does the account hold",
 * asked at launch and read as soon as it lands - from the game tick during a
 * game, from the main loop on the shelf.
 *
 * It used to be read only when the game ended, waiting there until it had
 * landed. So a game's first session after signing in showed 0 of N in its own
 * menu while the account held 13, and a quit straight after a launch on a slow
 * network sat out the request between the game's wait loop and the shelf's.
 * Now the menu is right about half a second in, and quitting never waits.
 *
 * The unlocks owed still go out after the answer, not before it: settling what
 * the account already holds first is what stops them being sent as duplicates.
 * So a quit that beats the answer leaves the sending to whichever comes first
 * - the answer landing, or something else needing the one async slot, which
 * gives up on the answer and sends anyway. Duplicates that causes are answered
 * as already held, which settles them just the same. */
static long g_sync_game;                   /* the game asked about, 0 none */
static char g_sync_set[LIB_PATH * 2];      /* its set, to check the answer against */

/* A send waiting for the answer. */
static bool g_sync_flush;
static char g_sync_rom[LIB_PATH * 2], g_sync_tag[16];
static int  g_sync_cur;

static void sync_begin(long game, const char *set_path)
{
	ra_sync_begin(game);
	g_sync_game = ra_sync_pending() ? game : 0;
	snprintf(g_sync_set, sizeof g_sync_set, "%s", set_path ? set_path : "");
}

static void sync_flush_now(void)
{
	if (!g_sync_flush) return;
	g_sync_flush = false;
	ra_flush_start(g_sync_rom, g_sync_tag, g_sync_cur);
}

/* The answer, merged if it is in. Returns how many achievements it added to
 * what this device knew - nonzero means the list Diatom is watching still
 * holds some the account already has. Cheap when nothing has landed: a
 * waitpid that does not wait. */
static int sync_poll(void)
{
	int ids[CHV_MAX], n, rc, added = 0;

	if (!g_sync_game) return 0;
	rc = ra_sync_poll(ids, CHV_MAX, &n);
	if (rc == 0) return 0;

	if (rc > 0 && n > 0) {
		/* Against the set the question was about. During a game that is the
		 * one loaded; on the shelf the info screen loads whichever set it is
		 * showing, and nothing on the shelf depends on which one that is. */
		if (chv_game() != g_sync_game) chv_load(g_sync_set);
		if (chv_game() == g_sync_game) added = ra_merge_unlocks(ids, n);
	}
	g_sync_game = 0;
	sync_flush_now();
	return added;
}

/* Something else needs the one async slot now - a launch, the box art
 * scraper. The answer is dropped, and the next launch of that game asks
 * again. */
static void sync_abandon(void)
{
	if (g_sync_game) {
		ra_sync_abandon();
		g_sync_game = 0;
		fprintf(stderr, "ra: dropped an account answer that had not landed\n");
	}
	sync_flush_now();
}

/* Where the launcher's own work happens during a game. Ten times a second,
 * from inside the wait loop, and it must stay cheap: that loop is the power
 * button's watchdog.
 *
 * Its one job is the first play of a game. The set is being found behind the
 * game rather than in front of it, and when it lands the player is told the
 * only way anyone can be told during a game - over Diatom's overlay. */
static char g_pending_set[LIB_PATH * 2];
static char g_pending_active[LIB_PATH * 2];
/* The watch list Diatom was last handed and when, so an account answer that
 * lands early can trim it. 0 when there is no list to trim. */
#define WATCH_RETRIM_MS 5000
static unsigned g_watch_ms;
static char     g_watch_active[LIB_PATH * 2];
/* The set path of the game actually running, so a fetch that lands during a
 * DIFFERENT game can be recognized and dropped. Written at every launch. */
static char g_game_set[LIB_PATH * 2];

/* ---- where the system's sound goes (Diatom's ADR-0029) ------------------- */

/* The policy is a setting; the other two facts are read from the device. The
 * RULE that combines them is src/audioout.c, where it can be checked without a
 * handheld - this half only gathers. */
static aout_policy g_aout_policy;
static char g_aout_sent[128];      /* the device Diatom was last told to use */
static bool g_aout_ever;
#if !defined(PLATFORM_GKD)
static unsigned g_aout_gen;        /* which connection it was told on */
#endif
static unsigned g_aout_retry_at;   /* earliest next in-game retry of a fallback */
static int      g_aout_retries;    /* spent on the game running now */
static bool     g_aout_fallen;     /* a fallback already noticed, so not new */

static aout_policy aout_load(void)
{
	char v[32];
	/* The default follows the cable; only an explicit "speaker" overrides. */
	db_get_str(db_dev(), "audioout", v, sizeof v, "auto");
	return !strcmp(v, "speaker") ? AOUT_SPEAKER : AOUT_AUTO;
}

static void aout_save(aout_policy p)
{
	db_set_str(db_dev(), "audioout", p == AOUT_SPEAKER ? "speaker" : "auto");
}

/* The connected sink, published by bt_reconnect in launch.sh.
 *
 * A file rather than asking Bluetooth ourselves: asking means forking
 * bluetoothctl out of a 119 MB process, which is what menu_wifi exists to
 * prevent. The shell loop already knows, so it writes and this reads. */
#if !defined(PLATFORM_GKD)
static char g_bt_link[32];    /* the published link's ACL handle, see bt_reconnect */
#endif

static const char *aout_bt_sink(void)
{
	static char   sink[128];
#if defined(PLATFORM_GKD)
	/* A label only: PipeWire holds the sink, see plat_bt_audio. */
	snprintf(sink, sizeof sink, "%s", plat_bt_audio() ? "bluetooth" : "");
	return sink;
#else
	static unsigned last;
	static bool   primed;
	unsigned now = plat_now_ms();
	FILE *f;

	/* Half a second. Plugging a cable in should feel immediate; a stat that
	 * often is nothing, but there is no reason to do it 120 times a second. */
	if (primed && now - last < 500) return sink;
	primed = true;
	last = now;
	sink[0] = g_bt_link[0] = '\0';
	f = fopen("/tmp/tortos_btsink", "r");
	if (f) {
		if (fgets(sink, sizeof sink, f)) sink[strcspn(sink, "\r\n")] = '\0';
		if (fgets(g_bt_link, sizeof g_bt_link, f))
			g_bt_link[strcspn(g_bt_link, "\r\n")] = '\0';
		fclose(f);
	}
	return sink;
#endif
}

static aout_state aout_now(void)
{
	aout_state s;

	s.policy  = g_aout_policy;
	s.wired   = plat_headphones_present();
#if defined(PLATFORM_GKD)
	/* Speaker overrides a cable on the GKD, whose amps it can set. */
	if (g_aout_policy == AOUT_SPEAKER) s.wired = false;
#endif
	s.bt_sink = aout_bt_sink();
	return s;
}

/* Where Diatom actually IS, which is not always where it was told to go: a
 * sink that will not open, or one that died under it, makes the port fall back
 * and report the fallback (ADR-0029). A non-empty device is the named sink; an
 * empty one is the codec, where the cable decides what you hear.
 *
 * Falls back to the launcher's own resolution until Diatom has said anything -
 * on the desktop build it never will. */
static aout_dest aout_actual(const aout_state *s)
{
#if defined(PLATFORM_GKD)
	/* PipeWire moves every stream with the default, so asked is actual. */
	return aout_resolve(s);
#else
	char at[128];
	const char *muse = musec_sink_now();

	/* Muse holding the headset is where the sound is, whatever Diatom says:
	 * Diatom was moved off it to let Muse have it. */
	if (musec_heard() && muse[0] && strcmp(muse, "default")) return AOUT_BT;
	if (!plat_resident_audio(at, sizeof at)) return aout_resolve(s);
	if (at[0]) return AOUT_BT;
	return s->wired ? AOUT_WIRED : AOUT_SPK;
#endif
}

/* SETAUDIO, after which Diatom's last report no longer counts until it has
 * answered this one. Reports are only read while a game runs, so at the shelf
 * the one held was from the last game: switching to Speaker there, or with
 * Muse on a headset, the Audio Output row went on saying "bluetooth" while the
 * sound came out of the speaker - #45, 2026-09-26. Until the answer, the row
 * shows what was asked; after it, what happened, fallback included. */
static bool aout_send(const char *dev)
{
	if (!plat_resident_line("SETAUDIO\tdevice=%s", dev)) return false;
	plat_resident_audio_asked();
	return true;
}

#if !defined(PLATFORM_GKD)
/* Send only on a change. SETAUDIO reopens an audio device, which is cheap but
 * not free, and doing it every tick would be a reopen per tick. */
static void aout_tell_diatom(const aout_state *s, const char *dev, bool force,
                             bool muse_has_it)
{
	unsigned gen = plat_resident_generation();

	/* Send when the ASKED-FOR device changes, or when the emulator is a new
	 * one. Never because Diatom ended up somewhere else.
	 *
	 * An earlier version compared against what Diatom REPORTED, so that a
	 * restarted emulator would be re-told. That is unrecoverable on a
	 * fallback: a headset switched off makes Diatom report the default while
	 * this side still wants the sink, the two can never agree, and the resend
	 * fires every tick. Measured 2026-09-05 - 413 route decisions and 335
	 * device reopens in one session, which left the codec parked in SETUP and
	 * the game silent on a speaker that was technically open.
	 *
	 * The restart case is covered by the generation instead, which changes
	 * exactly once per connection rather than continuously. */
	if (!force && g_aout_ever && gen == g_aout_gen && !strcmp(dev, g_aout_sent))
		return;
	g_aout_gen = gen;
	/* Only remember it as sent if it actually went. dsend does nothing when
	 * the socket is not open, and returns false saying so - which on the shelf
	 * before the first game is every time. Recording it as sent anyway meant
	 * the next call compared equal and never retried, so the route the log
	 * announced was one Diatom had never been told about. */
	if (!aout_send(dev)) return;

	snprintf(g_aout_sent, sizeof g_aout_sent, "%s", dev);
	g_aout_ever = true;
	/* Diatom's answer to this lands before a retry judges it. The previous
	 * device's report is still the one held until then. */
	g_aout_retry_at = plat_now_ms() + 5000;
	/* Logged because it only happens on a real change - a cable, a headset,
	 * or the row - and because "where is the sound going" is otherwise
	 * invisible after the fact. Diatom logs its own fallback, so the pair of
	 * lines says both what was asked for and what happened. */
	fprintf(stderr, "audio: %s -> %s%s\n",
	        aout_dest_name(aout_resolve(s)), dev[0] ? dev : "default",
	        muse_has_it ? " (Muse has the headset)" : "");
}
#endif

/* The headset's own volume, kept at the Brick's level while sound goes to one.
 *
 * The volume keys move the codec, which a Bluetooth route bypasses, so on a
 * headset they did nothing - and a headset with no buttons of its own, the
 * OpenFit or AirPods, had no volume anywhere. With btplayer registered, BlueZ
 * passes a volume on to the headset over AVRCP (tools/btplayer.c), so the
 * level is mirrored there: in a game the one Diatom last reported, since it
 * owns the keys and this side only applies its levels when the game ends;
 * otherwise this side's own. 0..20 onto AVRCP's 0..127, straight, because
 * AVRCP volume is a plain proportion and the headset applies its own curve.
 *
 * Sent on a change, and again for a new connection so a headset starts at
 * the Brick's level. One way only: a headset's own buttons move the headset
 * and not this level. A send that fails - no control yet, right after a
 * connect - waits two seconds before the next, because each is a mixer open. */
static bool g_game_ticking;     /* inside on_game_tick: a game is running */

#if !defined(PLATFORM_GKD)   /* PipeWire: no hand-over, no headset volume */
static void bt_volume_follow(const char *out, bool fresh)
{
	static int      sent = -1;
	static char     sent_to[64];     /* the headset it went to */
	static unsigned retry_at, looked, sent_at;
	static long     seen;            /* the stamp of the last report read */
	static bool     primed;
	int count = 0, level = plat_resident_volume(&count), v;
	unsigned now = plat_now_ms();
	struct stat st;

	if (!out[0]) { sent = -1; sent_to[0] = '\0'; return; }

	/* The other way first: the headset's OWN buttons, which btplayer passes on
	 * as `<PCM name> <0..127> <stamp>` (tools/btplayer.c). Adopted as the Brick's
	 * level - through Diatom in a game, which owns the level while it runs -
	 * and recorded as sent, so it is not echoed back to the headset. A new
	 * stamp is a new report - not a new inode, which tmpfs takes from a pool
	 * and can hand out again, and presses went missing until the next one.
	 * The report found at start is only noted, or a value left from before a
	 * restart would move the level. A tenth of a second between looks.
	 *
	 * NOT within a second of this side sending that headset a volume: it
	 * reports what it was sent, too - the OpenRun answered every set with its
	 * own announcement, measured 2026-09-26 - and sometimes not quite the same
	 * number (sent 32, back came 28), so adopting the echo stepped the level
	 * back and forth. A real press that close to a key press costs one step. */
	if (now - looked >= 100) {
		FILE *f = stat("/tmp/tortos_btvol", &st) == 0 ? fopen("/tmp/tortos_btvol", "r") : NULL;
		char who[64];
		int  hv, want;
		long stamp;

		looked = now;
		if (f && fscanf(f, "%63s %d %ld", who, &hv, &stamp) == 3 && stamp != seen) {
			bool fresh_report = primed;

			seen = stamp;
			primed = true;
			if (fresh_report && !strcmp(who, out) && hv >= 0 && hv <= 127) {
				bool echo = !strcmp(who, sent_to) && now - sent_at < 1000;
				int  have = g_game_ticking && level >= 0 ? level : plat_volume_get();

				want = (hv * PLAT_VOL_MAX + 63) / 127;
				if (!echo && want != have) {
					if (g_game_ticking)
						plat_resident_line("SETLEVEL\tkind=volume\tindex=%d\tcount=%d",
						                   want, PLAT_VOL_MAX + 1);
					else
						plat_volume_nudge(want - have);
					sent = (want * 127 + PLAT_VOL_MAX / 2) / PLAT_VOL_MAX;
					snprintf(sent_to, sizeof sent_to, "%s", out);
					fprintf(stderr, "audio: %s's own buttons: %d/127, level %d of %d\n",
					        out, hv, want, PLAT_VOL_MAX);
				}
			}
		}
		if (f) fclose(f);
		if (!primed) primed = true;               /* no file yet is a start too */
	}
	if (level < 0) { level = plat_volume_get(); count = PLAT_VOL_MAX + 1; }
	if (level < 0 || count < 2) return;
	v = (level * 127 + (count - 1) / 2) / (count - 1);
	/* And again for a different headset at the same level: with two connected
	 * the sound can move between them, and a link handle is no help there - it
	 * is reused (#44). */
	if (!fresh && v == sent && !strcmp(out, sent_to)) return;
	if ((int)(now - retry_at) < 0) return;
	if (!btvol_set(out, v)) { retry_at = now + 2000; return; }
	sent = v;
	sent_at = now;
	snprintf(sent_to, sizeof sent_to, "%s", out);
	/* With its cost, because it is a mixer open on the loop that draws. */
	fprintf(stderr, "audio: %s volume %d/127 (level %d of %d) in %u ms\n",
	        out, v, level, count - 1, plat_now_ms() - now);
}
#endif

/* Where the system's sound goes, for both players.
 *
 * On the speaker and the jack both simply open the default, which is dmix.
 * A Bluetooth headset cannot be shared - bluealsa 3.1 gives an A2DP PCM to one
 * client at a time, measured 2026-09-25 - so it is HANDED between them, and it
 * goes to whoever may be heard: Muse while it plays, which is exactly when
 * ADR-0032 has already quieted the game, and Diatom otherwise.
 *
 * Release before acquire, or the second open fails and falls back. Towards
 * Muse, Diatom is told first and Muse's SINK retries for a second while Diatom
 * lets go, because at the shelf nothing here reads Diatom's answers. Back
 * towards Diatom, Muse is told first and Diatom is only told once Muse has
 * answered, because Diatom does not retry. */
static void aout_apply(bool force)
{
#if defined(PLATFORM_GKD)
	/* No hand-over: PipeWire mixes, and platform_gkd.c picks the sink. */
	(void)force;
	plat_audio_speaker_only(g_aout_policy == AOUT_SPEAKER);
#else
	static char  link_seen[32];
	aout_state  s    = aout_now();
	const char *out  = aout_device(&s);
	bool        muse = out[0] && musec_heard();
	/* A new connection under the same name - the headset dropped and came
	 * back between two looks at it. Nothing else here can see that, and
	 * whoever fell back to the speaker while it was gone would stay there:
	 * seen 2026-09-25, a song in Muse. So the holder is told again, once.
	 * Harmless when it never left: Diatom and Muse both ignore a device they
	 * are already on. */
	bool        fresh = out[0] && g_bt_link[0] && strcmp(g_bt_link, link_seen);

	if (!muse) {
		musec_sink("");
		if (out[0] && !musec_sink_settled()) return;    /* Muse is still letting go */
	}
	if (fresh) {
		fprintf(stderr, "audio: %s is a new connection (link %s)\n", out, g_bt_link);
		if (muse) musec_sink_again();
	}
	snprintf(link_seen, sizeof link_seen, "%s", out[0] ? g_bt_link : "");
	aout_tell_diatom(&s, muse ? "" : out, force || (fresh && !muse), muse);
	if (muse) musec_sink(out);
	bt_volume_follow(out, fresh);
#endif
}

/* The hook musec calls just before a PLAY or RESUME, so the track starts on
 * the headset rather than reaching it a tick later. */
static void aout_before_muse(void)
{
	aout_apply(false);
}

/* Muse's own screens, each frame. They route only while music is heard: a
 * headset that connects mid-song takes the song there and then, where before
 * it stayed on the speaker until something new was played - seen 2026-09-25,
 * resume and R1 both went to the speaker right after a pairing. Paused, they
 * still leave the headset where it is; nothing else can be heard on these
 * screens, so handing it back on every pause would only be churn. */
static void muse_screen_poll(void)
{
	muse_poll();
	if (musec_heard()) aout_apply(false);
}

/* A sink that failed once is otherwise never tried again. aout_apply sends on
 * a change of the ASKED-FOR device, and a headset that would not open, then
 * became usable under the same name, is no change at all: 2026-09-24 left
 * Diatom on the speaker through a game with the headset connected and working.
 *
 * Retried where it cannot be seen, and rarely where it can. A failed SETAUDIO
 * is not free: Diatom closes, fails the sink and reopens the default on its
 * main thread. Measured 2026-09-24 in Contra, 17 switches, 15 of them failed
 * fakes: 56.00 fps against 59.95, ~351 frames short, 18 resyncs against 3.
 * About 20 frames - a third of a second of frozen game - per failed try, and
 * that is the lower bound, because a fake name fails before bluealsa is ever
 * asked. A retry a minute would be a stutter a minute. */

/* Whether Diatom is on the default device although it was last told to use a
 * sink. The default and not merely "somewhere else", because the default is
 * the only place a fallback goes, and a report naming another sink is one that
 * has not caught up with the last SETAUDIO yet. Testing for any mismatch fired
 * a retry in the same tick as the send, before Diatom had answered - measured
 * on the device 2026-09-24, and it spent one of the two stalls on nothing.
 *
 * Only true to life while a game runs: at the shelf nothing reads Diatom's
 * reports - they are dropped at the next game start - so there it describes
 * the last game. */
static bool aout_fell_back(void)
{
	char at[128];

	if (!g_aout_ever || !g_aout_sent[0]) return false;
	if (!plat_resident_audio(at, sizeof at)) return false;
	return at[0] == '\0';
}

/* Just before a game is handed over. Unconditional, because the shelf cannot
 * know whether it is needed and it costs nothing when it is not: Diatom
 * returns early on the device it is already using, without reopening. When
 * the sink is still broken, the stall is spent inside the launch. */
static void aout_before_launch(void)
{
	g_aout_retries = 0;
	g_aout_fallen = false;
	g_aout_retry_at = plat_now_ms() + 5000;
	if (!g_aout_ever || !g_aout_sent[0]) return;
	if (aout_fell_back())
		fprintf(stderr, "audio: trying %s again before the launch\n", g_aout_sent);
	aout_send(g_aout_sent);
}

/* Twice per game at most: for a headset whose A2DP stream was not up yet when
 * the game asked for it, or one that dropped out and came back under the same
 * name. Spaced so that Diatom's answer to the first has arrived before the
 * second is judged. */
static void aout_retry_in_game(void)
{
	bool fell = aout_fell_back();

	/* Never in the moment a fallback is noticed. A sink that has just died is
	 * certainly still dead: a headset disconnected mid-game on 2026-09-24 had
	 * its first retry fire within the same second and fail, and only the
	 * second, after bt_reconnect brought it back 18s later, landed. */
	if (fell && !g_aout_fallen) {
		unsigned soonest = plat_now_ms() + 5000;

		if ((int)(soonest - g_aout_retry_at) > 0) g_aout_retry_at = soonest;
	}
	g_aout_fallen = fell;

	if (!fell || g_aout_retries >= 2) return;
	if ((int)(plat_now_ms() - g_aout_retry_at) < 0) return;
	if (!aout_send(g_aout_sent)) return;
	g_aout_retries++;
	g_aout_retry_at = plat_now_ms() + 25000;
	fprintf(stderr, "audio: %s fell back; trying again (%d of 2)\n",
	        g_aout_sent, g_aout_retries);
}


static void on_game_tick(void)
{
	char msg[96];

	int rc;

	/* Muse, bounded: read what has arrived, and send at most one PLAY when a
	 * track ends - which is what keeps an album going through a game.
	 *
	 * And the game goes quiet while it plays - Diatom's ADR-0032, followed
	 * here at ten a second, which is how pausing the music or the album
	 * ending brings the game's sound back within a tenth of a second. Only a
	 * change is sent. */
	muse_poll();
	plat_resident_quiet(musec_playing());

	/* Play time. Writes at most one unsynced row and usually nothing, and
	 * never an fsync: measured on the card, an fsync is 1.56 ms median but
	 * 16.4 ms at the tail, which is a dropped frame. */
	stats_tick(plat_now_ms());

	/* A cable plugged in mid-game, or a headset that connected or walked out
	 * of range, moves the sound without leaving the game. That is the whole
	 * point of ADR-0029 making this a state rather than a launch argument. */
	g_game_ticking = true;             /* see bt_volume_follow */
	aout_apply(false);
	g_game_ticking = false;
#if defined(PLATFORM_GKD)
	plat_sink_follow(true);
#endif
	aout_retry_in_game();


	/* The account's answer, as soon as it lands - see sync_poll.
	 *
	 * When it adds achievements this device did not know were earned, the list
	 * Diatom is watching still has them in it, and one of them firing reads as
	 * a fresh unlock. So the list is handed over again without them - but only
	 * early in a session. Reloading a set starts every partly-met achievement
	 * over (Diatom's diatom_cheevos_load re-initializes the runtime), and
	 * WATCH_RETRIM_MS is ten times the measured answer and still the opening of
	 * a session. Later than that the count is still put right; a repeat unlock
	 * is sent and answered as already held.
	 *
	 * Before the return below, which is about a set still being fetched: the
	 * two never share the slot, because this game's question is only asked
	 * once its set has arrived. */
	if (sync_poll() > 0 && g_watch_ms &&
	    plat_now_ms() - g_watch_ms <= WATCH_RETRIM_MS) {
		if (chv_write_active(g_watch_active))
			plat_resident_line("SETCHEEVOS\tpath=%s\tconsole=%d",
			                   g_watch_active, chv_console());
		else                           /* all of it held: watch nothing */
			plat_resident_line("SETCHEEVOS\tpath=\tconsole=%d", chv_console());
		g_watch_ms = 0;
		fprintf(stderr, "cheevos: the account held some already; watching the rest\n");
	}

	if (!g_pending_set[0]) return;

	/* Once. Calling it twice advances the state machine twice, which is a
	 * lookup answered and then thrown away. */
	rc = ra_fetch_step();
	if (rc == 0) return;                        /* still working */
	if (rc < 0) { g_pending_set[0] = '\0'; return; }  /* RA has never seen it */

	/* A FETCH CAN OUTLIVE THE GAME IT WAS FOR, and handing over the wrong set
	 * is not a cosmetic mistake. This runs on the game tick, so the game that
	 * started the fetch may have ended and another may be running - and below,
	 * SETCHEEVOS hands the set to Diatom mid-game, which ADR-0026 makes the
	 * normal path precisely because a download can still be in flight.
	 *
	 * Nothing downstream would catch it. rcheevos would evaluate one game's
	 * conditions against another game's memory, and a condition that matched
	 * by coincidence would unlock an achievement the player never earned, in a
	 * game they were not playing, and on_cheevo_unlocked would queue it for
	 * submission to their account.
	 *
	 * Seen 2026-09-10: launching Advance Guardian Heroes drew the set notice
	 * for A Boy and His Blob, an NES game fetched earlier the same evening.
	 * The active set was not poisoned that time, which is luck, not a guard.
	 *
	 * Dropped rather than canceled: there is no ra_fetch_abort, and
	 * ra_fetch_step only runs while g_pending_set is set, so clearing it on
	 * game end - the fix this looked like it wanted - would abandon a live
	 * state machine with nobody to drive it. The fetch has already written the
	 * set to its own cache by now, so the next launch of THAT game finds it
	 * waiting. A stale fetch becomes a cache warm instead of a hazard. */
	if (strcmp(g_pending_set, g_game_set) != 0) {
		fprintf(stderr, "cheevos: set arrived for a game that is no longer "
		                "running; cached, not applied\n");
		g_pending_set[0] = '\0';
		return;
	}

	if (!chv_load(g_pending_set)) { g_pending_set[0] = '\0'; return; }
	sync_begin(chv_game(), g_pending_set);   /* read on a later tick - sync_poll */

	if (chv_write_active(g_pending_active)) {
		plat_resident_line("SETCHEEVOS	path=%s	console=%d",
		                   g_pending_active, chv_console());
		g_watch_ms = plat_now_ms();
		snprintf(g_watch_active, sizeof g_watch_active, "%s", g_pending_active);
		snprintf(msg, sizeof msg, "%d to earn", chv_count());
	} else {
		snprintf(msg, sizeof msg, "all %d already earned", chv_count());
	}

	/* Rendering is safe here; presenting would not be. Same rule as an
	 * unlock notice, and the same path. */
	{
		char p[CFG_STR * 2];

		snprintf(p, sizeof p, "%s/notice.dtov", P_USERDATA);
		if (notice_render(msg, chv_game_title(), p))
			plat_resident_line("OVERLAY	path=%s	ms=3500", p);
	}
	g_pending_set[0] = '\0';
}

/* Called from inside plat_resident_wait, mid-game, when the launcher owns
 * nothing and can draw nothing. Recording is all that happens here; the
 * telling happens the next time this process has the screen. */
static void on_cheevo_unlocked(int id)
{
	char p[CFG_STR * 2];
	const cheevo *c = NULL;
	int i;

	if (!chv_note_unlock(id)) return;

	/* Written through immediately rather than at exit. A game ends by power
	 * button as often as by menu, and an achievement lost to a flat battery is
	 * one the player has to earn twice. */
	chv_earned_save();

	/* And say so, on screen, now. This process owns no display while a game
	 * runs, so it renders the line and Diatom composites it (its ADR-0027).
	 * Rendering is safe here; presenting would not be. */
	for (i = 0; i < chv_count(); i++)
		if (chv_at(i)->id == id) { c = chv_at(i); break; }

	{
		char head[64];

		/* Points on the context line, not the name: "First Blood!" is what
		 * the player wants to read, and "1 point" is what it was worth. */
		if (c && c->points > 0)
			snprintf(head, sizeof head, "Unlocked  -  %d point%s",
			         c->points, c->points == 1 ? "" : "s");
		else
			snprintf(head, sizeof head, "Unlocked");

		snprintf(p, sizeof p, "%s/notice.dtov", P_USERDATA);
		if (notice_render(head, c && c->title[0] ? c->title : "Unlocked", p))
			plat_resident_line("OVERLAY\tpath=%s\tms=4000", p);
	}
}

/* The Screenshot hotkey's file reached the card (plorpos-gkd.86.2). Like an
 * unlock: mid-game, so rendered here and composited by Diatom - which waits
 * until the file is written before it says so, so the notice is never in it. */
static void on_shot(bool ok, const char *path)
{
	const char *name = strrchr(path, '/');
	char p[CFG_STR * 2];

	snprintf(p, sizeof p, "%s/notice.dtov", P_USERDATA);
	if (notice_render(ok ? "Screenshot saved" : "Screenshot failed",
	                  name ? name + 1 : path, p))
		plat_resident_line("OVERLAY\tpath=%s\tms=2000", p);
}

/* ---------- where you were ------------------------------------------------ */

/* Coming back to the shelf you left is worth four lines of file handling: the
 * launcher restarts after every game, and starting at the beginning of the
 * list every time would undo the point of it being quick. */
static void remember_place(app *a)
{
	char p[CFG_STR * 2];
	sysview *v = &a->view[a->sys_cursor];
	FILE *f;
	snprintf(p, sizeof p, "%s/.last", P_ROOT);
	f = atomic_open(p, 0644);
	if (!f) return;
	fprintf(f, "%s\n%s\n", a->sys.systems[a->sys_cursor].tag,
	        v->list.count ? v->list.items[v->cursor].file : "");
	atomic_commit(f, p);
}

/* Whether a game was running when this process last stopped.
 *
 * `.last` says what you were LOOKING at, which is not the same question - it
 * is written when the shelf moves as well as when a game starts, and it says
 * nothing about whether that game was still up. This is written at launch and
 * cleared only on a clean return to the shelf, so a power-off mid-game and a
 * quit are distinguishable, which is the whole point.
 *
 * Beside `.last` and in the same shape, because it is the same two facts. */
static void playing_path(char *out, size_t n)
{
	snprintf(out, n, "%s/.playing", P_ROOT);
}

static void playing_set(app *a)
{
	char p[CFG_STR * 2];
	sysview *v = &a->view[a->sys_cursor];
	int o = shelf_owner(a, a->sys_cursor, v->cursor);
	FILE *f;

	if (v->list.count == 0) return;
	playing_path(p, sizeof p);
	f = atomic_open(p, 0644);
	if (!f) return;
	/* The GAME's system, not the shelf's - launching from Favorites otherwise
	 * records a system that does not own the ROM, exactly as the launch path
	 * itself had to learn. */
	fprintf(f, "%s\n%s\n", a->sys.systems[o].tag, v->list.items[v->cursor].file);
	atomic_commit(f, p);
}

static void playing_clear(void)
{
	char p[CFG_STR * 2];

	playing_path(p, sizeof p);
	remove(p);
}

/* Point the cursor at whatever was being played, if anything was, and say so.
 * Only when there is a state to come back to: without one, "resume" would
 * mean starting the game from its title screen, which is not what anyone
 * powering off mid-game is asking for. */
static bool playing_restore(app *a)
{
	char p[CFG_STR * 2], tag[64] = { 0 }, file[LIB_PATH] = { 0 }, st[LIB_PATH * 2];
	FILE *f;
	int i, k;

	playing_path(p, sizeof p);
	f = fopen(p, "r");
	if (!f) return false;
	if (fgets(tag, sizeof tag, f)) tag[strcspn(tag, "\r\n")] = 0;
	if (fgets(file, sizeof file, f)) file[strcspn(file, "\r\n")] = 0;
	fclose(f);
	if (!tag[0] || !file[0]) { playing_clear(); return false; }

	for (i = 0; i < a->sys.count; i++) {
		if (strcmp(a->sys.systems[i].tag, tag) != 0) continue;
		for (k = 0; k < a->view[i].list.count; k++) {
			if (strcmp(a->view[i].list.items[k].file, file) != 0) continue;

			state_path(a, i, &a->view[i].list.items[k], st, sizeof st);
			if (access(st, R_OK) != 0) {
				/* The game is gone from under the state, or the autosave
				 * never landed. Clear the marker rather than trying every
				 * boot from here on. */
				playing_clear();
				return false;
			}
			a->sys_cursor = i;
			a->view[i].cursor = k;
			return true;
		}
	}
	playing_clear();                  /* the ROM or its system is no longer here */
	return false;
}

static void restore_place(app *a)
{
	char p[CFG_STR * 2], tag[64] = { 0 }, file[LIB_PATH] = { 0 };
	FILE *f;
	snprintf(p, sizeof p, "%s/.last", P_ROOT);
	f = fopen(p, "r");
	if (!f) return;
	if (fgets(tag, sizeof tag, f)) tag[strcspn(tag, "\r\n")] = 0;
	if (fgets(file, sizeof file, f)) file[strcspn(file, "\r\n")] = 0;
	fclose(f);
	for (int i = 0; i < a->sys.count; i++) {
		if (strcmp(a->sys.systems[i].tag, tag) != 0) continue;
		a->sys_cursor = i;
		for (int k = 0; k < a->view[i].list.count; k++)
			if (strcmp(a->view[i].list.items[k].file, file) == 0) {
				a->view[i].cursor = k;
				break;
			}
		return;
	}
}

/* ---------- drawing ------------------------------------------------------- */

/* One cell of the shell: a filled hexagon as a six-triangle fan, because
 * SDL_RenderGeometry is the only primitive here that antialiases nothing and
 * therefore looks identical to the boot animation's rasterizer. */
static void draw_hex(SDL_Renderer *r, float cx, float cy, float rad, SDL_Color col)
{
	SDL_Vertex v[7];
	int idx[18], i;
	v[0].position.x = cx; v[0].position.y = cy;
	for (i = 0; i < 6; i++) {
		float a = (float)(M_PI / 6.0 + i * M_PI / 3.0);
		v[i + 1].position.x = cx + rad * cosf(a);
		v[i + 1].position.y = cy + rad * sinf(a);
	}
	for (i = 0; i < 7; i++) {
		v[i].color = col;
		v[i].tex_coord.x = v[i].tex_coord.y = 0;
	}
	for (i = 0; i < 6; i++) {
		idx[i * 3 + 0] = 0;
		idx[i * 3 + 1] = 1 + i;
		idx[i * 3 + 2] = 1 + (i + 1) % 6;
	}
	SDL_RenderGeometry(r, NULL, v, 7, idx, 18);
}

/* The mark, drawn about its own center.
 *
 * `head` slides the dark cell back along the lattice: 1.0 is where it sits at
 * rest, 0.0 is home on the center. `dim` then fades that same dark over the
 * blue center cell, 0 to 1.
 *
 * The two are separate because at head = 0 the dark cell lands exactly on the
 * center and is drawn UNDER the blue, so on its own the head just disappears.
 * `dim` is what makes its arrival visible: the blue is the one lit thing in
 * either mark - the same color as the boot line and the menu chrome - so
 * covering it is the light going out, and the shell closes dark. */
/* A heart, as a fan over the classic parametric curve:
 *
 *     x = 16 sin^3 t
 *     y = 13 cos t - 5 cos 2t - 2 cos 3t - cos 4t
 *
 * Mirrors markdef.heart(), which is the definition - tools/genfavcard.py draws
 * the same shape into res/cards/FAVORITES.png, and the two are meant to match.
 * Change one, change both. Same standing arrangement as draw_shell and CELLS.
 *
 * Fanned from a point BELOW the center rather than from the center itself. The
 * notch between the lobes is the one concave part of the shape, and a fan
 * apex level with it produces slivers that cross the notch and fill it in.
 * Dropping the apex puts every boundary point in view of it. Same primitive as
 * draw_hex for the same reason: SDL has no polygon fill.
 */
#define HEART_N 48

static void draw_heart(SDL_Renderer *r, float cx, float cy, float rad,
                       SDL_Color col)
{
	SDL_Vertex v[HEART_N + 1];
	int idx[HEART_N * 3], i;

	v[0].position.x = cx;
	v[0].position.y = cy + rad * 0.25f;
	for (i = 0; i < HEART_N; i++) {
		float t = (float)(2.0 * M_PI * i / HEART_N);
		float x = 16.0f * powf(sinf(t), 3.0f);
		float y = 13.0f * cosf(t) - 5.0f * cosf(2 * t)
		          - 2.0f * cosf(3 * t) - cosf(4 * t);

		/* The curve spans 32 wide and 30 tall; halving the wider axis makes
		 * `rad` a radius in the same sense the star's was. */
		v[i + 1].position.x = cx + x * rad / 16.0f;
		v[i + 1].position.y = cy - y * rad / 16.0f;
	}
	for (i = 0; i <= HEART_N; i++) {
		v[i].color = col;
		v[i].tex_coord.x = v[i].tex_coord.y = 0;
	}
	for (i = 0; i < HEART_N; i++) {
		idx[i * 3 + 0] = 0;
		idx[i * 3 + 1] = 1 + i;
		idx[i * 3 + 2] = 1 + (i + 1) % HEART_N;
	}
	SDL_RenderGeometry(r, NULL, v, HEART_N + 1, idx, HEART_N * 3);
}

static void draw_shell(SDL_Renderer *r, float cx, float cy, float rad,
                       float head, float dim, Uint8 alpha)
{
	/* Mirrors tools/markdef.py, which is the definition. C cannot import it,
	 * so this is the one hand-kept copy: change one, change both. The ring
	 * alternates light, mid, light, mid, light, mid for three-fold symmetry. */
	static const struct { float i, j; Uint8 c[3]; } cells[] = {
		{ -0.5f, -1.0f, { 128, 176, 118 } }, {  0.5f, -1.0f, { 104, 138,  96 } },
		{  1.0f,  0.0f, { 128, 176, 118 } }, {  0.5f,  1.0f, { 104, 138,  96 } },
		{ -0.5f,  1.0f, { 128, 176, 118 } }, { -1.0f,  0.0f, { 104, 138,  96 } },
	};
	float dx = 1.7320508f * rad, dy = 1.5f * rad;
	SDL_Color col;
	int k;

	/* Head first, so the shell cells draw over it as it withdraws. */
	col.r = 61; col.g = 89; col.b = 67; col.a = alpha;
	draw_hex(r, cx + 2.0f * dx * head, cy, rad * 0.95f, col);
	for (k = 0; k < 6; k++) {
		col.r = cells[k].c[0]; col.g = cells[k].c[1]; col.b = cells[k].c[2];
		col.a = alpha;
		draw_hex(r, cx + cells[k].i * dx, cy + cells[k].j * dy, rad * 0.95f, col);
	}
	/* markdef.CYAN, and the same value as MENU_ACCENT below. */
	col.r = 61; col.g = 214; col.b = 255; col.a = alpha;
	draw_hex(r, cx, cy, rad * 0.95f, col);
	if (dim > 0.0f) {
		SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
		col.r = 61; col.g = 89; col.b = 67;
		col.a = (Uint8)(alpha * (dim > 1.0f ? 1.0f : dim));
		draw_hex(r, cx, cy, rad * 0.95f, col);
	}
}

/* The gauge, read at most every 5 s: each read is two sysfs files opened,
 * read and closed, and it is asked every frame something is drawn. */
static Uint32 g_batt_next;
static bool   g_batt_ok, g_batt_charging;
static int    g_batt_pct;

static void battery_poll(void)
{
	Uint32 now = SDL_GetTicks();
	if (g_batt_next == 0 || now >= g_batt_next) {
		g_batt_next = now + 5000;
		g_batt_ok = plat_battery(&g_batt_pct, &g_batt_charging);
	}
}

static bool battery_low(void)
{
	battery_poll();
	return g_batt_ok && !g_batt_charging && g_batt_pct <= BATT_LOW_PCT;
}

/* BEGIN PolyForm-Noncommercial-1.0.0 - NextUI-derived: keep-awake on a USB host, NextUI's PWR_preventAutosleep. See NOTICE. */
/* Charging, or attached to a computer (plat_usb_host) - cached. Both halves of Auto Off ask this - the shelf every
 * frame, the in-game tick ten times a second - and every call is two sysfs
 * files opened, read and closed. Two seconds is well inside the shortest
 * timeout anyone can set, so the lag is not observable. Same trick and the
 * same reasoning as battery_low() above.
 *
 * plat_battery leaves its out-parameter alone when it fails, so the answer is
 * written before the call rather than after it. */
static bool keep_awake(void)
{
	static unsigned last;
	static bool     awake, primed;
	unsigned now = plat_now_ms();

	/* Elapsed, not a deadline compare: unsigned subtraction is right across
	 * the wrap and `now >= next_check` is not. */
	if (!primed || now - last >= 2000) {
		primed = true;
		last = now;
		awake = false;
		plat_battery(NULL, &awake);
		/* NextUI's PWR_preventAutosleep, less its Keep Awake Over USB
		 * setting - a computer always counts (TortOS-2pv). */
		if (!awake) awake = plat_usb_host();
	}
	return awake;
}
/* END PolyForm-Noncommercial-1.0.0 */

/* Has the player been away long enough for the armed idle action - Auto
 * Sleep's light sleep or Auto Off's shutdown, never both?
 *
 * The gathering half; the policy is idle_check, in src/idle.c, where a check
 * can reach it. This part is the three things only the launcher knows: the
 * setting, whether any button is down, and whether the charger is in.
 *
 * Every screen the launcher draws polls input in its own loop, and Auto Off
 * lived in exactly one of them: the shelf. The other seven simply held the
 * device on - both menus, Wi-Fi, About, Cheevos, the slot strip and the
 * keyboard. A menu left open is not evidence that somebody is there, it is
 * evidence somebody WAS; pausing because something else came up is the most
 * ordinary way there is to walk away from a device still switched on.
 *
 * Asked only from power_check, which every screen calls beside its input
 * poll - `grep -n power_check` is the list, and it should have no gaps. Never
 * during live play: NextUI's autosleep is off while a game runs (see the
 * SETIDLE in launch()), and on again in the in-game menu, which is here. */
static bool sleep_cycle(app *a);
static pwr_action power_check(app *a);

/* Muse Settings' Screen Off, in seconds; 0 is Never. See idle_due. */
static const int MUSE_SCREEN_OFF[] = { 5, 10, 15, 30, 60, 0 };
#define MUSE_SCREEN_OFF_COUNT \
	((int)(sizeof MUSE_SCREEN_OFF / sizeof *MUSE_SCREEN_OFF))

static bool idle_due(app *a)
{
	int b, secs;

	/* Whichever of the two is armed - never both (ST_SLEEP/ST_AUTO_OFF) -
	 * except while music plays, when Muse Settings' Screen Off says how
	 * long (TortOS-28l): an iPod's backlight timer, short where Auto Sleep's
	 * shortest is long. Never is the screen on for as long as music plays. A
	 * new timer is a new countdown: an album that ended after five minutes
	 * untouched must not fire Auto Sleep the moment it stops. */
	secs = musec_playing() ? db_get_int(db_dev(), "muse.screenoff", 10)
	                       : a->auto_off ? a->auto_off : a->auto_poweroff;
	if (secs != a->idle.seconds) a->idle.since_ms = plat_now_ms();
	a->idle.seconds = secs;
	for (b = 0; b < IN_COUNT; b++)
		if (a->in.pressed[b] || a->in.down[b]) break;

	/* Music does NOT hold the clock (it did until TortOS-a5k): an album is
	 * somebody using the device with nobody touching it, so running out
	 * during one turns only the screen off - music_dark - and whatever the
	 * clock was armed for waits until the music stops. */
	return idle_check(&a->idle, plat_now_ms(), b < IN_COUNT, keep_awake());
}

/* The keyboard takes callbacks rather than an app - it is deliberately
 * general, and knows nothing about shelves or power policy. */
static bool power_due_ctx(void *ctx)
{
	return power_check((app *)ctx) == PWR_POWEROFF;
}

/* ---------- when the next frame is needed -------------------------------- */

/* The earliest moment something already on screen will look different with
 * nobody touching anything - a card mid-move, the tint still easing toward a
 * new system, a long title about to scroll, the volume line about to go.
 * REDRAW_NEVER means nothing drawn in the last frame changes by itself.
 *
 * Gathered WHILE drawing, because the things that move are the things that
 * know they are moving: cf_draw already returns whether a move is in flight,
 * and a marquee is the only thing that knows where it is in its hold. render
 * resets it and every drawing path that animates pulls it earlier. The main
 * loop then only draws when it has come due, or when there was input.
 *
 * It exists because the shelf was redrawn and presented at the refresh rate
 * whether or not anything had changed. Measured on the Brick 2026-09-13, idle
 * on a shelf: 26.6% of a core and 88 voluntary context switches a second, all
 * of it the main thread, for a picture that was not moving. */
#define REDRAW_NEVER UINT32_MAX
static Uint32 g_redraw_at = 0;

static void redraw_at(Uint32 t) { if (t < g_redraw_at) g_redraw_at = t; }
static void redraw_now(void)    { g_redraw_at = 0; }

/* The battery, top right: the one piece of chrome. Off - the default - it is
 * what it always was, a small red disc when the battery is low. On (System
 * Settings > Battery Percentage, plorpos-gkd.86.3), a disc with the
 * percentage in it, on every screen the launcher draws: red at BATT_LOW_PCT
 * or under, green while charging, quiet otherwise. Drawn with horizontal
 * spans - SDL has no circle. */
static int g_batt_show = -1;      /* the setting; -1 = read it again */

static void battery_indicator_changed(void) { g_batt_show = -1; redraw_now(); }

static void fill_disc(SDL_Renderer *r, int cx, int cy, int rad)
{
	for (int dy = -rad; dy <= rad; dy++) {
		int dx = (int)(sqrt((double)(rad * rad - dy * dy)) + 0.5);
		SDL_RenderDrawLine(r, cx - dx, cy + dy, cx + dx, cy + dy);
	}
}

static void draw_battery(SDL_Renderer *r)
{
	if (g_batt_show < 0) g_batt_show = db_get_int(db_dev(), "battpct", 0) == 1;
	if (!g_batt_show) {
		if (battery_low()) {
			SDL_SetRenderDrawColor(r, 224, 72, 72, 255);
			fill_disc(r, TORTOS_SCREEN_W - 34, 34, 9);
		}
		return;
	}
	battery_poll();
	/* The number changes with nobody touching anything, so an idle shelf
	 * still wakes for the next reading. */
	redraw_at(g_batt_next);
	if (!g_batt_ok) return;

	TTF_Font *f = ui_font(UI_F_BADGE);
	/* Sized for "100", the widest it gets, so the disc never changes size -
	 * and small, in the corner: clear of a long shelf title, which runs to
	 * about x 920, and of the GKD's in-game menu, whose panel reaches x 999
	 * from y 41 (measured 2026-10-05; user: no overlap, one size for all). */
	int rad = ui_text_width(f, "100") / 2 + 3;
	int cx = TORTOS_SCREEN_W - 10 - rad, cy = 4 + rad;
	char num[8];

	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
	if (g_batt_charging)                 SDL_SetRenderDrawColor(r, 64, 168, 96, 255);
	else if (g_batt_pct <= BATT_LOW_PCT) SDL_SetRenderDrawColor(r, 224, 72, 72, 255);
	else                                 SDL_SetRenderDrawColor(r, 84, 88, 104, 255);
	fill_disc(r, cx, cy, rad);
	snprintf(num, sizeof num, "%d", g_batt_pct);
	/* Centered on the digits' ink, not their line box. */
	ui_text(r, f, num, cx, cy - ui_font_ascent(f) + ui_font_cap(f) / 2, 0, UI_TEXT);
}

/* What goes over every launcher screen, last: the battery, then the volume or
 * brightness bar, which covers it while it shows. */
static void draw_chrome(SDL_Renderer *r)
{
	draw_battery(r);
	plat_draw_osd(r);
}

static void draw_background(app *a)
{
	SDL_Renderer *r = a->r;
	SDL_SetRenderDrawColor(r, UI_BG_R, UI_BG_G, UI_BG_B, 255);
	SDL_RenderClear(r);
	/* A wash of the focused system's color along the bottom edge, so the
	 * whole screen belongs to the machine you are looking at. Faint enough to
	 * read as light rather than as a panel. */
	{
		SDL_Rect band = { 0, TORTOS_SCREEN_H - 240, TORTOS_SCREEN_W, 480 };
		ui_glow(r, &band, a->tint, 34, 1.7f);
	}
}

/* How fast a shelf animates, written every frame from what is about to be
 * drawn rather than left behind by whatever drew last.
 *
 * These live on the coverflow so step_anim can read them without knowing
 * which mode is running, and that means they PERSIST: state that one of two
 * paths writes is state the other path has to write too. The row keeps its
 * own 240ms - anim_ms of 0 means the default - because a card step should
 * feel immediate. */
static void shelf_pacing(coverflow *cf)
{
	if (CARD_DIRS[g_dir].vertical) {
		/* THE DURATION IS FIXED BUT THE DISTANCE IS NOT. ANIM_MS is 240ms
		 * whatever the travel, and Vertical shows one card at a time, so a
		 * step moves 878px where the horizontal row moves 400. At the shared
		 * default that is 2.2x the speed, which reads as too fast AND as
		 * jerky - the frame rate is unchanged, so covering twice the ground
		 * means half as many frames per pixel.
		 *
		 * 360ms is 2.4 px/ms against the horizontal row's 1.7 - deliberately
		 * quicker than matching, because one card filling the screen carries
		 * no neighbors to read on the way past and the travel is dead time.
		 * 420 matched the row exactly and felt sluggish. Smooth rather than
		 * out-cubic because out-cubic spends its speed at the start, and over
		 * a throw this long that is a lurch before the settle. */
		cf->anim_ms = 360.0f;
		cf->ease = CF_EASE_SMOOTH;
		cf->chase = false;
		/* Two, not the row's eight. Vertical puts ONE card on the screen at a
		 * time, so a card of travel is a whole screen of travel; eight would
		 * be a full-screen blur rather than a departure. */
		cf->glide = 2.0f;
	} else {
		cf->anim_ms = 0.0f;
		cf->ease = CF_EASE_OUT_CUBIC;
		cf->chase = false;
		cf->glide = (float)CF_WARM_CARDS;
	}
}

/* A systems layout carrying the theme's reflection gap.
 *
 * The gap belongs to the art, not to the row: a drawn card rests on the
 * reflective surface and a photographed console floats above it, and one
 * layout draws both. Only system art gets it - box art is card-like whatever
 * the theme is, so the games shelf sits flush. */
static cf_layout sys_layout(const cf_layout *base)
{
	cf_layout lay = *base;

	lay.reflect_gap = CARD_SETS[g_cards].reflect_gap;
	return lay;
}


/* The systems rail's strip: one band per system, in shelf order.
 *
 * Only the systems rails get one. A games rail's items are all one system's
 * games and carry that system's accent, so a strip of them would be a single
 * color with the cost of asking per pixel - those pass NULL and stay one fill.
 *
 * Bounded rather than trusted: ui_rail walks the ring modulo `count`, which is
 * this array's length, but a rail drawn with a stale count during a rescan
 * would index past it. */
static unsigned rail_sys_hue(void *ctx, int i)
{
	app *a = ctx;

	if (i < 0 || i >= a->sys.count) return 0;
	return a->sys.systems[i].accent;
}

/* How far the shelf's stage sits below the top of a screen taller than the one
 * it was tuned on (see CF_STAGE_H): the cards are placed on it, so whatever is
 * placed against the cards moves with them. 0 on the Brick. */
#define STAGE_Y ((TORTOS_SCREEN_H - CF_STAGE_H) / 2)

static void draw_systems(app *a)
{
	shelf_pacing(&a->cf_sys);

	SDL_Rect focus;
	const system_cfg *s = &a->sys.systems[a->sys_cursor];
	char line[128];

	/* Systems only. The games shelf keeps the angled row whatever this says:
	 * box art is a wall of many, and one cover per screen would turn picking
	 * a game into paging through a catalog. */
	/* Vertical stacks the same row down the screen. */
	cf_layout lay = sys_layout(CARD_DIRS[g_dir].vertical
	                           ? &CF_LAYOUT_SYSTEMS_V : &CF_LAYOUT_SYSTEMS);

	cf_focus_rect(&lay, TORTOS_SCREEN_W, TORTOS_SCREEN_H, &focus);
	ui_glow(a->r, &focus, s->accent, 110, 2.4f);
	if (cf_draw(&a->cf_sys, a->r, TORTOS_SCREEN_W, TORTOS_SCREEN_H, a->sys.count,
	            sys_get_tex, a, &lay))
		redraw_now();

	/* WHICH SYSTEM THE WORDS BELOW DESCRIBE, and how visible they are.
	 *
	 * Vertical crossfades them: one card fills the screen there, so a name
	 * that changed the instant the button went down sat under a console that
	 * had not started moving - the only thing on screen ahead of the motion.
	 * cf_label answers both questions from the shelf's own position, so the
	 * name swaps while it is invisible and nothing has to remember the old
	 * one. The horizontal row keeps the cursor and the instant change: seven
	 * cards move at once there and the eye is on them, and fading the name
	 * would be a second thing happening for no reason.
	 *
	 * The count follows the name, not the cursor, or a move would show the
	 * old system's name over the new system's game count. */
	const system_cfg *label = s;
	float label_a = 1.0f;

	if (CARD_DIRS[g_dir].vertical) {
		int li = 0;

		label_a = cf_label(&a->cf_sys, a->sys.count, &li);
		if (li >= 0 && li < a->sys.count) label = &a->sys.systems[li];
	}

	/* Art that does not name itself gets named here, in the gap between the
	 * card and the count, which is where the classic cards carry it. */
	if (!CARD_SETS[g_cards].labeled) {
		char nfit[192];

		ui_fit_text(ui_font(UI_F_TITLE), label->name, nfit, sizeof nfit,
		            TORTOS_SCREEN_W - 48);
		/* 618 is not arbitrary: the title renders 52px tall, so it ends at
		 * 670 and leaves 20px before the count at 690. The card is sized to
		 * fit ABOVE this rather than this being pushed down to suit the
		 * card. */
		ui_text(a->r, ui_font(UI_F_TITLE), nfit, TORTOS_SCREEN_W / 2, STAGE_Y + 618, 0,
		        ui_fade(UI_TEXT, label_a));
	}

	if (is_muse(label)) {
		/* Muse has albums and books rather than games, each counted, and a
		 * kind the card has none of left out. */
		int albums = ml_count(&g_muse, false), books = ml_count(&g_muse, true);
		char al[32] = "", bk[32] = "";

		if (albums) snprintf(al, sizeof al, "%d album%s", albums, albums == 1 ? "" : "s");
		if (books)  snprintf(bk, sizeof bk, "%d book%s", books, books == 1 ? "" : "s");
		snprintf(line, sizeof line, "%s%s%s", al, al[0] && bk[0] ? ", " : "", bk);
	} else {
		int gc = a->view[(int)(label - a->sys.systems)].list.count;

		/* A shelf of one read "1 games". Favorites hits it first because it
		 * starts empty and grows one at a time, but any system with a single
		 * ROM has always said it. */
		if (gc > 0)
			snprintf(line, sizeof line, "%d game%s", gc, gc == 1 ? "" : "s");
		else
			/* The folder is bounded by CFG_STR and this line is not, so
			 * the name is capped rather than the buffer grown: a folder
			 * long enough to overflow a sentence is not one anybody can
			 * read off a shelf either. The cross compiler is the only one
			 * that says so - clang does not. */
			snprintf(line, sizeof line, "no games in Roms/%.96s", label->folder);
	}
	ui_text(a->r, ui_font(UI_F_META), line, TORTOS_SCREEN_W / 2, STAGE_Y + 690, 0,
	        ui_fade(UI_TEXT_DIM, label_a));
	(CARD_DIRS[g_dir].vertical ? ui_rail_v : ui_rail)
		(a->r, TORTOS_SCREEN_W, TORTOS_SCREEN_H, a->cf_sys.pos, a->sys.count,
		 s->accent, rail_sys_hue, a);
}

/* Milliseconds since the focused game last changed.
 *
 * The marquee's clock, and it lives here rather than in ui.c because only this
 * knows what "the subject" is. Moving the cursor - or changing shelves - is a
 * new subject and starts the wait again, which is what keeps a scan quiet. */
static int shot_phase = -1;        /* --phase, for the shot harness only */

/* The marquee's clock. It restarts when the subject changes, so a newly chosen
 * name is read from its beginning rather than joined halfway through a scroll.
 *
 * A SLOT PER SUBJECT, not one clock. A menu draws over a live shelf and the
 * shelf asks for its phase every frame either way, so one shared clock would
 * be reset twice a frame by two different subjects and nothing would ever
 * move. Two keys because the shelf's subject is a pair - which system, and
 * which game within it - and either changing is a new subject.
 *
 * shot_phase freezes it at a chosen moment. A still of a moving thing needs
 * one; without it the only way to look at a marquee is to film it. That used
 * to be here and not in the menu's copy of this, which is what having two
 * copies costs. */
/* Two note slots because a panel can carry two footer notes and both may be
 * long enough to move. Sharing one would be the bug this enum exists to
 * prevent, a step smaller: each would see the other's key every frame and
 * neither would ever get past the opening hold. */
#define MQ_NOTES 2
/* MQ_FIXED is for rows that move because nothing can ever select them, as
 * distinct from the ones that move because something just did.
 *
 * The other slots are keyed on the cursor, which is right when the row that
 * scrolls is the row you landed on: a new selection is a new subject and its
 * text should start from the beginning. It is wrong for a fact. The game
 * screen's Genre row is unselectable and scrolls whenever it is long, and
 * stepping between Synopsis and Cheevos was restarting it - a cursor move that
 * has nothing to do with the row it interrupted. Reported 2026-09-17, the day
 * Genre became the first long permanently-unselectable row on a screen that
 * also has a cursor.
 *
 * Keyed on the SCREEN instead - its heading and how many rows it has - which
 * is the same value for every such row in a frame, so they still share one
 * clock and still move together rather than resetting each other.
 *
 * It replaced a slot of its own for VALUES, which was the wrong axis to split
 * on: a label and a value never move in the same row - the wider of the two
 * takes the room and the other is laid out around it - so what needed separate
 * clocks was never the two columns. It was the selected row against the rest,
 * and only one row is ever selected. */
enum { MQ_SHELF, MQ_MENU, MQ_FIXED, MQ_NOTE0, MQ_NOTE1, MQ_VSCROLL,
       MQ_HEAD, MQ_NP, MQ_SLOTS };

static int      mq_last_a[MQ_SLOTS], mq_last_b[MQ_SLOTS];
static unsigned mq_since[MQ_SLOTS];
static bool     mq_primed[MQ_SLOTS];

/* Start this slot's clock over on the next frame.
 *
 * The keys are what a slot normally restarts on - a new game, a new heading -
 * and they are the right rule for a marquee that lives on a screen you are
 * already looking at. They are the wrong rule for a screen you LEAVE and come
 * back to: the keys are identical, so the clock is not reset, and a synopsis
 * reopened resumed halfway down where it had been left. Reported 2026-09-17,
 * and the only way to read a long one from the top was to wait out the rest.
 *
 * So a screen that has just opened says so, rather than the clock trying to
 * infer it from a key that did not change. */
static void mq_reset(int who)
{
	if (who >= 0 && who < MQ_SLOTS) mq_primed[who] = false;
}

static unsigned mq_phase(int who, int key_a, int key_b)
{
	int      *last_a = mq_last_a, *last_b = mq_last_b;
	unsigned *since  = mq_since;
	bool     *primed = mq_primed;
	unsigned now = plat_now_ms();

	if (shot_phase >= 0) return (unsigned)shot_phase;

	if (!primed[who] || key_a != last_a[who] || key_b != last_b[who]) {
		primed[who] = true;
		last_a[who] = key_a;
		last_b[who] = key_b;
		since[who]  = now;
	}
	return now - since[who];
}

/* The clock a moving ROW runs on: the cursor's when the cursor put it in
 * motion, the screen's when nothing ever will. See MQ_FIXED.
 *
 * The heading is measured rather than compared, because mq_phase's keys are
 * two ints - and a length is enough to notice a different game, which is what
 * a new screen means here. */
static unsigned mq_row_phase(bool selected, const char *heading, int n, int sel)
{
	if (selected) return mq_phase(MQ_MENU, sel, 0);
	return mq_phase(MQ_FIXED, heading ? (int)strlen(heading) : 0, n);
}

static unsigned title_phase(app *a)
{
	return mq_phase(MQ_SHELF, a->sys_cursor, a->view[a->sys_cursor].cursor);
}

/* Everything a games shelf says about one game: its title, its favorite mark
 * and its place in the list. */
static void draw_game_text(app *a, sysview *v, const system_cfg *s, int idx)
{
	const system_cfg *gs = &a->sys.systems[
		shelf_owner(a, a->sys_cursor, idx)];
	char count[64];

	if (v->list.count > 0) {
		game_entry *g = &v->list.items[idx];
		/* A title that fits is centered, as before. One that does not slides,
		 * because truncating it destroys the rest of the name permanently and
		 * sliding it only delays it - see ui_text_marquee.
		 *
		 * THE HEART HAS A FIXED SEAT, far left, whether or not this game
		 * has one. It used to be placed off the title's own width, which put
		 * it somewhere new for every game and - once titles could slide -
		 * would have moved it at the moment a title started sliding. A mark
		 * that jumps when the thing beside it grows is not a mark, it is more
		 * motion.
		 *
		 * Its width is reserved on BOTH sides, so the box stays centered on the
		 * screen and a short title is centered exactly where it always was.
		 * The cost is a narrower box, so a few more titles slide; sliding is
		 * the thing that made a narrow box acceptable. */
		TTF_Font *ft2 = ui_font(UI_F_TITLE);
		int line = ui_font_line(UI_F_TITLE);
		int margin = line / 2;
		int hrad = line * 2 / 5;
		int lead = margin + hrad * 2 + hrad;      /* edge, heart, its gap */
		int boxx = lead;
		int boxw = TORTOS_SCREEN_W - lead * 2;
		int tw = ui_text_width(ft2, g->title);
		int tx = TORTOS_SCREEN_W / 2;
		bool slides = tw > boxw;
		/* Asked for EVERY frame, not only when something slides. It is a
		 * clock that has to watch the cursor to know when to restart, and it
		 * cannot watch it on frames nobody calls it. Reading it only inside
		 * the sliding branch meant a short title in between - which needs no
		 * marquee and so never called this - left the last long one still
		 * recorded as current; coming back to it was not a change, and it
		 * carried on mid-scroll instead of starting over. */
		unsigned phase = title_phase(a);
		/* The one mark left on a card.
		 *
		 * There was a second, a dot on the right, for a game with an autosave
		 * - A continues it rather than starting it. Removed 2026-08-30: it
		 * only ever drew beside the CENTERED title, so it was never the
		 * scan-the-shelf signal it looked like, and it said one bit with no
		 * legend about the one game you could already ask about directly. X
		 * says "resume + 3" now, which is the same fact with a number on it. */
		if (fav_is(gs->tag, g->file)) {
			/* Centered on the title's INK, not its em box. The box reserves a
			 * descender's depth most titles never use, so a mark placed at
			 * the box's middle sits visibly below the letters. Descent is
			 * negative, so half of it lifts. */
			int dy = 40 + line / 2 + (ft2 ? ui_font_descent(ft2) / 2 : 0);
			SDL_Color c = { (Uint8)(gs->accent >> 16), (Uint8)(gs->accent >> 8),
			                (Uint8)gs->accent, 255 };

			draw_heart(a->r, (float)(margin + hrad), (float)dy, (float)hrad, c);
		}
		if (slides) {
			ui_text_marquee(a->r, ft2, g->title, boxx, 40, boxw,
			                phase, UI_TEXT);
			/* Not "a title is sliding, so keep drawing". It ping-pongs for
			 * as long as the game is focused, with 1.4s of stillness at the
			 * start and 0.9s at the far end, so ask for the moment it next
			 * moves and sleep through the holds. The phase clock is
			 * plat_now_ms, the same one the main loop compares against.
			 *
			 * Only the holds can be skipped; the travel between them is
			 * motion and has to be drawn. Measured on the Brick 2026-09-13,
			 * idle for 30s on Advanced Busterhawk Gleylancer: 17.4% of a core,
			 * against 1.7% on a title that fits and 26.6% before any of this.
			 *
			 * DECIDED, left that way. Scrolling once and resting would bring a
			 * long title down near 1.7%, and it is not worth doing, because
			 * this is not idle. A marquee only runs while its title is focused
			 * and on screen, so someone looking at one is reading it - the cost
			 * is the feature doing its job while it is being used. And a device
			 * set down on a long title with nobody looking is ended by Auto Off,
			 * so there is no long unattended stretch of it to save. */
			redraw_at(plat_now_ms() + ui_pingpong_wait(tw - boxw, phase));
		} else {
			ui_text(a->r, ft2, g->title, tx, 40, 0, UI_TEXT);
		}
		/* An album is two names, and on Muse's shelf the artist is the one
		 * the letter jump goes by - so it is said, a step quieter, under the
		 * album's. Fitted rather than slid: the album's title already slides
		 * on this clock, and two lines moving at once is a lot of motion. */
		if (is_muse(s)) {
			char afit[192];
			const char *under = g->name;
			int al = v->album && idx >= 0 && idx < v->list.count ? v->album[idx] : -1;

			/* A book says it is finished there, and says nothing rather than
			 * its own name again when it is a folder with no author above it. */
			if (al >= 0 && g_muse.albums[al].book) {
				if (g_book_done && g_book_done[al]) under = "Finished";
				else if (!strcmp(g->name, g->title)) under = "";
			}
			ui_fit_text(ui_font(UI_F_MENU), under, afit, sizeof afit, boxw);
			ui_text(a->r, ui_font(UI_F_MENU), afit, tx, 40 + line + 4, 0,
			        UI_TEXT_DIM);
		}
		/* Where you are in the list, not what this game is - the same kind
		 * of thing the rail says, so it goes where the rail is.
		 *
		 * Horizontally that is centered under the row, which is a band across
		 * the middle of the screen: nothing travels down there.
		 *
		 * Vertically it is bottom left, under the rail's lower end, because
		 * the shelf slides along y a card height and a half per step and a
		 * centered counter had every move drag a card and its reflection
		 * straight across it. */
		snprintf(count, sizeof count, "%d / %d", idx + 1, v->list.count);
		if (CARD_DIRS[g_dir].vertical)
			ui_text(a->r, ui_font(UI_F_META), count, 24, STAGE_Y + 700,
			        -1, UI_TEXT_DIM);
		else
			ui_text(a->r, ui_font(UI_F_META), count,
			        TORTOS_SCREEN_W / 2, STAGE_Y + 690, 0, UI_TEXT_DIM);
	} else {
		char nfit[192];

		ui_fit_text(ui_font(UI_F_TITLE), s->name, nfit, sizeof nfit,
		            TORTOS_SCREEN_W - 48);
		ui_text(a->r, ui_font(UI_F_TITLE), nfit, TORTOS_SCREEN_W / 2, 40, 0,
		        UI_TEXT);
	}
}

static void draw_games(app *a)
{
	shelf_pacing(&a->view[a->sys_cursor].cf);
	shelf_pacing(&a->cf_sys);

	sysview *v = &a->view[a->sys_cursor];
	const system_cfg *s = &a->sys.systems[a->sys_cursor];
	bool albums = is_muse(s);
	const cf_layout *row = albums ? &CF_LAYOUT_ALBUMS : &CF_LAYOUT_GAMES;

	if (cf_draw(&v->cf, a->r, TORTOS_SCREEN_W, TORTOS_SCREEN_H, v->list.count,
	            game_get_tex, a,
	            !CARD_DIRS[g_dir].vertical ? row
	            : albums ? &CF_LAYOUT_ALBUMS_V : &CF_LAYOUT_GAMES_V))
		redraw_now();
	/* Warm where the move is about to cut to, one card per frame, underneath
	 * the departure that is still being drawn. Eviction waits: the cursor is
	 * already at the destination and evict_far measures from the cursor, so
	 * running it now would free the very cards the departure is drawing. */
	{
		int land = cf_landing(&v->cf, v->list.count);
		if (land >= 0) prime_toward(a, a->sys_cursor, land);
	}
	if (!cf_cutting(&v->cf)) evict_far(v, TEX_KEEP_NEAR);

	draw_game_text(a, v, s, v->cursor);
	(CARD_DIRS[g_dir].vertical ? ui_rail_v : ui_rail)
		(a->r, TORTOS_SCREEN_W, TORTOS_SCREEN_H, v->cf.pos, v->list.count,
		 s->accent, NULL, NULL);
}

/* The shelf, without presenting it: the options menu draws over a live one, so
 * the cards keep their tint easing behind the panel rather than freezing into
 * a still. */
/* Nothing on the card at all.
 *
 * Hiding empty systems has an edge that hiding one system does not: if every
 * system is empty the shelf is simply blank, and a blank screen is what a
 * broken launcher looks like. Someone who has just written a card, or put the
 * ROMs one directory too deep, needs to be told where the games go rather
 * than left to conclude the device is dead. The path is named because that is
 * the actual question being asked. */
static void draw_no_games(app *a)
{
	int cy = TORTOS_SCREEN_H / 2;

	ui_text(a->r, ui_font(UI_F_TITLE), "No games found",
	        TORTOS_SCREEN_W / 2, cy - 60, 0, UI_TEXT_SOFT);
	ui_text(a->r, ui_font(UI_F_MENU), "Put ROMs in Roms/<System>/ on the card",
	        TORTOS_SCREEN_W / 2, cy + 6, 0, UI_TEXT_DIM);
	ui_text(a->r, ui_font(UI_F_META), "one folder per system, named as in systems.cfg",
	        TORTOS_SCREEN_W / 2, cy + 56, 0, UI_TEXT_DIM);
}

/* The background and the focus glow as one picture, kept while nothing they
 * show changes - for SDL's software renderer only, the one under native
 * PICO-8's menu: there the two scaled, tinted glows cost 32 of a 65 ms frame
 * and Muse scrolled at 15 fps (plorpos-7ny.37). A GPU draws them for nothing.
 * Freed with the shelf's textures, which is before its renderer goes. */
static struct { unsigned tint, accent; SDL_Rect focus; bool glow; } g_backdrop_is;

static bool backdrop_cached(app *a, const SDL_Rect *focus, unsigned accent)
{
	SDL_RendererInfo ri;
	SDL_Texture *was;

	if (SDL_GetRendererInfo(a->r, &ri) != 0 || !(ri.flags & SDL_RENDERER_SOFTWARE))
		return false;
	if (!g_backdrop) {
		g_backdrop = SDL_CreateTexture(a->r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET,
		                               TORTOS_SCREEN_W, TORTOS_SCREEN_H);
		if (!g_backdrop) return false;
		SDL_SetTextureBlendMode(g_backdrop, SDL_BLENDMODE_NONE);
		g_backdrop_is.glow = !focus;   /* differs from any ask: drawn below */
	}
	if (g_backdrop_is.tint != a->tint || g_backdrop_is.glow != !!focus ||
	    (focus && (g_backdrop_is.accent != accent ||
	               memcmp(&g_backdrop_is.focus, focus, sizeof *focus)))) {
		was = SDL_GetRenderTarget(a->r);
		if (SDL_SetRenderTarget(a->r, g_backdrop) != 0) return false;
		draw_background(a);
		if (focus) ui_glow(a->r, focus, accent, 100, 2.3f);
		SDL_SetRenderTarget(a->r, was);
		g_backdrop_is.tint = a->tint;
		g_backdrop_is.glow = !!focus;
		g_backdrop_is.accent = accent;
		g_backdrop_is.focus = focus ? *focus : (SDL_Rect){ 0 };
	}
	SDL_RenderCopy(a->r, g_backdrop, NULL, NULL);
	return true;
}

static void draw_shelf(app *a)
{
	/* Install whatever the workers finished, before anything is drawn.
	 *
	 * Here rather than in the main loop, because the main loop is one of
	 * seventeen places that draw the shelf - every menu and overlay shows it
	 * behind itself, each from its own loop. Draining only in the main loop
	 * meant that while any of those was open, a card not yet decoded stayed
	 * blank for as long as the screen was up, and finished decodes piled into
	 * the result ring and were discarded once it filled. Everything that draws
	 * the shelf wants its textures, so the thing that draws the shelf drains. */
	texload_drain(a);
	{
		bool games = a->sys.count > 0 && a->screen != SCREEN_SYSTEMS;
		unsigned accent = 0;
		SDL_Rect focus = { 0 };

		/* The glow behind the focused card. */
		if (games) {
			accent = a->sys.systems[a->sys_cursor].accent;
			cf_focus_rect(is_muse(&a->sys.systems[a->sys_cursor]) ? &CF_LAYOUT_ALBUMS
			              : &CF_LAYOUT_GAMES, TORTOS_SCREEN_W, TORTOS_SCREEN_H, &focus);
		}
		if (!backdrop_cached(a, games ? &focus : NULL, accent)) {
			draw_background(a);
			if (games) ui_glow(a->r, &focus, accent, 100, 2.3f);
		}
	}
	if (a->sys.count <= 0) draw_no_games(a);
	else if (a->screen == SCREEN_SYSTEMS) draw_systems(a);
	else draw_games(a);
}

/* Drawing only when something changed. See g_redraw_at.
 *
 * The loop still runs every IDLE_POLL_MS when nothing is drawn, so the power
 * button, Auto Off, the headphone jack and the Bluetooth sink are all still
 * checked at the same rate as before - only the draw and the present are
 * skipped. 16ms keeps the worst case from a press to its first frame where
 * it was, one refresh.
 *
 * Why a poll and not a wait on the input devices: input arrives through two
 * roads, SDL's own events and three raw evdev descriptors, and a held
 * button produces no event at all while it repeats - in_repeat works from
 * the clock. A wait that slept until the next event would have stopped key
 * repeat dead. Polling costs a handful of syscalls a frame, against a full
 * draw and a present. */
#define IDLE_POLL_MS 16
/* A gap this long between two passes means something else had the screen -
 * a menu, a game, the art scraper - and whatever it drew is not the shelf.
 * Catching it here covers every one of them without a list to keep. */
#define AWAY_MS 100
/* Drawn at least this often regardless. A backstop, not a mechanism: if some
 * animation is ever added without telling g_redraw_at, it shows as a screen
 * updating once a second, which is visibly wrong, rather than one that has
 * silently frozen. */
#define HEARTBEAT_MS 1000

/* Whether a loop that shows the shelf draws it this pass, by the rule above:
 * the main loop and Muse's shelf (plorpos-7ny.38 - that one drew every pass,
 * 88% of a core under native PICO-8's menu). */
static bool shelf_draw_due(app *a, Uint32 *last_pass, Uint32 last_render)
{
	Uint32 now = plat_now_ms();
	/* Held counts as touched, not only pressed: a held direction
	 * repeats from the clock and moves the shelf every 90ms. */
	bool touched = false, away = now - *last_pass > AWAY_MS;
	int b;

	for (b = 0; b < IN_COUNT && !touched; b++)
		touched = a->in.pressed[b] || a->in.down[b];
	*last_pass = now;

	/* texload_ready because finished art is installed while drawing:
	 * a loop that never drew would never find it. */
	return touched || away || texload_ready() || now >= g_redraw_at ||
	       now - last_render >= HEARTBEAT_MS;
}

static void render(app *a)
{
	g_redraw_at = REDRAW_NEVER;
	draw_shelf(a);
	draw_chrome(a->r);
	/* The tint lands exactly on its target now (see tick_tint), so this is a
	 * test that ends rather than one that is true forever. */
	if (a->sys.count > 0 && a->tint != a->sys.systems[a->sys_cursor].accent)
		redraw_now();
	redraw_at(plat_osd_until());
	plat_present(a->r);
}

/* Ease the background tint toward the focused system rather than snapping: the
 * color is meant to feel like the light the machine gives off, and light does
 * not cut. Called from every loop that draws the shelf.
 *
 * And then land it. ui_mix truncates, so an ease this gentle stops short on any
 * channel that is rising: at 60fps each step is about 14% of the gap, and 14% of
 * 7 is under one, so it came to rest up to 7 units below its target and stayed
 * there for good. That never showed, but it meant "has the tint arrived" was
 * never true - and a launcher that only redraws while something is still
 * changing would have redrawn at the refresh rate forever. Within 12 on every
 * channel it takes the target outright: under 5% of a channel, on a dim wash, at
 * the very tail of an ease nobody watches finish. 12 rather than 7 because a
 * faster frame means a smaller step and a wider stall; dt is what sets it. */
static unsigned last_tint_ms;
static void tick_tint(app *a)
{
	unsigned now = plat_now_ms();
	unsigned target = a->sys.systems[a->sys_cursor].accent, next;
	float dt = (float)(now - last_tint_ms) / 1000.0f;
	int dr, dg, db;

	last_tint_ms = now;
	if (dt > 0.1f) dt = 0.1f;
	next = ui_mix(a->tint, target, 1.0f - expf(-dt * 9.0f));
	dr = (int)((next >> 16) & 255) - (int)((target >> 16) & 255);
	dg = (int)((next >> 8) & 255) - (int)((target >> 8) & 255);
	db = (int)(next & 255) - (int)(target & 255);
	a->tint = (abs(dr) <= 12 && abs(dg) <= 12 && abs(db) <= 12) ? target : next;
}

/* ---------- transitions --------------------------------------------------- */

/* Starting a game the slow way: the focused card comes at you and the screen
 * goes with it.
 *
 * Only on the fallback, when there is no resident emulator and Diatom runs as
 * a process of its own. A normal launch has no animation at all, and that is a
 * display-safety rule rather than a taste call - see the resident path in
 * launch(): Diatom presents through fbdev and this process through GL, and the
 * two presenting at once wedges the display engine. On the fallback nothing
 * else is presenting yet, and this process tears its display down straight
 * after. This comment used to say the zoom played while the resident loaded
 * the ROM, which stopped being true when that rule was made. */
static void anim_launch(app *a, unsigned ms)
{
	sysview *v = &a->view[a->sys_cursor];
	unsigned t0 = plat_now_ms(), now;
	SDL_Rect from;
	int tw = 0, th = 0;
	SDL_Texture *card;

	cf_focus_rect(&CF_LAYOUT_GAMES, TORTOS_SCREEN_W, TORTOS_SCREEN_H, &from);
	card = v->list.count ? game_get_tex(a, v->cursor, &tw, &th, NULL) : NULL;

	while ((now = plat_now_ms()) - t0 < ms) {
		float k = (float)(now - t0) / (float)ms;
		float e = k * k;                     /* accelerate away */
		float scale = 1.0f + e * 2.2f;
		SDL_Rect dst;

		/* Asked again every frame until it is there. Card art decodes on a
		 * worker, so A pressed straight after a letter jump can start this
		 * before the focused card has arrived - and asking once, as this did,
		 * zoomed an empty frame for the whole animation. Now a card that lands
		 * partway through joins the zoom where it is. */
		if (!card && v->list.count) {
			texload_drain(a);
			card = game_get_tex(a, v->cursor, &tw, &th, NULL);
		}

		SDL_SetRenderDrawColor(a->r, UI_BG_R, UI_BG_G, UI_BG_B, 255);
		SDL_RenderClear(a->r);
		if (card) {
			dst.w = (int)(from.w * scale);
			dst.h = (int)(from.h * scale);
			dst.x = TORTOS_SCREEN_W / 2 - dst.w / 2;
			dst.y = TORTOS_SCREEN_H / 2 - dst.h / 2;
			SDL_SetTextureAlphaMod(card, (Uint8)(255 * (1.0f - k)));
			SDL_RenderCopy(a->r, card, NULL, &dst);
			SDL_SetTextureAlphaMod(card, 255);
		}
		plat_present(a->r);
		SDL_Delay(6);
	}
	SDL_SetRenderDrawColor(a->r, 0, 0, 0, 255);
	SDL_RenderClear(a->r);
	plat_present(a->r);
}

/* The send-off, and the inverse of the boot animation's gesture: there he
 * launches head-first out of frame, here he arrives and pulls the head in.
 *
 * Timed against the shutdown rather than chosen. Measured 2026-08-28: 1.42 s
 * from the Power off press to adbd dying, of which the old 620 ms animation
 * was the first slice - the rest was a black screen while sync and the kernel
 * finished. So the animation runs to about 900 ms and then DOES NOT clear.
 *
 * Not clearing is the point. Whatever was last presented stays on the panel
 * until the kernel cuts it, so the mark sits there through the remainder of
 * the shutdown and the screen going dark is the device going dark. The boot
 * animation relies on exactly the same thing at the other end. */
static void anim_poweroff(app *a)
{
	const unsigned T_IN = 430, T_HEAD = 260, T_DIM = 210;
	const unsigned ms = T_IN + T_HEAD + T_DIM;
	const float cx = TORTOS_SCREEN_W / 2.0f, cy = TORTOS_SCREEN_H / 2.0f;
	const float rad = 46.0f;
	unsigned t0 = plat_now_ms(), now;

	while ((now = plat_now_ms()) - t0 < ms) {
		unsigned t = now - t0;
		float x = cx, head = 1.0f, dim = 0.0f;
		if (t < T_IN) {
			/* In from the left, decelerating onto the center. */
			float k = (float)t / (float)T_IN;
			float e = 1.0f - (1.0f - k) * (1.0f - k) * (1.0f - k);
			x = -TORTOS_SCREEN_W * 0.35f + (cx + TORTOS_SCREEN_W * 0.35f) * e;
		} else if (t < T_IN + T_HEAD) {
			float k = (float)(t - T_IN) / (float)T_HEAD;
			head = 1.0f - k * k;          /* accelerating in, like a flinch */
		} else {
			float k = (float)(t - T_IN - T_HEAD) / (float)T_DIM;
			head = 0.0f;
			dim = 1.0f - (1.0f - k) * (1.0f - k) * (1.0f - k);
		}
		SDL_SetRenderDrawColor(a->r, 17, 19, 16, 255);
		SDL_RenderClear(a->r);
		draw_shell(a->r, x, cy, rad, head, dim, 255);
		plat_present(a->r);
		SDL_Delay(6);
	}
	/* Land on the closed state exactly, in case the loop exited a frame early,
	 * and then deliberately no clear: the dark shell is the last thing on
	 * screen and stays there while the device powers down. */
	SDL_SetRenderDrawColor(a->r, 17, 19, 16, 255);
	SDL_RenderClear(a->r);
	draw_shell(a->r, cx, cy, rad, 0.0f, 1.0f, 255);
	plat_present(a->r);
}

static void power_off(app *a)
{
	fprintf(stderr, "power: off\n");
	/* Here rather than only on the return path: launch.sh runs its leds_off at
	 * the TOP of its restart loop, and a power-off breaks that loop instead of
	 * going round it, so this is the last chance to darken them. */
	plat_leds_off();
	remember_place(a);
	book_keep(true);
	plat_request_poweroff();
	anim_poweroff(a);
	a->running = false;
}

/* ---------- menus --------------------------------------------------------- */

/* Both menus in TortOS are the same shape - a short list on a slab, over a
 * paused game frame or over the shelf - so they are drawn by one function and
 * cannot drift apart. The slab is sized to its own widest row with the same
 * padding on every side, rather than to a number picked once and left behind
 * by the next label someone adds. */
/* menu_row and the row kinds: src/menu.h; the rules: docs/menus.md. */

#define MENU_RADIUS 20
/* The system's one accent: this menu, the volume OSD, and the mark's center
 * cell in both animations. Hand-kept equal to markdef.CYAN and UI_CYAN_*. */
#define MENU_ACCENT 0x3DD6FFu

/* The unit every menu measurement is in. Row height, padding and the gap
 * between the two columns are all cut from it, so the whole panel scales with
 * the type rather than with a set of numbers that have to be retuned together. */
/* Two colors, `t` of the way from a to b. */
static SDL_Color col_mix(SDL_Color a, SDL_Color b, float t)
{
	if (t <= 0.0f) return a;
	if (t >= 1.0f) return b;
	return (SDL_Color){ (Uint8)(a.r + (b.r - a.r) * t),
	                    (Uint8)(a.g + (b.g - a.g) * t),
	                    (Uint8)(a.b + (b.b - a.b) * t),
	                    (Uint8)(a.a + (b.a - a.a) * t) };
}

static int menu_row_h(void) { return ui_font_line(UI_F_MENU) * 3 / 2; }
/* The air between the panel edge and its content, on each side. menu_draw
 * draws it and menu_std_width subtracts it, and a panel width means nothing
 * unless both use the same number. */
static int menu_pad(void)   { return menu_row_h() * 3 / 4; }

/* The air between the panel and the screen edge, on every side. */
#define MENU_MARGIN 24

/* How many row_h rows menu_draw can show beside `fixed` other full rows,
 * `rules` rules and `notes` notes, before it starts windowing.
 *
 * It windows from the TOP of the list, which is right for a plain menu and
 * wrong for a screen whose last rows are a footer: Play Time overflowed by
 * 42px and menu_draw answered by scrolling the footer off the bottom, so the
 * totals and the key legend were never on screen together. A screen that
 * builds furniture around a list has to know how much list is left, and this
 * is that arithmetic in one place rather than a constant tuned to one text
 * size - Text Size is a setting, and at 125% a hardcoded row count is wrong
 * again. */
static int menu_list_fit(int fixed, int rules, int notes)
{
	int pad = menu_pad(), row_h = menu_row_h();
	int head_h = ui_font_line(UI_F_LABEL) + pad;
	int avail = TORTOS_SCREEN_H - MENU_MARGIN * 2
	          - (head_h + pad / 2)          /* heading down to the first row */
	          - pad;                        /* and the panel's bottom air */

	avail -= fixed * row_h;
	avail -= rules * (pad / 2 + 2);
	avail -= notes * (ui_font_height(UI_F_MENU) + pad / 3);
	return avail < row_h ? 1 : avail / row_h;
}

/* How many BODY lines fit in a panel that is nothing but body - a synopsis
 * card. The same arithmetic menu_list_fit does, asked the other way round: that
 * one reserves notes and answers in rows, and a page with no rows on it at all
 * would get the wrong answer from it.
 *
 * Here rather than in the caller because the two must agree about the heading,
 * the padding and the note height, and a screen that measured itself would
 * drift from the panel that draws it the first time either is retuned. */
static int menu_notes_fit(void)
{
	int pad = menu_pad();
	int head_h = ui_font_line(UI_F_LABEL) + pad;
	int note_h = ui_font_height(UI_F_MENU) + pad / 3;
	int avail = TORTOS_SCREEN_H - MENU_MARGIN * 2
	          - (head_h + pad / 2) - pad;

	return note_h > 0 && avail > note_h ? avail / note_h : 1;
}

/* `fixed_w` is the content width to use, or 0 to size to these rows. The shelf
 * menus pass a width measured across both of them so the panel never resizes;
 * the in-game menu has no values to cycle and sizes to itself. */
/* `accent` is the panel's border and heading rule. The shelf's own menu passes
 * MENU_ACCENT because that menu is TortOS, not whichever card is under the
 * cursor; a menu that belongs to a system passes that system's color. */
/* `vcolors` is an optional array parallel to `rows`: a nonzero entry is the
 * 0xRRGGBB to draw that row's value in, whatever the cursor is doing. It is
 * not a field on menu_row because exactly one screen has any use for it, and
 * putting it there made all 69 initializers in the launcher declare that they
 * do not care. NULL, which is what menu_draw passes, is the ordinary rule. */
/* `visits_all` says the cursor can rest on EVERY row, whatever their `live`
 * says - which is only true of a list that uses `live` for something other
 * than selectability. The achievement list is the one: there `live` means
 * EARNED, and the cursor walks earned and unearned alike. Everywhere else the
 * two meanings coincide and this is false. */
/* `loop_at` is where the rows start repeating: rows[loop_at] is a second copy
 * of rows[0], laid out by the caller with a rule between the copies, so a body
 * too tall for its panel can scroll THROUGH and come round rather than running
 * backwards. 0 for every screen that does not do that, which is all but one.
 *
 * The caller passes it rather than this inferring it, because only the caller
 * knows the content is a repeat - and getting it wrong does not look like a
 * bug, it looks like a scroll that jumps. */
static void menu_draw_ex(app *a, const char *heading, const menu_row *rows,
                         int n, int sel, int fixed_w, unsigned accent,
                         const unsigned *vcolors, bool visits_all, int loop_at)
{
	TTF_Font *fm = ui_font(UI_F_MENU), *fh = ui_font(UI_F_LABEL);
	int row_h = menu_row_h();
	int pad = menu_pad();
	/* A rule is 2px of bar plus the gap the heading's rule gets below it, so
	 * the footer sits off the list by the same amount the first row sits off
	 * the heading. Giving it a whole row_h left it swimming.
	 *
	 * A note is one line of text and nothing else. row_h is 1.5x the line, so a
	 * note in a full row carried half a row of air underneath it and then the
	 * panel's own pad on top of that, which read as the footer floating off the
	 * bottom border. */
	int rule_h = pad / 2 + 2;
	int gap = row_h;                 /* between the label and value columns */
	int text_h = fm ? ui_font_box(fm) : row_h;
	int note_h = text_h + pad / 3;
#define ROW_H(r) (!(r).label ? rule_h : (ROW_IS_NOTE(r) ? note_h : row_h))
	/* Center the ink, not the em box. The box reserves a descender's depth
	 * below the baseline that labels like "Wi-Fi" and "Bluetooth" never use,
	 * so centering the box leaves the visible line riding high in its row and
	 * reads as a highlight sitting too low. Descent is negative, so half of it
	 * subtracted moves the line down onto the middle of the plate. */
	int ink_off = fm ? -ui_font_descent(fm) / 2 : 0;
	/* The heading band runs from the panel's top edge down to the rule, and
	 * the heading is centered inside it rather than hung a fixed distance from
	 * the top - otherwise retuning the heading's size moves it off center,
	 * which is exactly what happened when it grew. `content_off` is the panel
	 * top to the first row, so a panel with no heading just pads instead. */
	int notes_drawn = 0;   /* which marquee slot the next long note takes */
	int scroll = 0;        /* pixels the body is lifted by, cursorless */
	int scroll_h = 0;      /* the body viewport's height, 0 if nothing scrolls */
	int scroll_span = 0;
	int scroll_split = 0;  /* first pinned row: the rule, or n if there is none */
	int scroll_foot = 0;   /* the pinned rows' total height */
	bool pinned = false;   /* a cursor list with its footer pinned under it */
	/* A SMALL, FAST GIVE WHEN THE WINDOW SCROLLS - not a full slide.
	 *
	 * REPEAT_RATE_MS is 90, so any animation long enough to watch is
	 * interrupted before it lands; chasing the whole row's travel means
	 * permanently lagging behind the cursor. This takes the hard edge off
	 * instead: at most a third of a row of displacement, decaying fast, so it
	 * is never far from where it belongs and a held button stays honest.
	 *
	 * A decay rather than a timed ease for the same reason - there is no
	 * animation to interrupt, only a distance that shrinks. */
	float win_off = 0.0f;

	/* Where the highlight actually is. The text colors are weighed by how
	 * much of it covers each row rather than by `i == sel`, so a row lights
	 * as the plate arrives and dims as it leaves, and the two rows either
	 * side of a move cross over correctly without anyone tracking which row
	 * was left. */
	float plate_y = -1e9f, plate_h = 0.0f;
	int line_head   = ui_font_line(UI_F_LABEL);
	/* A heading may carry a second line, separated by a newline. Nothing else
	 * passes one; the achievements screen does, because its heading is a game
	 * title AND two counts, and on one line that was wider than the panel and
	 * got both its ends clipped. Splitting keeps both rather than choosing. */
	const char *head2 = heading ? strchr(heading, '\n') : NULL;
	char head1[192];
	int head_lines = head2 ? 2 : 1;
	int head_h      = heading ? line_head * head_lines + pad : 0;
	int content_off = heading ? head_h + pad / 2 : pad;
	bool two_col = false;
	int content_w = 0, i, k, rows_h = 0;
	SDL_Rect panel;
	int cx, content_x, content_y;
	/* The list can outgrow the screen from either end - the type scale turns
	 * up, and this menu is meant to gain rows - so the panel is capped to the
	 * screen and the rows window around the selection when they do not all
	 * fit. A menu that runs off the top is worse than one that scrolls. */
	const int margin = MENU_MARGIN;
	int vis = n, first = 0;

	for (i = 0; i < n; i++)
		if (rows[i].value && !ROW_IS_NOTE(rows[i])) two_col = true;
	for (i = 0; i < n; i++) {
		int w;

		if (!rows[i].label) continue;          /* a rule measures nothing */
		w = ui_text_width(fm, rows[i].label);
		if (two_col && rows[i].value && !ROW_IS_NOTE(rows[i]))
			w += gap + ui_text_width(fm, rows[i].value);
		if (w > content_w) content_w = w;
	}
	if (head2) {
		size_t n1 = (size_t)(head2 - heading);

		if (n1 >= sizeof head1) n1 = sizeof head1 - 1;
		memcpy(head1, heading, n1);
		head1[n1] = '\0';
		head2++;                                    /* past the newline */
	} else if (heading) {
		snprintf(head1, sizeof head1, "%s", heading);
	}

	if (heading) {
		int w = ui_text_width(fh, head1);
		int w2 = head2 ? ui_text_width(fh, head2) : 0;

		if (w2 > w) w = w2;
		if (w > content_w) content_w = w;
	}
	if (fixed_w > 0) content_w = fixed_w;
	if (content_w > TORTOS_SCREEN_W - margin * 2 - pad * 2)
		content_w = TORTOS_SCREEN_W - margin * 2 - pad * 2;

	{
		int sum = 0;
		for (i = 0; i < n; i++) sum += ROW_H(rows[i]);
		rows_h = sum;
	}
	if (content_off + rows_h + pad > TORTOS_SCREEN_H - margin * 2) {
		int avail = TORTOS_SCREEN_H - margin * 2 - content_off - pad;

		if (sel >= 0) {
			/* THE WINDOW ONLY MOVES WHEN THE CURSOR WOULD LEAVE IT.
			 *
			 * It used to center itself on the selection, which meant almost
			 * every press scrolled the list: the highlight stayed put in the
			 * middle of the panel and the rows jumped under it. That is the
			 * wrong thing to move. The cursor is what the player is aiming,
			 * so the cursor travels and the list holds still until it runs
			 * out of window.
			 *
			 * Static, and keyed on `n` like the highlight's own state, since
			 * menu_draw keeps nothing between frames and every screen shares
			 * it. A different row count is a different menu and starts at the
			 * top rather than inheriting someone else's scroll. */
			static int win_n = -1, win_first;
			int body_n = n, j;

			/* A FOOTER STAYS PUT. A rule followed by nothing but notes - the
			 * Wi-Fi screen's address and key legend - is about the list, not
			 * in it, and as the last rows of a list taller than the screen it
			 * was never on it: no cursor can walk down to a row it cannot
			 * select (plorpos-3pk.8, the RG SP's 768-unit screen). So it is
			 * pinned under the window the way a cursorless card pins its own,
			 * and only the rows above it scroll. */
			for (j = n - 1; j >= 0 && rows[j].label && !rows[j].live; j--) ;
			if (j > sel && j < n - 1 && !rows[j].label && !ROW_IS_HR(rows[j])) {
				pinned = true;
				body_n = scroll_split = j;
				for (; j < n; j++) scroll_foot += ROW_H(rows[j]);
				avail -= scroll_foot;
			}

			vis = avail / row_h;
			if (vis < 1) vis = 1;
			if (vis > body_n) vis = body_n;
			if (pinned) scroll_h = vis * row_h;

			if (win_n != n) win_first = 0;
			win_n = n;
			win_first = menu_window_first(rows, body_n, sel, vis, win_first);
			first = win_first;
		} else {
			/* NO CURSOR MEANS NOTHING CAN SCROLL IT, so it scrolls itself.
			 * Dropping rows off the bottom is what a cursor makes safe: you
			 * can always walk down to them. A card has no walk, so the rows
			 * all stay and the window moves over them instead, on the same
			 * ping-pong a long title runs sideways.
			 *
			 * ONLY THE BODY MOVES. Past the rule is the footer, and on the
			 * achievement card that is the points and whether it is earned -
			 * the two facts a reader wants WHILE reading the description, not
			 * after waiting out a scroll to reach them. The rule is already
			 * where this menu divides a list from its footer, so it is the
			 * split here too and nothing new has to be declared. */
			int body = 0, j;

			/* MENU_RULE only: a MENU_HR is punctuation inside the body,
			 * and treating it as the footer split would pin half a synopsis
			 * to the bottom of the panel. */
			scroll_split = n;
			for (j = 0; j < n; j++)
				if (!rows[j].label && !ROW_IS_HR(rows[j])) {
					scroll_split = j;
					break;
				}
			for (j = scroll_split; j < n; j++) scroll_foot += ROW_H(rows[j]);
			for (j = 0; j < scroll_split; j++) body += ROW_H(rows[j]);

			scroll_h = avail - scroll_foot;
			if (scroll_h < row_h) scroll_h = row_h;
			scroll_span = body - scroll_h;
			if (scroll_span < 0) scroll_span = 0;
			if (loop_at > 0 && loop_at < n) {
				/* One lap is everything before the repeat, so the wrap lands
				 * on the same picture the lap started from. What keeps the
				 * bottom of the viewport covered through a whole lap is that
				 * the second copy is as tall as the first, which is only
				 * worth scrolling at all when it is taller than the panel. */
				int lap = 0;

				for (j = 0; j < loop_at; j++) lap += ROW_H(rows[j]);
				scroll = ui_scrollthrough(lap,
				                          mq_phase(MQ_VSCROLL, n, rows_h));
			} else {
				scroll = ui_pingpong(scroll_span,
				                     mq_phase(MQ_VSCROLL, n, rows_h));
			}
		}
	}

	panel.w = content_w + pad * 2;
	{   /* the heights actually on screen, not vis * row_h */
		int sum = 0, j, last = first + vis - 1;

		for (j = first; j < first + vis && j < n; j++) sum += ROW_H(rows[j]);
		/* A scrolling card is as tall as its viewport plus its pinned footer.
		 * Summing the rows would size the panel to text that is deliberately
		 * not all on it. */
		panel.h = content_off + (scroll_h ? scroll_h + scroll_foot : sum) + pad;
		/* A panel that ends in a footer gets a smaller bottom pad. The full
		 * pad is there to keep list ITEMS off the border; a note is already
		 * held off the list by its rule and is meant to sit low.
		 *
		 * The gap under the footer text is pad/6 from centering plus whatever
		 * bottom pad is left. Set by eye on the device 2026-09-04, not derived:
		 * the full pad gives 1.17 and floats, 0.50 is too tight, and 0.75 is
		 * where Eric called it. 5/12 is the subtraction that leaves 0.75.
		 *
		 * A body row is not a footer. A synopsis short enough to fit without
		 * scrolling ends in prose, which is the panel's content and wants the
		 * same pad under it that a list does. */
		if (pinned) last = n - 1;            /* the footer is on screen */
		if ((!scroll_h || pinned) && last >= 0 && last < n && rows[last].label &&
		    ROW_IS_NOTE(rows[last]) && !ROW_IS_BODY(rows[last]))
			panel.h -= pad * 5 / 12;
	}
	panel.x = (TORTOS_SCREEN_W - panel.w) / 2;
	panel.y = (TORTOS_SCREEN_H - panel.h) / 2;
	cx = panel.x + panel.w / 2;
	content_x = panel.x + pad;
	content_y = panel.y + content_off;

	/* TortOS's own color, not the focused system's. The menu belongs to the
	 * launcher rather than to whatever card happens to be under the cursor, so
	 * the boot animation, the mark and this chrome are one accent: the device's
	 * first frame and the shelf agree.
	 *
	 * They did not always. The mark carried its own blue, (74,158,255), while
	 * this was (61,214,255), and comments in both files called them the same
	 * color without either having been checked against the other. Unified on
	 * this cyan on 2026-08-28, that being the direction that stays clear of the
	 * eleven system accents; the mark's old blue sat close to Genesis. */
	ui_glow(a->r, &panel, accent, 60, 1.5f);
	ui_panel(a->r, &panel, MENU_RADIUS, accent);

	if (heading) {
		/* Centered in the band by its ink, on the same reasoning as the rows:
		 * the em box carries descender depth that "TortOS" and "NES" do not use.
		 * This read "mostly do not" while the heading was "PlayOS", whose y was
		 * the exception; the shift is by font metrics rather than by the string,
		 * so nothing here changed with the name, it just got exactly true. */
		/* Center the INK between the panel's top and the rule, computed from
		 * the font rather than nudged by a fraction of the descent.
		 *
		 * The heading is capitals and lowercase with nothing below the
		 * baseline, so what should sit in the middle of that band is cap-top
		 * to baseline - not the em box, which carries a descender's depth of
		 * empty space at the bottom. Centering the box left "Wi-Fi" visibly
		 * high on the scanning screen.
		 *
		 * cap comes from 'H': maxy is its height above the baseline. */
		int asc = fh ? ui_font_ascent(fh) : line_head;
		int cap = fh ? ui_font_cap(fh) : asc;
		int block, hy;

		/* cap-to-baseline for the first line, plus a whole line for a second */
		block = cap + (head_lines - 1) * line_head;
		/* From the panel's INNER edge, not panel.y. The border is a visible
		 * frame and the eye reads the space inside it, so centering against
		 * the outer edge is arithmetically right and looks high by exactly
		 * the border's width - measured on the Wi-Fi panel as 18px above the
		 * ink against 32 below, which is what Eric saw. */
		hy = panel.y + UI_PANEL_BORDER
		     + (head_h - UI_PANEL_BORDER - block) / 2 - (asc - cap);

		/* Same rule for the heading, and for the same reason: art_screen and
		 * notice.c each fit their own before handing it over, which worked
		 * and meant two callers had to remember. */
		{
			/* A HEADING THAT DOES NOT FIT SCROLLS, like every other line in
			 * this launcher that does not fit. It used to be cut with an
			 * ellipsis and left there, which on the achievement list meant a
			 * game's own name was the one thing on screen you could not read:
			 * "Castlevania III - Dracula's C...". The panel is a fixed width
			 * shared by every menu, and a game title is whatever it is.
			 *
			 * Left-aligned while it moves, for the reason a long note is:
			 * text wider than the box has nothing left to center.
			 *
			 * Both lines share one slot, so when a heading has two they move
			 * together rather than taking turns resetting each other's clock -
			 * the same lesson the value marquee learned on Over The Hare. */
			char h1[192], h2[192];
			int hw = ui_text_width(fh, head1);
			unsigned ph = mq_phase(MQ_HEAD, (int)strlen(head1), content_w);

			if (hw <= content_w) {
				ui_fit_text(fh, head1, h1, sizeof h1, content_w);
				ui_text(a->r, fh, h1, cx, hy, 0, UI_TEXT_SOFT);
			} else {
				ui_text_marquee(a->r, fh, head1, content_x, hy, content_w,
				                ph, UI_TEXT_SOFT);
			}
			if (head2) {
				if (ui_text_width(fh, head2) <= content_w) {
					ui_fit_text(fh, head2, h2, sizeof h2, content_w);
					ui_text(a->r, fh, h2, cx, hy + line_head, 0, UI_TEXT_SOFT);
				} else {
					ui_text_marquee(a->r, fh, head2, content_x,
					                hy + line_head, content_w, ph,
					                UI_TEXT_SOFT);
				}
			}
		}
		SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(a->r, (Uint8)(accent >> 16),
		                       (Uint8)(accent >> 8), (Uint8)accent, 70);
		SDL_RenderFillRect(a->r, &(SDL_Rect){ content_x, panel.y + head_h,
		                                      content_w, 2 });
	}

	/* THE HIGHLIGHT SLIDES. It used to be drawn inside the row loop at
	 * i == sel, which put it wherever the cursor was that frame and made it
	 * snap. Here it is a rect of its own, eased from where it was toward
	 * where it belongs, and drawn BEFORE the rows so it stays under all of
	 * them - inside the loop, a plate that had moved up would cover the text
	 * of the row already drawn above it.
	 *
	 * The state is static because menu_draw has none and every screen shares
	 * it. `n` is the key: a different row count is a different menu, and the
	 * plate snaps rather than travelling across from wherever the last menu
	 * left it. Two menus of the SAME length in a row will slide between
	 * themselves, which is wrong but invisible - both are lists of the same
	 * shape and the plate is already close to where it should be. */
	if (vis < n) {
		static int      wn = -1, wfirst = -1;
		static float    wcur, wmid;
		static unsigned wlast;
		/* Two chained decays, not one. A single exponential moves furthest on
		 * its first frame and then crawls - 10.5px of a 16px give, then 6.8,
		 * then 4.4 - which is the front-loading that reads as a jerk. Chained
		 * gives an S: 1.8, 2.7, 2.9, 3.0, 2.6, near-equal steps through the
		 * middle where the eye is tracking.
		 *
		 * 55ms and a third of a row would be smoother again at 70ms, but the
		 * lag while a button is held grows with both: 0.28 of a row here,
		 * 0.44 there, and past that the list visibly trails the cursor. */
		const float TAU = 55.0f, CAP = (float)row_h * 0.55f;
		unsigned nw = plat_now_ms();
		float dt = wlast ? (float)(nw - wlast) : 0.0f;

		wlast = nw;
		if (dt > 100.0f) dt = 100.0f;
		if (wn != n) wcur = wmid = 0.0f;
		else if (wfirst != first) {
			int a0 = first < wfirst ? first : wfirst;
			int b0 = first < wfirst ? wfirst : first;
			float d = 0.0f;
			int j2;

			/* A STEP GIVES, A JUMP DOES NOT. Wrapping from the last row to
			 * the first moves the window the whole length of the list, and a
			 * shoulder button pages it by eight; easing across either drags
			 * rows in from the far end and reads as the list skipping. Only
			 * a single row's move is softened. */
			if (b0 - a0 > 1) {
				wcur = wmid = 0.0f;
			} else {
				for (j2 = a0; j2 < b0 && j2 < n; j2++) d += ROW_H(rows[j2]);
				wcur += (first > wfirst) ? d : -d;
				wmid += (first > wfirst) ? d : -d;
				if (wcur >  CAP) wcur =  CAP;
				if (wcur < -CAP) wcur = -CAP;
				if (wmid >  CAP) wmid =  CAP;
				if (wmid < -CAP) wmid = -CAP;
			}
		}
		wn = n; wfirst = first;
		if (dt > 0.0f) {
			float kk = 1.0f - expf(-dt / TAU);

			wmid -= wmid * kk;
			wcur += (wmid - wcur) * kk;
		}
		if (wcur < 0.5f && wcur > -0.5f) wcur = 0.0f;
		win_off = wcur;
	}

	if (sel >= 0 && sel < n && rows[sel].label) {
		static int   pl_n = -1, pl_sel = -1;
		static float pl_y, pl_h, pl_my, pl_mh;
		static unsigned pl_last;
		/* A CHASE, NOT A TIMED SLIDE. REPEAT_RATE_MS is 90 and this used to
		 * take 150, so holding the d-pad meant the plate never finished a
		 * move: it trailed a row behind, and on the wrap - which snaps - it
		 * jumped from halfway to the top. The last row before the wrap was
		 * being selected and never visibly highlighted, which is what it
		 * looked like from the outside.
		 *
		 * A chase has no duration to be interrupted. It closes whatever gap
		 * exists, so under repeat it keeps up and after a single press it
		 * eases. Two stages for the same reason the list uses two: one
		 * exponential moves furthest on its first frame. */
		/* 22ms, not 34. At 34 the plate is only 83% of the way across when
		 * the next repeat arrives, so holding the d-pad it never quite
		 * settles on a row - the last one before a wrap almost highlights and
		 * then the wrap takes it. 22 puts it at 96% within one repeat, which
		 * is the difference between a row being visited and a row being
		 * seen. Its first frame is still only 14px of 48, so a single press
		 * eases rather than snaps. */
		const float TAU = 22.0f;
		int ty_sel = content_y - scroll, jj;
		unsigned nowp = plat_now_ms();
		float dt = pl_last ? (float)(nowp - pl_last) : 0.0f;
		float tgt_h = (float)ROW_H(rows[sel]);
		SDL_Rect plate;

		pl_last = nowp;
		if (dt > 100.0f) dt = 100.0f;

		for (jj = first; jj < sel && jj < n; jj++) ty_sel += ROW_H(rows[jj]);
		ty_sel += (int)win_off;

		if (pl_n != n) {                       /* another menu: no journey */
			pl_y = pl_my = (float)ty_sel; pl_h = pl_mh = tgt_h;
		} else if (pl_sel != sel) {
			/* A step is a move to the next SELECTABLE row, whatever its
			 * index. Distance cannot tell a wrap from a step in a menu whose
			 * top rows are placeholders - the cursor goes 6 to 3 there, and
			 * a step past one placeholder goes 3 to 5. What separates them is
			 * whether anything selectable lies between. */
			int lo2 = sel < pl_sel ? sel : pl_sel;
			int hi2 = sel < pl_sel ? pl_sel : sel;
			bool jump = false;
			int q;

			for (q = lo2 + 1; q < hi2; q++)
				if (rows[q].label && rows[q].live) { jump = true; break; }
			if (jump) { pl_y = pl_my = (float)ty_sel; pl_h = pl_mh = tgt_h; }
		}
		pl_n = n; pl_sel = sel;

		if (dt > 0.0f) {
			float k = 1.0f - expf(-dt / TAU);

			pl_my += ((float)ty_sel - pl_my) * k;
			pl_mh += (tgt_h - pl_mh) * k;
			pl_y  += (pl_my - pl_y) * k;
			pl_h  += (pl_mh - pl_h) * k;
		}

		/* A soft white plate, not the system's color. The accent already
		 * frames the panel; using it again for the cursor made the two
		 * compete, and on a dark red system the plate read as a stain on the
		 * row rather than a highlight under it. White is neutral against all
		 * eleven accents.
		 *
		 * Flat, with no radial glow under it. The glow was brightest at the
		 * row's midpoint and fell off toward both ends, which put a soft blob
		 * behind the middle of every highlighted row and read as a smudge
		 * rather than as a selection. */
		plate.x = panel.x + pad / 2;
		plate.y = (int)(pl_y + 0.5f);
		plate.w = panel.w - pad;
		plate.h = (int)(pl_h + 0.5f);
		ui_round_rect(a->r, &plate, row_h / 4, (SDL_Color){ 255, 255, 255, 34 });

		plate_y = pl_y;
		plate_h = pl_h;
	}

	notes_drawn = 0;
	if (scroll_h)
		SDL_RenderSetClipRect(a->r, &(SDL_Rect){ panel.x, content_y,
		                                         panel.w, scroll_h });
	/* Clipped to the panel, and ONE ROW BEYOND THE WINDOW at each end.
	 * Offsetting the rows opens a gap at one edge and pushes the far row past
	 * the other; without the extra row the gap is empty, and without the clip
	 * the overflow draws outside the panel. Leaving either out is what made
	 * the first attempt at this look like the list was wrapping. */
	if (vis < n) {
		SDL_Rect rc = { panel.x + UI_PANEL_BORDER, content_y,
		                panel.w - UI_PANEL_BORDER * 2,
		                pinned ? scroll_h : panel.y + panel.h - pad - content_y };
		SDL_RenderSetClipRect(a->r, &rc);
	}
	for (k = 0, i = (first > 0 ? first - 1 : first); i < n; k++, i++) {
		int y, ty, j;
		float sel_w;
		SDL_Color lc, vc;

		/* Past the window's spare row: done, or on to the pinned footer. */
		if (i > first + vis && !(pinned && i >= scroll_split)) {
			if (!pinned) break;
			i = scroll_split - 1;
			continue;
		}

		if (scroll_h && i >= scroll_split) {
			/* Pinned. Measured from the bottom of the viewport rather than
			 * from the top of the rows, so the footer does not move when the
			 * body does. The clip comes off here and stays off. */
			if (i == scroll_split) SDL_RenderSetClipRect(a->r, NULL);
			y = content_y + scroll_h;
			for (j = scroll_split; j < i; j++) y += ROW_H(rows[j]);
		} else {
			y = content_y - scroll + (int)win_off;
			if (i >= first) for (j = first; j < i; j++) y += ROW_H(rows[j]);
			else            for (j = i; j < first; j++) y -= ROW_H(rows[j]);
		}
		/* Centered in ITS OWN row, not in row_h: a note is shorter. */
		ty = y + (ROW_H(rows[i]) - text_h) / 2 + ink_off;

		/* The same bar as the heading's, at the same alpha and width. If one
		 * is ever retuned the other follows, which is the point. */
		if (!rows[i].label) {
			SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
			SDL_SetRenderDrawColor(a->r, (Uint8)(accent >> 16),
			                       (Uint8)(accent >> 8), (Uint8)accent, 70);
			SDL_RenderFillRect(a->r, &(SDL_Rect){ content_x,
			                                      y + rule_h - 2,
			                                      content_w, 2 });
			continue;
		}

		/* A placeholder row still highlights - it is a real place on the list -
		 * and stays one step quieter than a working row, which is the whole
		 * signal that it does nothing yet. One step, though, not two: most of
		 * this list is placeholders, and ranking them against an unselected
		 * working row as well left the entire menu reading as grayed out. */
		/* `w` is how selected this row is RIGHT NOW, not whether it is the
		 * one the cursor points at: 1 once the plate has arrived, 0 before it
		 * sets off, and the two rows crossfade past each other in between. */
		{
			/* How much of the plate is over this row, 0 to 1. */
			float top = y > plate_y ? (float)y : plate_y;
			float bot = (float)(y + ROW_H(rows[i])) < plate_y + plate_h
			            ? (float)(y + ROW_H(rows[i])) : plate_y + plate_h;
			float w = (bot - top) / (float)ROW_H(rows[i]);

			if (w < 0.0f) w = 0.0f;
			if (w > 1.0f) w = 1.0f;

			lc = rows[i].live
			     ? col_mix(UI_TEXT_SOFT, UI_TEXT, w)
			     : col_mix(UI_TEXT_DIM, UI_TEXT_SOFT, w);
			sel_w = w;
		}
		/* The panel's own accent, not a->tint. These were the same value for
		 * as long as Display mode was the only live row, because that row is
		 * in the SYSTEM menu where the accent IS the system tint. The first
		 * live row in the TortOS menu made them diverge and drew a red value
		 * inside a cyan panel. `accent` is already MENU_ACCENT for one menu
		 * and the system tint for the other, which is the answer in both. */
		if (vcolors && vcolors[i])
			vc = (SDL_Color){ (Uint8)(vcolors[i] >> 16),
			                  (Uint8)(vcolors[i] >> 8),
			                  (Uint8)vcolors[i], 255 };
		else
			vc = rows[i].live
			     ? col_mix(UI_TEXT_DIM,
			               (SDL_Color){ (Uint8)(accent >> 16),
			                            (Uint8)(accent >> 8),
			                            (Uint8)accent, 255 }, sel_w)
			     : UI_TEXT_DIM;

		/* Trimmed to the panel, HERE, rather than by every caller.
		 *
		 * content_w is capped to the screen above, but nothing used to fit
		 * the text to it - the label was drawn from the left edge and the
		 * value right-aligned to the right, so a row wider than the cap drew
		 * them straight through each other and out past the border. Seen on
		 * the game info screen: "File" and "Alex Kidd in Miracle World
		 * (World) (Sega Ages).zip" overlapping in the middle of the panel.
		 *
		 * Callers were each remembering to call ui_fit_text, or forgetting.
		 * A rule about how text fits a panel belongs in the thing that draws
		 * the panel, where a screen written next year gets it for free.
		 *
		 * The VALUE yields first: labels are short and fixed ("File", "Size"),
		 * values are whatever the card happens to hold. */
		if (ROW_IS_NOTE(rows[i])) {
			char note[192];

			/* A note that fits is centered, which is what a caption wants.
			 * One that does not SCROLLS rather than being cut: a note is a
			 * sentence, and Play Time's footer lost the last of four facts
			 * to an ellipsis - "longest 56m 30s · t..." - where the reader
			 * cannot even tell which word went. Same ping-pong and the same
			 * faded edges a long title gets on the shelf.
			 *
			 * Left-aligned in that case, not centered: the text is wider
			 * than the box, so centering it has nothing left to center.
			 *
			 * A body row is left-aligned whether it fits or not. It is one
			 * line of a paragraph, and a paragraph needs one left edge. */
			if (ui_text_width(fm, rows[i].label) <= content_w) {
				bool body = ROW_IS_BODY(rows[i]);

				ui_fit_text(fm, rows[i].label, note, sizeof note, content_w);
				ui_text(a->r, fm, note, body ? content_x : cx, ty,
				        body ? -1 : 0, lc);
			} else {
				int slot = MQ_NOTE0 + (notes_drawn < MQ_NOTES - 1
				                       ? notes_drawn : MQ_NOTES - 1);

				ui_text_marquee(a->r, fm, rows[i].label, content_x, ty,
				                content_w, mq_phase(slot, sel, i), lc);
			}
			notes_drawn++;
		} else if (two_col) {
			char lbl[192], val[192];
			int lw = ui_text_width(fm, rows[i].label);
			int vw = rows[i].value ? ui_text_width(fm, rows[i].value) : 0;
			int room;

			/* WHICHEVER IS LONGER IS THE CONTENT, and the content is what
			 * moves. The other one is a caption, and a caption keeps its
			 * natural width.
			 *
			 * On the play-time list the label is a game name and the value a
			 * duration, so the name scrolls and the number stays put; a
			 * duration pushed off the row it exists to report is no use to
			 * anyone. On the game info screen it is the other way round -
			 * "File" is the caption and the filename is the content - and
			 * the same rule sends the filename to the marquee without
			 * needing to know which screen it is on.
			 *
			 * The condition used to ask whether the label fit in what the
			 * value left, which answers a different question: with a long
			 * value that subtraction goes negative, so a two-letter label
			 * failed it and took the long-LABEL path. The filename was then
			 * drawn ellipsized across the full width with "File" printed on
			 * top of it, which is what sent me looking.
			 *
			 * Only the selected row moves, OR one the cursor can never reach
			 * - menu_run_body steps off any row whose `live` is false, so on
			 * a screen of facts, waiting for selection waits forever. Rows
			 * long enough to move are rare either way, and a page where
			 * everything is moving will not sit still to be read. */
			/* See menu_row_moves. The rule lives in menu.c because the
			 * last time it quietly meant something else, nothing could see
			 * it: the achievement list scrolled a title or cut it depending
			 * on whether the player had earned it. */
			bool moves = menu_row_moves(rows[i], i == sel, visits_all);

			if (lw + (vw ? gap + vw : 0) <= content_w) {
				ui_fit_text(fm, rows[i].label, lbl, sizeof lbl, content_w);
				ui_text(a->r, fm, lbl, content_x, ty, -1, lc);
				if (rows[i].value)
					ui_text(a->r, fm, rows[i].value,
					        content_x + content_w, ty, 1, vc);
			} else if (vw > lw) {
				/* The VALUE is the long one. The label keeps its width and
				 * the value takes everything left of the right edge. */
				ui_text(a->r, fm, rows[i].label, content_x, ty, -1, lc);
				room = content_w - lw - gap;
				if (room <= 0) {
					/* No room for it at all; the label alone is the row. */
				} else if (moves) {
					/* One slot for every long value on the screen, never one
					 * per row: keyed per row, a screen of facts with two of
					 * them reset the clock twice a frame and neither ever
					 * moved. Over The Hare showed it 2026-09-14 - "Transferred"
					 * and "Now" both too long, and two captures a second apart
					 * identical. Sharing the clock, they all move.
					 *
					 * WHICH clock depends on why this row moves, not on what
					 * is in it. See MQ_FIXED. */
					ui_text_marquee(a->r, fm, rows[i].value,
					                content_x + content_w - room, ty, room,
					                mq_row_phase(i == sel, heading, n, sel), vc);
				} else {
					ui_fit_text(fm, rows[i].value, val, sizeof val, room);
					ui_text(a->r, fm, val, content_x + content_w, ty, 1, vc);
				}
			} else {
				/* The LABEL is the long one: a game's name, an SSID. */
				room = content_w - (vw ? vw + gap : 0);
				if (rows[i].value) {
					ui_fit_text(fm, rows[i].value, val, sizeof val, content_w);
					ui_text(a->r, fm, val, content_x + content_w, ty, 1, vc);
				}
				if (room <= 0) {
					/* Nothing left for a name. Better a cut label than none. */
					ui_fit_text(fm, rows[i].label, lbl, sizeof lbl, content_w);
					ui_text(a->r, fm, lbl, content_x, ty, -1, lc);
				} else if (moves) {
					ui_text_marquee(a->r, fm, rows[i].label, content_x, ty,
					                room,
					                mq_row_phase(i == sel, heading, n, sel), lc);
				} else {
					ui_fit_text(fm, rows[i].label, lbl, sizeof lbl, room);
					ui_text(a->r, fm, lbl, content_x, ty, -1, lc);
				}
			}
		} else {
			char lbl[192];

			/* A centered list's row, under the two-column rule: the row that
			 * moves slides when it does not fit, rather than losing its end
			 * to an ellipsis. Left-aligned while it slides, the way a long
			 * note is - wider than the box, it has nothing to be centered in.
			 *
			 * This branch only ever cut. Found on Muse's track list
			 * 2026-09-19: "Bird Dream Of The Olympus Mons" ended in "M..."
			 * - and slid the moment a track of that album was playing,
			 * because the "playing" value put the list in two columns. */
			if (ui_text_width(fm, rows[i].label) > content_w &&
			    menu_row_moves(rows[i], i == sel, visits_all)) {
				ui_text_marquee(a->r, fm, rows[i].label, content_x, ty,
				                content_w,
				                mq_row_phase(i == sel, heading, n, sel), lc);
			} else {
				ui_fit_text(fm, rows[i].label, lbl, sizeof lbl, content_w);
				ui_text(a->r, fm, lbl, cx, ty, 0, lc);
			}
		}
	}
	if (vis < n) SDL_RenderSetClipRect(a->r, NULL);

	if (scroll_h) {
		/* Faded where the text runs out of viewport, and faded by laying the
		 * PANEL'S OWN FILL over it, which is the opposite of what the shelf
		 * does. There the background is a coverflow with a vignette, so a
		 * gradient painted on top would show as a band and the glyphs have to
		 * be faded instead. Here the surface under the text is one flat color,
		 * ui_panel's 22,24,32, so the cheap way is also the right one.
		 *
		 * A side fades only while something is hidden on it, so a card sitting
		 * at the top of its travel has a hard top edge and a soft bottom one,
		 * and says by that which way it is about to move. */
		const int fade = row_h;
		int e;

		SDL_RenderSetClipRect(a->r, NULL);
		SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
		for (e = 0; e < fade; e++) {
			Uint8 al = (Uint8)(252 - 252 * e / fade);

			SDL_SetRenderDrawColor(a->r, 22, 24, 32, al);
			if (scroll > 0)
				SDL_RenderFillRect(a->r, &(SDL_Rect){
					panel.x + UI_PANEL_BORDER, content_y + e,
					panel.w - UI_PANEL_BORDER * 2, 1 });
			if (scroll < scroll_span)
				SDL_RenderFillRect(a->r, &(SDL_Rect){
					panel.x + UI_PANEL_BORDER, content_y + scroll_h - 1 - e,
					panel.w - UI_PANEL_BORDER * 2, 1 });
		}
		SDL_RenderSetClipRect(a->r, NULL);
	}

	/* Three dim dots where the list carries on, in the same vocabulary the
	 * slot carousel's rail uses. Hung just off the rows rather than centered in
	 * the padding: with a heading above, the padding is already spoken for by
	 * the separator, and the indicator belongs to the list in any case. */
	if (vis < n && (!scroll_h || pinned)) {
		/* A triangle pointing the way the list continues, rather than three
		 * dots that said "there is more" without saying which way. Drawn as
		 * rows because there is no filled-triangle primitive and this needs
		 * no texture. */
		const int tw = row_h / 3, th = tw / 2, off = 8;

		SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(a->r, 138, 143, 163, 200);
		for (k = 0; k < th; k++) {
			int up_w   = tw * (k + 1) / th;      /* apex at the top */
			int down_w = tw * (th - k) / th;     /* apex at the bottom */

			if (first > 0)
				SDL_RenderFillRect(a->r, &(SDL_Rect){ cx - up_w / 2,
				                   content_y - off - th + k, up_w, 1 });
			if (first + vis < (pinned ? scroll_split : n))
				SDL_RenderFillRect(a->r, &(SDL_Rect){ cx - down_w / 2,
				                   content_y + vis * row_h + off + k, down_w, 1 });
		}
	}
}

static void menu_draw(app *a, const char *heading, const menu_row *rows, int n,
                      int sel, int fixed_w, unsigned accent)
{
	menu_draw_ex(a, heading, rows, n, sel, fixed_w, accent, NULL, false, 0);
}

/* The two menus behind MENU, their row indices and their buffers: src/sys_menu.h */

/* Build whichever menu the current screen calls for. Returns the row count, so
 * the input loop never needs to know which of the two it is driving. */
/* One answer per two seconds, for every row that needs it.
 *
 * wifi_status forks wpa_cli, and this launcher is a 119 MB process - forking it
 * copies page tables and costs about thirty milliseconds, not the seven a small
 * process pays. menu_build runs every frame, and the TortOS menu asked three
 * times: once cached for the Wi-Fi row, then twice more, uncached, for Over The
 * Hare and Box Art. Measured on the device that was 12 fps with the menu open,
 * against a loop that delays 8 ms - two thirds of every frame spent forking.
 *
 * The cache existed and said why; the two rows below it defeated it. So the
 * cache moves out here where it covers all of them, and nothing calls
 * wifi_status directly from a per-frame path again. */
static wifi_state menu_wifi(char *ssid, int cap)
{
	static unsigned   next;
	static wifi_state st = WIFI_OFF;
	static char       name[WIFI_SSID_MAX];
	unsigned now = plat_now_ms();

	if (next == 0 || now >= next) {
		st = wifi_status(name, sizeof name, NULL, 0);
		next = now + 2000;
	}
	if (ssid && cap) snprintf(ssid, (size_t)cap, "%s", name);
	return st;
}

/* Gather what the device currently is, and hand it to sys_menu_build.
 *
 * The split is ADR-0001's: this half is allowed to fork wpa_cli and read the
 * app, the other half is not allowed to do anything but produce rows. That is
 * what lets tools/menu-check.c state a whole device situation in a struct
 * initializer and ask what the menu says about it.
 *
 * Returns the row count, so the input loop never needs to know which of the
 * two menus it is driving. */
/* What either MENU menu, and the plorpOS menu's submenus, need to know about
 * the device. `ss` holds the network's name and has to outlive `u_`. */
static void menu_ui(app *a, screen_id screen, int sys, sys_ui *u_,
                    char *ss, size_t ss_n)
{
	sys_ui u = { 0 };

	/* Asked once. The cache is why - see menu_wifi - and three rows used to
	 * ask separately, which is how it got expensive in the first place. */
	u.wifi  = menu_wifi(ss, ss_n);
	u.ssid  = ss;
	u.games = screen == SCREEN_GAMES;

	if (u.games) {
		const system_cfg *sc = &a->sys.systems[sys];

		u.sys_name   = sc->name;
		u.sys_core   = sc->core;
		/* By what it HAS, not by its name or its position: a shelf with no
		 * core and no folder is the built one, and a check can hand that
		 * over without inventing a flag day. */
		u.muse       = is_muse(sc);
		u.fav        = !u.muse && sc->core[0] == '\0' && sc->folder[0] == '\0';
		u.game_count = a->view[sys].list.count;
		u.dmode      = DMODES[a->view[sys].dmode].label;
		u.engine     = is_pico8(sc) ? ENGINE_LABEL(a->view[sys].native) : NULL;
		u.muse_books = u.muse && muse_books_shown();
		u.muse_both  = u.muse && ml_count(&g_muse, true) && ml_count(&g_muse, false);
		u.sort       = u.muse ? ml_order_label((ml_order)a->view[sys].sort, u.muse_books)
		                      : SORTS[a->view[sys].sort].label;
	} else {
		aout_state ao = aout_now();

		u.ss_have   = ss_have_dev();
		u.ss_in     = ss_signed_in();
		u.ss_name   = u.ss_in ? ss_user() : NULL;
		u.cards     = CARD_SETS[g_cards].name;
		u.cards_dir = CARD_DIRS[g_dir].name;
		u.auto_off  = a->auto_off;
		u.auto_poweroff = a->auto_poweroff;
		u.suspend_timeout = plat_suspend_timeout_secs();
		u.mute_lock = db_get_int(db_dev(), "muteswitch", 0) == 1;
		u.batt_pct = db_get_int(db_dev(), "battpct", 0) == 1;
		u.audio_policy = ao.policy;
		u.audio_dest   = aout_actual(&ao);

		/* The connected headset's name.
		 *
		 * Cached for two seconds, the same way the Wi-Fi row is and for the
		 * same reason: this is rebuilt every menu frame and the answer costs
		 * a fork. The first version read /tmp/tortos_btsink to avoid that,
		 * which is what made this row say "not connected" while the Bluetooth
		 * screen said the opposite - launch.sh writes that file on a
		 * twenty-second poll. One source for both now. */
		{
			static char btname[BT_NAME_MAX];
			static unsigned next_bt;
			unsigned now = plat_now_ms();

			if (next_bt == 0 || now >= next_bt) {
				bt_device bd[BT_MAX];
				int nb = bt_bonded(bd, BT_MAX), i;

				next_bt = now + 2000;
				btname[0] = '\0';
				if (nb > 0 && bt_status() == BT_READY) {
					bt_mark_connected_now(bd, nb);
					for (i = 0; i < nb; i++)
						if (bd[i].connected) {
							snprintf(btname, sizeof btname, "%s", bd[i].name);
							break;
						}
				}
			}
			u.bt_name = btname[0] ? btname : NULL;
		}
	}
	*u_ = u;
}

static int menu_build(app *a, screen_id screen, int sys,
                      menu_row *out, menu_bufs *b, const char **heading)
{
	char ss[WIFI_SSID_MAX];
	sys_ui u;

	menu_ui(a, screen, sys, &u, ss, sizeof ss);
	return sys_menu_build(&u, out, b, heading);
}


/* ---------- the WiFi screen ----------------------------------------------- */

/* One frame with a single line on it, drawn before each blocking call.
 *
 * wifi_up can take twenty seconds waiting out the stock service's retry loop,
 * a scan takes several, and an association takes up to twenty more. None of
 * that is asynchronous here, so the loop is not running and input is not read
 * while it happens. Painting the reason first is the difference between a
 * device that is working and a device that has hung: the picture is identical
 * otherwise, and the second reading is the one that gets a power cycle. */
/* One line on a panel over the shelf, for the moments something is happening
 * and there is nothing yet to show. The heading is a parameter because it was
 * not: this began as the Wi-Fi screen's own and said "Wi-Fi" over every
 * message it was given, so signing in to RetroAchievements and fetching a set
 * both announced themselves as Wi-Fi. */
static void wait_panel(app *a, const char *heading, const char *msg)
{
	menu_row row = { msg, NULL, false };

	draw_shelf(a);
	SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(a->r, 0, 0, 0, 120);
	SDL_RenderFillRect(a->r, NULL);
	menu_draw(a, heading, &row, 1, -1, 0, MENU_ACCENT);
	draw_chrome(a->r);
	plat_present(a->r);
}

/* MENU CLOSES THE WHOLE MENU, from any depth in it. B is back one level.
 * Eric's call, 2026-09-23: MENU used to mean exactly what B meant, so from
 * Play Time or Wi-Fi it went back one screen instead of out, the same button
 * doing one thing to open the menu and another inside it.
 *
 * A menu screen that sees MENU raises this and leaves; every screen under it
 * then leaves on its next pass, until the one MENU opened is gone. It is put
 * down again only OUTSIDE the menus - the shelf's loop, game_menu, Muse - and
 * never when a menu opens: a screen that opens a second one after the first
 * came back has to see it still up, or that second one would stay open. */
static bool g_menu_closing;

/* True when a menu screen should leave: B, MENU, or a MENU pressed deeper in. */
static bool menu_leaving(app *a)
{
	if (a->in.pressed[IN_MENU]) g_menu_closing = true;
	return a->in.pressed[IN_BACK] || g_menu_closing;
}

/* A yes/no panel, drawn like wait_panel but answerable.
 *
 * Defaults to "no": the selection starts on the safe row, so a stray press of
 * the button that opened the panel cannot also confirm it. Returns false for
 * BACK, for MENU, and for a power-off, because everything except a deliberate
 * press on the destructive row means "not that".
 *
 * Power is handled here rather than left to the caller. A confirm can sit on
 * screen indefinitely, which makes it exactly the kind of place the device gets
 * put down and the power button pressed. */
/* ...and its general form: `n` answers, then Cancel. Returns the answer's
 * index, or -1 for Cancel and everything confirm_panel counts as "not that".
 * Settings > Scraping's import is the caller with two (TortOS-mh0). */
static int pick_panel(app *a, const char *heading, const char *msg,
                      const char *const *opts, int n)
{
	menu_row rows[4];
	int sel, i;

	if (n > 2) n = 2;
	rows[0] = (menu_row){ msg, NULL, false };
	for (i = 0; i < n; i++) rows[1 + i] = (menu_row){ opts[i], NULL, true };
	rows[1 + n] = (menu_row){ "Cancel", NULL, true };
	sel = 1 + n;

	for (;;) {
		plat_input_poll(&a->in);
		if (a->in.quit_requested) { a->running = false; return -1; }
		{
			pwr_action pa = power_check(a);
			if (pa == PWR_POWEROFF) { power_off(a); return -1; }
		}
		if (menu_leaving(a)) return -1;

		/* Only the answerable rows are reachable; row 0 is the question. */
		if (in_repeat(&a->in, IN_UP))   sel = sel > 1 ? sel - 1 : 1 + n;
		if (in_repeat(&a->in, IN_DOWN)) sel = sel < 1 + n ? sel + 1 : 1;
		if (a->in.pressed[IN_ACCEPT]) return sel <= n ? sel - 1 : -1;

		tick_tint(a);
		draw_shelf(a);
		SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(a->r, 0, 0, 0, 120);
		SDL_RenderFillRect(a->r, NULL);
		menu_draw(a, heading, rows, 2 + n, sel, 0, MENU_ACCENT);
		draw_chrome(a->r);
		plat_present(a->r);
		SDL_Delay(8);
	}
}

static bool confirm_panel(app *a, const char *heading, const char *msg,
                          const char *yes_label)
{
	return pick_panel(a, heading, msg, &yes_label, 1) == 0;
}

/* Rows to read and nothing to choose, until A or B: a job's result. */
static void note_panel(app *a, const char *heading, const menu_row *rows, int n)
{
	for (;;) {
		plat_input_poll(&a->in);
		if (a->in.quit_requested) { a->running = false; return; }
		{
			pwr_action pa = power_check(a);
			if (pa == PWR_POWEROFF) { power_off(a); return; }
		}
		if (menu_leaving(a) || a->in.pressed[IN_ACCEPT]) return;

		tick_tint(a);
		draw_shelf(a);
		SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(a->r, 0, 0, 0, 120);
		SDL_RenderFillRect(a->r, NULL);
		menu_draw(a, heading, rows, n, -1, 0, MENU_ACCENT);
		draw_chrome(a->r);
		plat_present(a->r);
		SDL_Delay(8);
	}
}

/* ---- the menu runner (ADR-0001) --------------------------------------- */

/* Wi-Fi is the largest menu: the switch, WIFI_MAX_NETS networks, a rule and a
 * note. Distinct from MENU_MAX_ROWS, which is the system menu's own count and
 * is much smaller. */
#define MENU_RUN_ROWS (WIFI_MAX_NETS + 8)

typedef enum {
	MENU_STAY,   /* keep the screen up */
	MENU_DONE    /* leave it */
} menu_result;

/* How a screen ended, for the callers that have to act differently. The
 * in-game menu is the one that does: B and MENU mean Continue there, and it
 * has to send RESUME - but the runner owns those two buttons and flushes the
 * input on the way out, so asking a->in afterwards would always say no. */
typedef enum {
	MENU_LEFT_BACK,     /* B or MENU, the ordinary way out */
	MENU_LEFT_SCREEN,   /* the screen's own handler said so */
	MENU_LEFT_GONE      /* quit, power, or nothing left to show */
} menu_exit;

/* Fill `rows`, name the screen, and return how many rows. MUST NOT touch the
 * renderer or the event loop, and the part that actually decides what the rows
 * SAY must be a separate function in a file the check can link - src/wifi_menu.c
 * and src/sys_menu.c are those files. ADR-0001 says the decision has failed if
 * that stops being true.
 *
 * Reading the device from here is allowed and unavoidable: a row that reports
 * the network has to ask every frame. What makes it checkable is that the
 * asking is confined to this half - wifi.c is stubbed on a host build, and the
 * system menu's asking lives in menu_build rather than in sys_menu_build.
 *
 * Called every frame, so it is also where live state is reflected - there is no
 * separate "refresh".
 *
 * The heading comes from here because it is a function of the same state the
 * rows are: the system menu is titled with the system whose rows it is showing.
 * Passing it to the runner separately meant keeping two things in step that are
 * really one thing. */
typedef int (*menu_build_fn)(void *ctx, menu_row *rows, int max,
                             const char **heading);

/* How the panel is drawn, as opposed to what is in it. Everything here needs
 * the renderer or the app, which is exactly why it is not in the build
 * function - see ADR-0001. */
typedef struct {
	/* 0: measure these rows. The system menu passes menu_shelf_width, a width
	 * measured once across every system and every display mode, because a
	 * panel that resizes while you cycle a value on it reads as a glitch. */
	int      fixed_w;
	unsigned accent;
	/* The shelf's animated tint instead of `accent`, read per frame. The
	 * system menu on a game shelf belongs to that system, and tick_tint is
	 * still moving toward its color while the menu is open. */
	bool     follow_tint;
	/* What goes behind the panel. NULL is the shelf, tinted and dimmed, which
	 * is what every screen reached from the shelf wants. The in-game menu
	 * draws the paused game instead, because the shelf is not what is under
	 * it. Called before the panel, every frame. */
	void   (*backdrop)(app *a, void *ctx);
	/* What the power button and the idle timer mean here. NULL powers the
	 * device off. The in-game menu stops the GAME instead: the device is not
	 * going anywhere, and the wait loop below it has to be told, or it goes
	 * straight back to waiting on a game nobody is running. */
	menu_result (*on_power)(app *a, void *ctx);
	/* The row the cursor starts on: 0, the top, unless a list opens on the
	 * entry already chosen (the in-game Shader list). */
	int      start;
} menu_style;

/* SELECT: Muse, over whatever is on screen. Declared here, after menu_style,
 * because it borrows two of its hooks. */
static bool muse_open(app *a, const menu_style *over, void *ctx);

/* Read every frame through the caller's pointer, so a screen that keeps its
 * style inside its own context can change it between frames. The info screen
 * does: its accent belongs to the system that OWNS the game, and toggling a
 * favorite on the Favorites shelf can hand it a different one. */

/* A key the runner did not consume, and the row it happened on. Free to open a
 * confirm, a keyboard or a wait panel: each runs its own loop and returns.
 * `sel` indexes the rows the last build produced. */
typedef menu_result (*menu_key_fn)(app *a, void *ctx, in_button key, int sel);

/* The loop itself. Called only through menu_run below, which owns the flush on
 * either side of it. */
static menu_exit menu_run_body(app *a, const menu_style *st,
                               menu_build_fn build, menu_key_fn on_key,
                               void *ctx)
{
	menu_row rows[MENU_RUN_ROWS];
	const char *heading = NULL;
	int sel = st->start, n = 0, b;

	while (a->running && !want_quit) {
		n = build(ctx, rows, MENU_RUN_ROWS, &heading);
		if (n > MENU_RUN_ROWS) n = MENU_RUN_ROWS;
		if (n <= 0) return MENU_LEFT_GONE;

		/* The list is rebuilt every frame and can shrink under the cursor -
		 * a rescan finding fewer networks, a row going away. Clamp first,
		 * then land on something live. */
		if (sel >= n) sel = n - 1;
		if (sel < 0) sel = 0;
		if (!rows[sel].live) sel = menu_step_sel(rows, n, sel, +1);

		plat_input_poll(&a->in);
		/* Where the sound goes, followed in the menus too: a headset connected
		 * from the Bluetooth screen, or a cable plugged in, only took the sound
		 * once the menus were left - seen 2026-09-26. It sends only on a change.
		 * And Muse is read here as well: its answers - where it now is, and the
		 * end of a track, which is when the next is sent - waited for the menus
		 * to be left, so the row read stale and an album stopped at the end of a
		 * song while a menu was open. #45. */
		muse_poll();
		aout_apply(false);
		if (a->in.quit_requested) { a->running = false; return MENU_LEFT_GONE; }
		{
			pwr_action pa = power_check(a);
			if (pa == PWR_POWEROFF) {
				if (!st->on_power) { power_off(a); return MENU_LEFT_GONE; }
				if (st->on_power(a, ctx) == MENU_DONE) return MENU_LEFT_GONE;
			}
		}
		if (menu_leaving(a))
			return MENU_LEFT_BACK;
		/* The in-game menu too: its game is paused underneath, and Muse
		 * borrows its backdrop and its power rule for as long as it is up.
		 * Rebuilt on the way back, because anything may have changed while
		 * Muse covered it - a Bluetooth headset connecting, for one. */
		if (a->in.pressed[IN_SELECT]) {
			if (muse_open(a, st, ctx)) return MENU_LEFT_GONE;
			continue;
		}

		if (in_repeat(&a->in, IN_UP))   sel = menu_step_sel(rows, n, sel, -1);
		if (in_repeat(&a->in, IN_DOWN)) sel = menu_step_sel(rows, n, sel, +1);

		/* Volume and brightness keep working here, as they do everywhere.
		 *
		 * They were missed when this runner was written, so the Wi-Fi screen
		 * spent a day as the one screen in the launcher where the volume keys
		 * did nothing. That is the exact failure this runner exists to make
		 * impossible, arriving by the one route it could still come from: the
		 * runner forgetting, rather than a screen forgetting. Every other loop
		 * in this file carries these four lines by hand. */
		if (in_repeat(&a->in, IN_VOLUP))    plat_volume_nudge(+1);
		if (in_repeat(&a->in, IN_VOLDN))    plat_volume_nudge(-1);
		if (in_repeat(&a->in, IN_BRIGHTUP)) plat_brightness_nudge(+1);
		if (in_repeat(&a->in, IN_BRIGHTDN)) plat_brightness_nudge(-1);

		/* Left and right reach the screen on REPEAT, not just on press, so a
		 * row whose value cycles can be held down. Everything else is edge
		 * triggered: holding A on a row that opens a screen should open it
		 * once. Sent before the press loop below, which skips them. */
		if (on_key && in_repeat(&a->in, IN_LEFT))
			if (on_key(a, ctx, IN_LEFT, sel) == MENU_DONE)
				return MENU_LEFT_SCREEN;
		if (on_key && in_repeat(&a->in, IN_RIGHT))
			if (on_key(a, ctx, IN_RIGHT, sel) == MENU_DONE)
				return MENU_LEFT_SCREEN;
		if (!a->running) return MENU_LEFT_GONE;

		/* Everything the runner did not consume goes to the screen, so a
		 * screen-specific key needs no runner change to exist. */
		for (b = 0; b < IN_COUNT; b++) {
			if (!a->in.pressed[b]) continue;
			if (b == IN_LEFT || b == IN_RIGHT) continue;
			if (b == IN_UP || b == IN_DOWN || b == IN_BACK || b == IN_MENU
			    || b == IN_POWER || b == IN_VOLUP || b == IN_VOLDN
			    || b == IN_BRIGHTUP || b == IN_BRIGHTDN) continue;
			if (on_key && on_key(a, ctx, (in_button)b, sel) == MENU_DONE)
				return MENU_LEFT_SCREEN;
			if (!a->running) return MENU_LEFT_GONE;
			/* The screen it opened was left with MENU: out of this one too,
			 * without drawing it again first. */
			if (g_menu_closing) return MENU_LEFT_BACK;
			/* A handler may have run a modal and changed everything under
			 * us; rebuild before drawing rather than drawing stale rows. */
			break;
		}

		tick_tint(a);
		if (st->backdrop) {
			st->backdrop(a, ctx);
		} else {
			draw_shelf(a);
			SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
			SDL_SetRenderDrawColor(a->r, 0, 0, 0, 120);
			SDL_RenderFillRect(a->r, NULL);
		}
		menu_draw(a, heading, rows, n, sel, st->fixed_w,
		          st->follow_tint ? a->tint : st->accent);
		draw_chrome(a->r);
		plat_present(a->r);
		SDL_Delay(8);
	}
	return MENU_LEFT_GONE;
}

/* One screen, one loop. Owns polling, power, idle, quit, back, volume,
 * brightness, the cursor and the whole frame, so that no screen can forget any
 * of them - which every menu defect this project has had came down to. See
 * docs/menus.md and ADR-0001.
 *
 * The flush is here rather than in the body: the press that opens a screen must
 * not also close it, and the press that closes one must not arrive again at
 * whatever is underneath. This file does that by hand about twenty times, and
 * the body has nine ways out, so putting it at each of them would eventually
 * mean missing one. */
static menu_exit menu_run(app *a, const menu_style *st,
                          menu_build_fn build, menu_key_fn on_key, void *ctx)
{
	menu_exit how;

	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);

	how = menu_run_body(a, st, build, on_key, ctx);

	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);
	return how;
}

static void wifi_backdrop(void *ctx)
{
	app *a = ctx;
	draw_shelf(a);
	SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(a->r, 0, 0, 0, 150);
	SDL_RenderFillRect(a->r, NULL);
	draw_battery(a->r);   /* the keyboard draws over this and presents itself */
}

static menu_result wifi_key(app *a, void *ctx, in_button key, int sel)
{
	wifi_ui *w = ctx;
	char status[96], ssid[WIFI_SSID_MAX], ip[64];
	int k = sel - WIFI_TOP_ROWS;        /* the fixed rows, then the networks */
	bool net_row = sel >= WIFI_TOP_ROWS && sel < WIFI_TOP_ROWS + w->n;

	if (key == IN_Y && w->on) { wifi_begin_scan(w); return MENU_STAY; }

	/* Forget a saved network. Only saved rows offer it - there is nothing to
	 * forget about one the device has never joined, and a button that silently
	 * does nothing on some rows is the same defect as a menu entry wired to
	 * nothing.
	 *
	 * Forgetting the CONNECTED network is allowed, deliberately. Refusing would
	 * be safer - it drops the device off the LAN, taking Over The Hare and ssh
	 * with it, and a handheld has no keyboard to climb back out with. But
	 * wanting to forget the network you are on is reasonable, and the honest
	 * place to say what it costs is the confirm. */
	if (key == IN_X && w->on && net_row && w->nets[k].known) {
		char cur[WIFI_SSID_MAX], msg[160];
		bool joined = wifi_status(cur, sizeof cur, NULL, 0) == WIFI_CONNECTED
		              && !strcmp(cur, w->nets[k].ssid);

		if (joined)
			snprintf(msg, sizeof msg,
			         "Forget %s? You are connected to it and will lose "
			         "the network.", w->nets[k].ssid);
		else
			snprintf(msg, sizeof msg, "Forget %s?", w->nets[k].ssid);

		if (confirm_panel(a, "Wi-Fi", msg, "Forget")) {
			snprintf(status, sizeof status,
			         wifi_forget(w->nets[k].ssid) ? "Forgot %s"
			                                      : "Could not forget %s",
			         w->nets[k].ssid);
			wait_panel(a, "Wi-Fi", status);
			SDL_Delay(1400);
			if (w->on) wifi_begin_scan(w);
		}
		return MENU_STAY;
	}

	if (key != IN_ACCEPT) return MENU_STAY;

	/* SSH, Samba and Syncthing, where the device has them. No confirm on turning SSH off,
	 * though it may be the only way in: the owner's call (gkd.10). */
	if (sel >= WIFI_ROW_SVC && sel < WIFI_ROW_SVC + WIFI_SVC_ROWS) {
		wifi_svc s = (wifi_svc)(sel - WIFI_ROW_SVC);
		if (wifi_svc_set(s, !w->svc[s])) w->svc[s] = !w->svc[s];
		return MENU_STAY;
	}

	/* The switch. Saved on every change rather than on the way out: the way out
	 * of a handheld is often the power button. */
	/* The two that moved here from the plorpOS menu (plorpos-z0d.1). The
	 * account is read again on the way back, since signing in or out is what
	 * the screen behind it is for. */
	if (sel == WIFI_ROW_CHEEVOS) {
		ra_signin_screen(a);
		w->ra_name = ra_signed_in() ? ra_user() : NULL;
		return MENU_STAY;
	}
	if (sel == WIFI_ROW_XFER) { xfer_screen(a); return MENU_STAY; }

	if (sel == WIFI_ROW_SWITCH) {
		if (w->on) {
			wifi_down();
			wifi_pref_save(false);
			w->on = false;
			w->n = 0;
		} else {
			wait_panel(a, "Wi-Fi", "Turning Wi-Fi on...");
			w->on = wifi_up();
			wifi_pref_save(w->on);
			if (!w->on) {
				wait_panel(a, "Wi-Fi", "Wi-Fi did not come up");
				SDL_Delay(1800);
			}
			if (w->on) wifi_begin_scan(w);
		}
		return MENU_STAY;
	}

	if (net_row) {
		char psk[80] = "";
		bool ok;

		/* A saved network already has its passphrase in wpa_supplicant.conf,
		 * so asking again would be asking the user to retype something the
		 * device is holding. An open network has none to ask for. */
		if (w->nets[k].secured && !w->nets[k].known) {
			kb_result kr = kb_prompt(a->r, &a->in, w->nets[k].ssid,
			                         psk, (int)sizeof psk, MENU_ACCENT,
			                         wifi_backdrop, power_due_ctx, a);
			if (kr == KB_POWER) { power_off(a); return MENU_DONE; }
			if (kr != KB_ACCEPT) return MENU_STAY;
		}

		wait_panel(a, "Wi-Fi", "Connecting...");
		ok = wifi_connect(w->nets[k].ssid, psk[0] ? psk : NULL);
		/* Wiped as soon as it has been handed over. It still exists in the
		 * supplicant's config, which is the point, but there is no reason for
		 * a copy to sit in the launcher's stack afterwards. */
		memset(psk, 0, sizeof psk);

		if (ok) {
			/* Connecting is turning it on, whatever the switch said. */
			wifi_pref_save(true);
			w->on = true;
			wifi_status(ssid, sizeof ssid, ip, sizeof ip);
			snprintf(status, sizeof status, "Connected to %s", ssid);
		} else {
			snprintf(status, sizeof status, "Could not connect to %s",
			         w->nets[k].ssid);
		}
		wait_panel(a, "Wi-Fi", status);
		SDL_Delay(1800);
		/* Out on success, because the job is done and the menu row behind this
		 * screen already names the network - staying in the list makes you back
		 * out by hand to see the result. On failure stay, because the next
		 * thing wanted is another try or another network, and both are here. */
		if (ok) return MENU_DONE;
		wifi_begin_scan(w);
	}
	return MENU_STAY;
}

static void wifi_screen(app *a)
{
	wifi_ui w = { 0 };
	int i;

	for (i = 0; i < WIFI_SVC_ROWS; i++) w.svc[i] = wifi_svc_on((wifi_svc)i);

	/* Entering does not switch the radio on. It used to, which made the screen
	 * impossible to leave in the off state: you opened it to turn wifi OFF and
	 * the act of opening it turned wifi on. */
	w.on = wifi_status(NULL, 0, NULL, 0) != WIFI_OFF;
	w.ra_name = ra_signed_in() ? ra_user() : NULL;
	if (w.on) wifi_begin_scan(&w);

	menu_run(a, &(menu_style){ .accent = MENU_ACCENT,
	                           .fixed_w = menu_std_width(a) },
	         wifi_build, wifi_key, &w);
}


/* ---------- About --------------------------------------------------------- */

#ifndef TORTOS_VERSION
#define TORTOS_VERSION "0.0"       /* set by the makefiles from Makefile's VERSION */
#endif

/* Facts about the machine, which is a different thing from settings. The
 * address in particular has nowhere else to live: the Wi-Fi screen names the
 * network, and short of asking the router there is no way to find out what
 * address the device took. */
/* Signing in to RetroAchievements. Two prompts and a request; the account
 * screen is deliberately not a screen, because there is nothing to look at
 * until there is something to say.
 *
 * The password is used to get a token and then wiped. Nothing stores it -
 * ra.cfg holds the token, which is what every later call uses anyway. */
static void ra_signin_screen(app *a)
{
	char user[RA_USER_MAX] = "", pass[96] = "", err[160] = "";
	menu_row row;
	kb_result kr;
	bool ok;

	snprintf(user, sizeof user, "%s", ra_user());

	if (!net_online()) {
		row = (menu_row){ "Not on a network. Connect Wi-Fi first.", NULL, false };
		wifi_backdrop(a);
		menu_draw(a, "RetroAchievements", &row, 1, -1, 0, MENU_ACCENT);
		draw_chrome(a->r);
		plat_present(a->r);
		SDL_Delay(1600);
		return;
	}

	kr = kb_prompt(a->r, &a->in, "RetroAchievements User", user,
	               (int)sizeof user, MENU_ACCENT, wifi_backdrop, power_due_ctx, a);
	if (kr == KB_POWER) { power_off(a); return; }
	if (kr != KB_ACCEPT || !user[0]) return;

	kr = kb_prompt(a->r, &a->in, "Password", pass,
	               (int)sizeof pass, MENU_ACCENT, wifi_backdrop, power_due_ctx, a);
	if (kr == KB_POWER) { memset(pass, 0, sizeof pass); power_off(a); return; }
	if (kr != KB_ACCEPT || !pass[0]) { memset(pass, 0, sizeof pass); return; }

	wait_panel(a, "RetroAchievements", "Signing in...");
	ok = ra_sign_in(user, pass, err, sizeof err);
	/* Wiped the moment it has been used, the same as the Wi-Fi passphrase.
	 * The token it bought is the thing worth keeping. */
	memset(pass, 0, sizeof pass);

	if (ok) {
		ra_creds_save();
		snprintf(err, sizeof err, "Signed in as %s", ra_user());
	} else if (!err[0]) {
		snprintf(err, sizeof err, "Sign-in failed");
	}

	row = (menu_row){ err, NULL, ok };
	wifi_backdrop(a);
	menu_draw(a, "RetroAchievements", &row, 1, -1, 0, MENU_ACCENT);
	draw_chrome(a->r);
	plat_present(a->r);
	SDL_Delay(1800);
	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);
}

/* The ScreenScraper account. The same shape as the screen above, and the
 * differences are the interesting part.
 *
 * There is nothing to trade the password for. RetroAchievements returns a
 * token and the password is dropped here and never stored; ScreenScraper signs
 * every request with the password itself, so what ss_sign_in keeps is a
 * reusable credential and the only "sign-in" available is asking them whether
 * they answer to it. The password is still wiped from THIS screen's stack the
 * moment it has been handed over, because a buffer that outlives its use is
 * the one thing a screen can get wrong on its own.
 *
 * The third state is the one RetroAchievements does not have: a build made
 * without SS_DEVID cannot sign anyone in, and says so rather than taking a
 * password it has no way to use. See src/ss.h. */
static void ss_signin_screen(app *a)
{
	char user[SS_USER_MAX] = "", pass[SS_PASS_MAX] = "", err[160] = "";
	menu_row row;
	kb_result kr;
	bool ok;

	snprintf(user, sizeof user, "%s", ss_user());

	if (!ss_have_dev() || !net_online()) {
		row = (menu_row){ !ss_have_dev()
		                  ? "This build has no ScreenScraper key."
		                  : "Not on a network. Connect Wi-Fi first.", NULL, false };
		wifi_backdrop(a);
		menu_draw(a, "ScreenScraper", &row, 1, -1, 0, MENU_ACCENT);
		draw_chrome(a->r);
		plat_present(a->r);
		SDL_Delay(1600);
		return;
	}

	kr = kb_prompt(a->r, &a->in, "ScreenScraper User", user,
	               (int)sizeof user, MENU_ACCENT, wifi_backdrop, power_due_ctx, a);
	if (kr == KB_POWER) { power_off(a); return; }
	if (kr != KB_ACCEPT || !user[0]) return;

	kr = kb_prompt(a->r, &a->in, "Password", pass,
	               (int)sizeof pass, MENU_ACCENT, wifi_backdrop, power_due_ctx, a);
	if (kr == KB_POWER) { memset(pass, 0, sizeof pass); power_off(a); return; }
	if (kr != KB_ACCEPT || !pass[0]) { memset(pass, 0, sizeof pass); return; }

	wait_panel(a, "ScreenScraper", "Signing in...");
	ok = ss_sign_in(user, pass, err, sizeof err);
	memset(pass, 0, sizeof pass);

	/* ss_sign_in stores the account itself, because it is the half that knows
	 * whether the reply was real. Nothing to save here. */
	if (ok) snprintf(err, sizeof err, "Signed in as %s", ss_user());
	else if (!err[0]) snprintf(err, sizeof err, "Sign-in failed");

	row = (menu_row){ err, NULL, ok };
	wifi_backdrop(a);
	menu_draw(a, "ScreenScraper", &row, 1, -1, 0, MENU_ACCENT);
	draw_chrome(a->r);
	plat_present(a->r);
	SDL_Delay(1800);
	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);
}

/* Over The Hare: the address, the PIN, and what is happening.
 *
 * The whole screen is a waiting room. It exists so somebody can read two
 * things off a handheld and type them into a laptop, and then watch enough to
 * know it is working. Everything real happens in hare.c and in a browser
 * somewhere else on the network.
 *
 * The server lives exactly as long as this screen. Closing it stops
 * listening, forgets the PIN and drops every session - which is the honest
 * answer to "how long is my ROM folder on the network for". */
/* Bytes, in a unit a person reads rather than the one the counter is in.
 *
 * The shot harness was handed "41 MB out" as a literal while this screen could
 * only ever print kilobytes - a picture of a screen that did not exist, which
 * is the exact failure the shared row builder was meant to stop. Sharing the
 * layout is not enough if the fixture claims the content can be something it
 * cannot.
 *
 * Decimal, matching human() in res/web/app.js. These two describe the same
 * bytes to the same person a foot apart, so they have to divide by the same
 * thing, and decimal is what every file manager but Windows Explorer shows -
 * 1024 under a KB/MB label is the one combination that is wrong everywhere. */
static void human_bytes(char *out, size_t n, unsigned long long b)
{
	if (b < 1000ull)              snprintf(out, n, "%llu B", b);
	else if (b < 1000ull * 1000)  snprintf(out, n, "%llu KB", b / 1000);
	else if (b < 1000ull * 1000 * 1000)
		snprintf(out, n, "%.1f MB", (double)b / (1000 * 1000));
	else snprintf(out, n, "%.1f GB", (double)b / (1000ull * 1000 * 1000));
}

/* The address and the PIN go in the HEADING, not in rows.
 *
 * They were four rows of equal weight, and rendered it was obvious that the
 * two things this screen exists to be read off were in the dimmest color the
 * launcher has: menu_draw paints a value UI_TEXT_DIM unless its row is both
 * live and selected, and nothing on a status screen is either. The heading is
 * UI_F_LABEL at UI_TEXT_SOFT - larger and brighter - and takes two lines when
 * it contains a newline, which is exactly two things worth copying.
 *
 * Everything else is status and belongs below the rule. */
static void hare_head(char *out, size_t n, const char *addr, const char *pin)
{
	snprintf(out, n, "%s\nPIN %s", addr, pin);
}

static int hare_rows(menu_row *out, const char *who, const char *moved,
                     const char *now)
{
	out[0] = (menu_row){ "Browsers", who,   false };
	/* "In / out", not "Transferred": the longest label on the panel beside
	 * "12 KB in / 41 MB out" scrolled on every real session - just browsing
	 * moves bytes both ways, so both numbers are never zero - and a still
	 * caught it mid-word. Rendered 2026-09-30, this fits even at
	 * "1.2 GB / 413.5 MB". Backlog 37. */
	out[1] = (menu_row){ "In / out", moved, false };
	out[2] = (menu_row){ "Now",      now,   false };
	return 3;
}

/* One frame of it, for the shot harness. The rows come from the same
 * function the screen uses, so a picture of this cannot quietly stop being a
 * picture of that. */
void hare_preview(app *a, const char *addr, const char *pin, const char *who,
                  const char *moved, const char *now)
{
	menu_row rows[3];
	char head[192];
	int n = hare_rows(rows, who, moved, now);

	hare_head(head, sizeof head, addr, pin);
	draw_shelf(a);
	SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(a->r, 0, 0, 0, 120);
	SDL_RenderFillRect(a->r, NULL);
	menu_draw(a, head, rows, n, -1, menu_std_width(a), MENU_ACCENT);
}

/* Download logs, from Over The Hare: see logpack.h. The names to hide are
 * gathered here, at the moment of the download, because only the launcher
 * knows them - every saved network rather than just the one it is on, since
 * an older boot's log can name another; every paired headset; both account
 * names. about.txt carries the first questions asked about any report. */
static app *g_logs_app;

static bool pack_logs(char *path, size_t pn, char *name, size_t nn)
{
	wifi_net   nets[WIFI_MAX_NETS];
	bt_device  bt[BT_MAX];
	const char *secrets[WIFI_MAX_NETS + BT_MAX + 4];
	char       now_ssid[WIFI_SSID_MAX] = "", ip[64], logs[CFG_STR * 2];
	char       folder[64], about[4096], err[128], stamp[32];
	int        n = 0, saved, nb, i;
	size_t     at = 0;
	time_t     t = time(NULL);
	struct tm  tm;
	struct statvfs vf;
	double     up = 0;
	FILE      *f;

	saved = wifi_known(nets, WIFI_MAX_NETS);
	for (i = 0; i < saved; i++) secrets[n++] = nets[i].ssid;
	if (wifi_status(now_ssid, sizeof now_ssid, ip, sizeof ip) != WIFI_OFF && now_ssid[0])
		secrets[n++] = now_ssid;
	nb = bt_bonded(bt, BT_MAX);
	for (i = 0; i < nb; i++) secrets[n++] = bt[i].name;
	if (ra_signed_in() && ra_user()) secrets[n++] = ra_user();
	if (ss_signed_in() && ss_user()) secrets[n++] = ss_user();

	localtime_r(&t, &tm);
	strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M", &tm);
	strftime(folder, sizeof folder, "tortos-logs-%Y-%m-%d-%H%M", &tm);
	if ((f = fopen("/proc/uptime", "r"))) { if (fscanf(f, "%lf", &up) != 1) up = 0; fclose(f); }

#define ABOUT(...) do { \
		int k_ = snprintf(about + at, sizeof about - at, __VA_ARGS__); \
		if (k_ > 0 && (size_t)k_ < sizeof about - at) at += (size_t)k_; \
	} while (0)
	ABOUT("TortOS %s\n", TORTOS_VERSION);
	ABOUT("Packed %s, up %.0f minutes\n", stamp, up / 60);
	if (statvfs(P_CARD, &vf) == 0)
		ABOUT("Card: %.1f GB free of %.1f GB\n",
		      (double)vf.f_bavail * vf.f_frsize / 1e9, (double)vf.f_blocks * vf.f_frsize / 1e9);
	ABOUT("Auto Off: %d s\n", g_logs_app ? g_logs_app->auto_off : -1);
	ABOUT("Wi-Fi: %s\n", now_ssid[0] ? now_ssid : "not connected");
	ABOUT("Paired headsets: %d\n", nb);
	ABOUT("\nShelves:\n");
	for (i = 0; g_logs_app && i < g_logs_app->sys.count; i++) {
		const system_cfg *sc = &g_logs_app->sys.systems[i];

		if (is_muse(sc))
			ABOUT("  %-22s %d albums, %d books, %d tracks\n", sc->name,
			      ml_count(&g_muse, false), ml_count(&g_muse, true), g_muse.ntracks);
		else
			ABOUT("  %-22s %d\n", sc->name, g_logs_app->view[i].list.count);
	}
#undef ABOUT

	snprintf(logs, sizeof logs, "%s/logs", P_USERDATA);
	snprintf(name, nn, "%s.tar.gz", folder);
	snprintf(path, pn, "/tmp/%s", name);
	if (!logpack_build(logs, about, secrets, n, folder, path, err, sizeof err)) {
		fprintf(stderr, "logs: could not pack them: %s\n", err);
		return false;
	}
	fprintf(stderr, "logs: packed %s, %d names masked\n", name, n);
	return true;
}

/* A folder about to be deleted whole from Over The Hare: if Muse is playing
 * or paused on a file in it, stop Muse first. The shelf is rebuilt when the
 * screen closes, as for any change there; this is only so the player is not
 * left holding a file that is gone. */
static void muse_before_delete(const char *abs)
{
	const char *p = musec_path();
	char full[LIB_PATH * 2];
	size_t n = strlen(abs);

	if (!p[0]) return;
	snprintf(full, sizeof full, "%s/%s", P_CARD, p);
	if (!strncmp(full, abs, n) && full[n] == '/') {
		fprintf(stderr, "muse: stopped, its folder is being deleted\n");
		musec_stop();
	}
}

static void xfer_screen(app *a)
{
	char       ssid[WIFI_SSID_MAX], ip[64];
	char       addr[96], pinbuf[32], who[64], moved[64], head[192];
	menu_row   rows[3];
	hare_stats st;
	unsigned long long total_in = 0, total_out = 0;
	bool       done = false;

	g_logs_app = a;
	hare_set_logs(pack_logs);
	hare_set_before_delete(muse_before_delete);
	if (!hare_start(P_ROMS, P_CARD, P_SHARED, P_WEB)) {
		menu_row row = { "Could not start", NULL, false };

		draw_shelf(a);
		menu_draw(a, "Over The Hare", &row, 1, -1, 0, MENU_ACCENT);
		draw_chrome(a->r);
		plat_present(a->r);
		SDL_Delay(1800);
		plat_input_flush();
		memset(&a->in, 0, sizeof a->in);
		return;
	}

	snprintf(pinbuf, sizeof pinbuf, "%s", hare_pin());
	addr[0] = '\0';

	while (!done && !want_quit && a->running) {
		static unsigned next_check;
		unsigned now = plat_now_ms();
		int busy;

		/* The launcher's slice of the transfer. Bounded, like everything else
		 * in a frame - see httpd.h. */
		busy = hare_poll();
		hare_status(&st);
		total_in  += st.in;
		total_out += st.out;

		/* Two seconds, because wifi_status forks wpa_cli and this loop runs
		 * every frame. Same cache the About screen uses. */
		if (!addr[0] || now - next_check > 2000u) {
			next_check = now;
			if (wifi_status(ssid, sizeof ssid, ip, sizeof ip) == WIFI_CONNECTED
			    && ip[0]) {
				/* Port 80 needs no colon, and the address is being copied by
				 * hand off a three-inch screen - so it is only shown when it
				 * is not the one everybody assumes. */
				if (hare_port() == 80)
					snprintf(addr, sizeof addr, "%s", ip);
				else
					snprintf(addr, sizeof addr, "%s:%d", ip, hare_port());
			} else {
				snprintf(addr, sizeof addr, "Wi-Fi went away");
			}
		}

		if (st.clients == 0)      snprintf(who, sizeof who, "waiting");
		else if (st.clients == 1) snprintf(who, sizeof who, "1 connected");
		else                      snprintf(who, sizeof who, "%d connected", st.clients);

		if (total_in || total_out) {
			char hin[24], hout[24];

			human_bytes(hin,  sizeof hin,  total_in);
			human_bytes(hout, sizeof hout, total_out);
			snprintf(moved, sizeof moved, "%s / %s", hin, hout);
		} else {
			snprintf(moved, sizeof moved, "nothing yet");
		}

		hare_head(head, sizeof head, addr, pinbuf);
		hare_rows(rows, who, moved, st.last[0] ? st.last : "waiting");

		plat_input_poll(&a->in);
		if (a->in.quit_requested) { hare_stop(); a->running = false; return; }

		/* Auto Off must not shut the device down in the middle of a 900MB
		 * upload, and it also must not shut it down while somebody is reading
		 * a listing on the other side of the room. Bytes moving is obvious
		 * activity; a connected browser is a person at the other end of it,
		 * which is the same claim a button press makes and no weaker for
		 * arriving over a network. Neither suspends Auto Off - close the tab
		 * and the countdown resumes. */
		if (busy || st.clients > 0) a->idle.since_ms = now;

		{
			pwr_action pa = power_check(a);
			if (pa == PWR_POWEROFF) {
				hare_stop();
				power_off(a);
				return;
			}
		}
		if (menu_leaving(a)) done = true;

		if (in_repeat(&a->in, IN_VOLUP))    plat_volume_nudge(+1);
		if (in_repeat(&a->in, IN_VOLDN))    plat_volume_nudge(-1);
		if (in_repeat(&a->in, IN_BRIGHTUP)) plat_brightness_nudge(+1);
		if (in_repeat(&a->in, IN_BRIGHTDN)) plat_brightness_nudge(-1);

		tick_tint(a);
		draw_shelf(a);
		SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(a->r, 0, 0, 0, 120);
		SDL_RenderFillRect(a->r, NULL);
		menu_draw(a, head, rows, 3, -1, menu_std_width(a), MENU_ACCENT);
		draw_chrome(a->r);
		plat_present(a->r);
		/* Shorter than the usual 8ms: this loop is also the server's, and a
		 * transfer moves POLL_BUDGET per pass. */
		SDL_Delay(4);
	}

	/* Asked before hare_stop, which is what the screen owning the server
	 * means: after it, there is nobody left to ask. */
	{
		bool changed = hare_shelf_changed();

		hare_stop();
		/* The point of the whole feature is getting ROMs onto the card, and
		 * the shelf is scanned once at startup - so without this the file
		 * lands, the screen says it landed, and the game is not there. The
		 * only way to see it was to restart the launcher.
		 *
		 * On the way out rather than as each upload finishes: a transfer of
		 * twenty games would otherwise rescan twenty times, and every one of
		 * them would stall the loop that is still receiving. */
		if (changed) {
			wait_panel(a, "Over The Hare", "Scanning...");
			rescan_all(a);
		}
	}
	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);
}

/* Box art, from libretro. See artscrape.h for the matching rule and why it is
 * that one.
 *
 * Starts as soon as the screen opens rather than asking again. The player has
 * already pressed A on a row called Box Art; a confirmation would be a second
 * question with the same answer. It is safe to start because it is safe to
 * stop: art already on the card costs no request, so B is free and so is
 * running the whole thing again tomorrow.
 *
 * Deliberately NOT automatic. It is a network request on someone's behalf,
 * and a launcher that quietly fetches two hundred images the first time a
 * card is inserted has made that decision for them. */
/* The heading and rows, in one place so a shot cannot picture a screen that
 * does not exist - the lesson from --hare, where the fixture claimed a byte
 * count the code could not produce. */
static void art_head(char *out, size_t n, const char *now)
{
	char fit[128];

	/* Trimmed to fit. A ROM name is as long as somebody's dump of it -
	 * "Legend of Zelda, The - A Link to the Past (USA)" and worse - and this
	 * is a heading, which menu_draw centers rather than wraps, so a long one
	 * ran off both sides of the panel. Three quarters of the screen leaves
	 * the panel a margin it can keep. */
	ui_fit_text(ui_font(UI_F_LABEL), now, fit, sizeof fit,
	            TORTOS_SCREEN_W * 3 / 4);
	snprintf(out, n, "Box Art\n%s", fit);
}

static int art_rows(menu_row *out, const char *where, const char *counts,
                    bool working)
{
	out[0] = (menu_row){ "Systems", where,  false };
	out[1] = (menu_row){ "Art",     counts, false };
	out[2] = (menu_row){ working ? "B to stop" : "B to close", NULL, false };
	return 3;
}

void art_preview(app *a, const char *now, const char *where,
                 const char *counts, bool working)
{
	menu_row rows[3];
	char head[192];
	int n = art_rows(rows, where, counts, working);

	art_head(head, sizeof head, now);
	draw_shelf(a);
	SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(a->r, 0, 0, 0, 120);
	SDL_RenderFillRect(a->r, NULL);
	menu_draw(a, head, rows, n, -1, menu_std_width(a), MENU_ACCENT);
}

/* game_info, gi_row and gi_rows: src/game_menu.h */

static void gi_gather(app *a, int owner, const game_entry *g, game_info *gi)
{
	const system_cfg *s = &a->sys.systems[owner];
	char p[LIB_PATH * 3];
	struct stat st;

	memset(gi, 0, sizeof *gi);
	gi->deletable = !is_splore(s, g->file);

	/* The save slots are not read here any more. The row that showed them went
	 * on 2026-09-17 - the carousel shows them when you load, with the frames
	 * rather than a count - and with it went a stat of every slot plus the
	 * autosave each time this screen opened. */
	box_art_path(s->folder, g->name, p, sizeof p);
	gi->has_art = (stat(p, &st) == 0 && st.st_size > 0);

	/* Loading the set clobbers whatever set is loaded, which is nobody's
	 * during shelf browsing - no game is running. It stays loaded after this
	 * returns, which is what lets the Cheevos row open the list: the same set
	 * this count was taken from is the one that screen reads. */
	chv_path(P_ROMS, s->folder, g->name, p, sizeof p);
	gi->has_cheevos = chv_load(p) && chv_count() > 0;
	if (gi->has_cheevos)
		snprintf(gi->cheevos, sizeof gi->cheevos, "%d/%d, %d/%d points",
		         chv_earned(), chv_count(),
		         chv_points_earned(), chv_points_total());
	else
		snprintf(gi->cheevos, sizeof gi->cheevos, "none");

	/* What a scrape left on the card, if anything. Read here with the rest
	 * rather than when the synopsis card opens: this is one indexed row, the
	 * screen already reads the save directory and the achievement set, and a
	 * row that appears a frame after the panel does reads as a glitch. */
	{
		game_meta m;

		if (db_game_get(db_lib(), s->folder, g->file, &m)) {
			gi->scraped = true;
			gi->has_synopsis = m.synopsis[0] != '\0';
			snprintf(gi->year, sizeof gi->year, "%.4s", m.year);
			/* The scrape's own English names, in its own order - but
			 * joined with a comma AND A SPACE, which it does not do. That is
			 * typesetting rather than an opinion about somebody else's
			 * taxonomy: "Platform,Shoot'em Up" reads as one word with a
			 * stumble in it. One in three games has a second genre. */
			{
				const char *g = m.genres;
				size_t k = 0;

				while (*g && k + 2 < sizeof gi->genre) {
					gi->genre[k++] = *g;
					if (*g++ == ',' && *g != ' ') gi->genre[k++] = ' ';
				}
				gi->genre[k] = '\0';
			}
		}
	}
}

/* FBNeo's set table on the card, for the shelf titles and the art names. */
static const char *fbneo_dat(void)
{
	static char p[CFG_STR * 2];

	if (!p[0]) snprintf(p, sizeof p, "%s/res/fbneo-titles.tsv", P_ROOT);
	return p;
}

/* `only` names one system's folder, or NULL for the whole library.
 *
 * One system is worth having for the FIRST run on a card, where it is the
 * difference between twenty images and a hundred and eighty. It is worth much
 * less afterwards: since the direct-name pass landed, a whole-library re-run
 * over a full card is nine directory sweeps and no network at all. */
static void art_screen(app *a, const char *only, const char *one,
                       const char *stem, const char *one_file, unsigned accent)
{
	menu_row     rows[3];
	art_progress p;
	char         counts[64], where[64], head[192];
	int          nrows = 3;
	bool         done = false, working = true;
	/* Whether the one game already had a cover, so a miss can say it kept it
	 * rather than read as though the cover went. */
	bool         had = one && stem && only && has_box_art(only, stem);

	if (!net_online()) {
		menu_row row = { "No network", NULL, false };

		draw_shelf(a);
		menu_draw(a, "Box Art", &row, 1, -1, 0, accent);
		draw_chrome(a->r);
		plat_present(a->r);
		SDL_Delay(1600);
		plat_input_flush();
		memset(&a->in, 0, sizeof a->in);
		return;
	}

	/* The scraper needs the one async slot, and an account answer from a game
	 * just quit may still be holding it. */
	sync_abandon();

	/* SCREENSCRAPER FIRST, FOR ONE GAME. BACKLOG 27 settled the order: them
	 * when the player has an account, libretro when they do not and libretro
	 * again when they cannot answer - a miss, a refusal, or a name that does
	 * not survive the check.
	 *
	 * Only the single-game path for now. The bulk run is the same idea over
	 * 1,708 games and a different conversation about time: measured at 5.5
	 * seconds a game it is an overnight job, where libretro fetches one index
	 * per system and then only what is missing.
	 *
	 * It brings the text with it. A cover fetched without the year and the
	 * synopsis would mean asking again later for what was in hand now. */
	if (one && stem && only && ss_signed_in()) {
		const system_cfg *sys = NULL;
		char dir[LIB_PATH * 2];
		int i;

		for (i = 0; i < a->sys.count; i++)
			if (!strcmp(a->sys.systems[i].folder, only)) { sys = &a->sys.systems[i]; break; }
		snprintf(dir, sizeof dir, "%s/%s", P_ROMS, only);
		if (sys && ss_run_begin(only, one_file, stem, dir, sys->exts)) {
			bool ss_working = true;

			while (ss_working && !want_quit && a->running) {
				int st = ss_run_step();

				plat_input_poll(&a->in);
				if (a->in.quit_requested) { ss_run_cancel(); a->running = false; return; }
				{
					pwr_action pa = power_check(a);
					if (pa == PWR_POWEROFF) {
						ss_run_cancel();
						power_off(a);
						return;
					}
				}
				if (menu_leaving(a)) {
					ss_run_cancel();
					plat_input_flush();
					memset(&a->in, 0, sizeof a->in);
					return;
				}
				if (in_repeat(&a->in, IN_VOLUP))    plat_volume_nudge(+1);
				if (in_repeat(&a->in, IN_VOLDN))    plat_volume_nudge(-1);
				if (in_repeat(&a->in, IN_BRIGHTUP)) plat_brightness_nudge(+1);
				if (in_repeat(&a->in, IN_BRIGHTDN)) plat_brightness_nudge(-1);

				if (st == 0) {
					/* Got it. Say so on the same panel the libretro run
					 * uses, so the two sources look like one feature. */
					menu_row row[2];

					art_head(head, sizeof head, one);
					row[0] = (menu_row){ "Box Art",
					                     had ? "replaced" : "found", false };
					row[1] = (menu_row){ "B to close", NULL, false };
					tick_tint(a);
					draw_shelf(a);
					SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
					SDL_SetRenderDrawColor(a->r, 0, 0, 0, 120);
					SDL_RenderFillRect(a->r, NULL);
					menu_draw(a, head, row, 2, -1, menu_std_width(a), accent);
					draw_chrome(a->r);
					plat_present(a->r);
					SDL_Delay(900);
					ss_run_cancel();
					free_all_textures(a);
					plat_input_flush();
					memset(&a->in, 0, sizeof a->in);
					return;
				}
				if (st < 0) { ss_run_cancel(); ss_working = false; break; }

				art_head(head, sizeof head, one);
				rows[0] = (menu_row){ "Box Art", ss_run_where(), false };
				rows[1] = (menu_row){ "B to stop", NULL, false };
				tick_tint(a);
				draw_shelf(a);
				SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
				SDL_SetRenderDrawColor(a->r, 0, 0, 0, 120);
				SDL_RenderFillRect(a->r, NULL);
				menu_draw(a, head, rows, 2, -1, menu_std_width(a), accent);
				draw_chrome(a->r);
				plat_present(a->r);
				SDL_Delay(8);
			}
			if (!a->running || want_quit) return;
		}
	}

	if (only) {
		/* A one-entry library rather than a filter inside artscrape.c. The
		 * scraper already takes the list it should work on; handing it a
		 * shorter list is the same operation, and it keeps "which systems"
		 * a question the caller answers. */
		static systems_cfg one;
		int i;

		one.count = 0;
		for (i = 0; i < a->sys.count; i++)
			if (!strcmp(a->sys.systems[i].folder, only)) {
				one.systems[0] = a->sys.systems[i];
				one.count = 1;
				break;
			}
		art_begin(&one, P_ROMS, fbneo_dat(), stem);
	} else {
		art_begin(&a->sys, P_ROMS, fbneo_dat(), NULL);
	}

	while (!done && !want_quit && a->running) {
		/* One step per frame. A step is one request STARTED or one poll of
		 * the request already running - never a wait for one, because this
		 * loop is where the power button is read. */
		if (working && art_step() == 0) working = false;
		art_status(&p);

		snprintf(counts, sizeof counts, "%d found, %d missing, %d already",
		         p.found, p.missing, p.skipped);
		snprintf(where, sizeof where, "%d of %d", p.systems_done, p.systems);
		if (one) {
			/* Asked about one game, answer about one game: the run is that
			 * game alone (art_begin's `only`), and "1 found, 19 already" would
			 * be a true statement about work the player did not ask for. */
			art_head(head, sizeof head, one);
			rows[0] = (menu_row){ "Box Art",
			                      working ? "fetching"
			                      : p.found ? (had ? "replaced" : "found")
			                      : had ? "not found, kept the old one"
			                            : "not found", false };
			rows[1] = (menu_row){ working ? "B to stop" : "B to close",
			                      NULL, false };
			nrows = 2;
		} else {
			/* Running: what it is doing. Finished: what went wrong, or
			 * that it is done - never the last game it happened to fetch,
			 * which is what this said before and reads as still working
			 * on it. */
			art_head(head, sizeof head,
			         working ? (p.now[0] ? p.now : "starting")
			         : p.problem[0] ? p.problem : "Done");
			nrows = art_rows(rows, where, counts, working);
		}

		plat_input_poll(&a->in);
		if (a->in.quit_requested) { art_cancel(); a->running = false; return; }

		/* A fetch in flight is not somebody in the room, so it does not hold
		 * the idle clock the way a connected browser does in Over The Hare -
		 * there, a person was at the other end. Here the device is talking to
		 * itself, and a library left scraping unattended on battery is
		 * exactly what Auto Off is for. */
		{
			pwr_action pa = power_check(a);
			if (pa == PWR_POWEROFF) {
				art_cancel();
				power_off(a);
				return;
			}
		}
		if (menu_leaving(a)) {
			art_cancel();
			done = true;
		}

		if (in_repeat(&a->in, IN_VOLUP))    plat_volume_nudge(+1);
		if (in_repeat(&a->in, IN_VOLDN))    plat_volume_nudge(-1);
		if (in_repeat(&a->in, IN_BRIGHTUP)) plat_brightness_nudge(+1);
		if (in_repeat(&a->in, IN_BRIGHTDN)) plat_brightness_nudge(-1);

		tick_tint(a);
		draw_shelf(a);
		SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(a->r, 0, 0, 0, 120);
		SDL_RenderFillRect(a->r, NULL);
		menu_draw(a, head, rows, nrows, -1, menu_std_width(a), accent);
		draw_chrome(a->r);
		plat_present(a->r);
		SDL_Delay(8);
	}

	art_cancel();
	/* The cards on the shelf are textures already uploaded from whatever art
	 * existed when the shelf was built. New art on the card changes nothing
	 * until they are dropped and read again. */
	free_all_textures(a);
	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);
}

/* One game, and the two things you can do to it from here.
 *
 * X on the shelf, because Y is already favorite and the pair reads as a
 * unit - look at this one, or mark it. Nothing here is reachable during a
 * game: the in-game menu is about the session, this is about the file.
 *
 * A still, not a live view. Everything is read when the screen opens and
 * again after an action changes something, rather than every frame - these
 * are numbers that only move when the player moves them, and the alternative
 * is a stat storm at 120 Hz for a panel nobody is interacting with. */
/* The info screen's context. owner, g and v all move when a favorite is
 * toggled, so they live here where the key handler can put them back. */
typedef struct {
	app        *a;
	sysview    *v;
	game_entry *g;
	int         owner;
	game_info   gi;
	menu_style  st;   /* the accent follows the OWNING system, which can move */
} info_ctx;

static int info_build(void *ctx, menu_row *rows, int max, const char **heading)
{
	info_ctx *c = ctx;
	int n;

	(void)max;
	*heading = c->g->title;
	/* menu_wifi, not wifi_status. This loop runs every frame and wifi_status
	 * forks wpa_cli out of a 119 MB process; this screen was doing it about
	 * 125 times a second for a row that says "needs Wi-Fi". The system menu
	 * was measured at 12 fps for the same mistake. */
	n = gi_rows(rows, &c->gi, menu_wifi(NULL, 0) == WIFI_CONNECTED);
	return n > max ? max : n;
}

/* Defined with the achievement card, which is where wrap_text lives and which
 * this is the same gesture as: a row that opens into something to read. */
static void synopsis_screen(app *a, const char *title, const char *text,
                            unsigned accent);

/* The achievement list, which the in-game menu opens over a paused frame and
 * this screen opens over the shelf. `over_shelf` says which, rather than the
 * absence of a frame saying it: in-game, a capture that failed also leaves no
 * frame, and drawing the shelf under a paused game would be a lie about where
 * the player is. */
static void cheevos_screen(app *a, SDL_Texture *bg, bool over_shelf);
static void hotkeys_screen(app *a, SDL_Texture *bg, const char *tag);

/* Delete Game, from the info screen (plorpos-gkd.69): the game's file, or a
 * disc game's whole folder - the folder is the game - and its favorite.
 * Nothing else: saves, states, art and play time stay, so a game put back
 * picks up where it was (user, 2026-10-02). Then the card is read again, and
 * the cursor stays where the game was, on what is now the next one. */
static int rm_one(const char *p, const struct stat *st, int flag, struct FTW *f)
{
	(void)st; (void)flag; (void)f;
	return remove(p);
}

static void game_delete(app *a, int owner, const game_entry *g)
{
	const system_cfg *s = &a->sys.systems[owner];
	const char *slash = strchr(g->file, '/');
	int at = a->view[a->sys_cursor].cursor;
	char path[LIB_PATH * 3];
	sysview *v;
	bool ok;

	if (slash)
		snprintf(path, sizeof path, "%s/%s/%.*s", P_ROMS, s->folder,
		         (int)(slash - g->file), g->file);
	else
		snprintf(path, sizeof path, "%s/%s/%s", P_ROMS, s->folder, g->file);
	wait_panel(a, "Delete Game", "Deleting...");
	ok = slash ? nftw(path, rm_one, 8, FTW_DEPTH | FTW_PHYS) == 0
	           : remove(path) == 0;
	fprintf(stderr, "delete: %s: %s\n", path, ok ? "gone" : strerror(errno));
	/* Before the rescan: g points into the list it frees. */
	if (ok && fav_is(s->tag, g->file)) {
		fav_toggle(s->tag, g->file);
		fav_save();
	}
	/* Even when it failed: a folder can be half gone. */
	rescan_all(a);
	v = &a->view[a->sys_cursor];
	if (a->screen == SCREEN_GAMES && v->list.count) {
		v->cursor = at < v->list.count ? at : v->list.count - 1;
		cf_reset(&v->cf, v->cursor);
	}
}

/* Rename, from the info screen (plorpos-gkd.86.4): the player's name for the
 * game, kept in the games table apart from any imported title (db.h), so a
 * scrape or a gamelist imported again never undoes it. Then the card is read
 * again, as after Delete - every shelf the game is on, Favorites too, shows
 * and sorts by the new name - and the cursor follows the game to wherever its
 * name put it. Empty gives it back the name it had. */
static void game_rename(app *a, int owner, const game_entry *g, const char *name)
{
	char tag[sizeof a->sys.systems[0].tag], file[sizeof g->file];
	Uint32 t0 = SDL_GetTicks();
	sysview *v;
	bool ok;
	int i;

	snprintf(tag, sizeof tag, "%s", a->sys.systems[owner].tag);
	snprintf(file, sizeof file, "%s", g->file);   /* g goes with the rescan */
	ok = db_game_rename(db_lib(), a->sys.systems[owner].folder, file, name);
	rescan_all(a);
	fprintf(stderr, "rename: %s/%s -> %s%s (%u ms with the rescan)\n", tag, file,
	        name[0] ? name : "(its own name)", ok ? "" : ": NOT SAVED",
	        (unsigned)(SDL_GetTicks() - t0));
	v = &a->view[a->sys_cursor];
	for (i = 0; i < v->list.count; i++) {
		if (strcmp(v->list.items[i].file, file)) continue;
		if (strcmp(a->sys.systems[shelf_owner(a, a->sys_cursor, i)].tag, tag)) continue;
		v->cursor = i;
		cf_reset(&v->cf, v->cursor);
		break;
	}
}

static menu_result info_key(app *a, void *ctx, in_button key, int sel)
{
	info_ctx *c = ctx;
	menu_row  rows[GI_MAX];
	const char *heading;
	int n = info_build(c, rows, GI_MAX, &heading);

	/* X closes it, the same button that opened it. */
	if (key == IN_X) return MENU_DONE;
	if (key != IN_ACCEPT || sel >= n || !rows[sel].live) return MENU_STAY;

	if (!strcmp(rows[sel].label, "Synopsis")) {
		game_meta m;

		/* Read again here rather than carried in game_info: it is up to 4 KB
		 * of prose for a row that is usually not opened, and the gatherer runs
		 * on every action this screen takes. */
		if (db_game_get(db_lib(), a->sys.systems[c->owner].folder, c->g->file, &m)
		    && m.synopsis[0])
			synopsis_screen(a, c->g->title, m.synopsis, c->st.accent);
		return MENU_STAY;
	}

	if (!strcmp(rows[sel].label, "Rename")) {
		char name[sizeof c->g->title], *p;
		size_t len;
		kb_result kr;

		snprintf(name, sizeof name, "%s", c->g->title);
		kr = kb_prompt(a->r, &a->in, "Rename", name, (int)sizeof name,
		               c->st.accent, wifi_backdrop, power_due_ctx, a);
		plat_input_flush();
		memset(&a->in, 0, sizeof a->in);
		if (kr == KB_POWER) { power_off(a); return MENU_DONE; }
		if (kr != KB_ACCEPT) return MENU_STAY;
		p = name + strspn(name, " ");
		while ((len = strlen(p)) && p[len - 1] == ' ') p[len - 1] = '\0';
		if (!strcmp(p, c->g->title)) return MENU_STAY;
		/* Closes the screen, as Delete does: the rescan moves the game. */
		game_rename(a, c->owner, c->g, p);
		return MENU_DONE;
	}

	if (!strcmp(rows[sel].label, "Delete Game")) {
		/* Closes the screen when it went: there is no game left to show. */
		if (confirm_panel(a, "Delete Game", c->g->title, "Delete")) {
			game_delete(a, c->owner, c->g);
			return MENU_DONE;
		}
		plat_input_flush();
		memset(&a->in, 0, sizeof a->in);
		return MENU_STAY;
	}

	if (!strcmp(rows[sel].label, "Cheevos")) {
		/* The set the gatherer loaded to count this row is still the loaded
		 * one, so the list has what it needs without reloading it. Over the
		 * shelf rather than a paused frame: no game is running here. */
		cheevos_screen(a, NULL, true);
		return MENU_STAY;
	}

	{
		/* Replace, or get: the ordinary scrape, run over this one game and
		 * nothing else. Nothing is deleted first. It used to be - remove, then
		 * fill in - and a game libretro has no art for lost the cover it had,
		 * which for a translation, homebrew or a hand-added cover is one the
		 * scraper can never bring back. The new one is renamed over the old
		 * only once it has arrived whole. */
		art_screen(a, a->sys.systems[c->owner].folder, c->g->title, c->g->name,
		           c->g->file, a->sys.systems[c->owner].accent);
	}
	gi_gather(a, c->owner, c->g, &c->gi);
	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);
	return MENU_STAY;
}

static void game_info_screen(app *a)
{
	info_ctx c = { 0 };

	c.a = a;
	c.v = &a->view[a->sys_cursor];
	if (c.v->list.count == 0) return;
	c.owner = shelf_owner(a, a->sys_cursor, c.v->cursor);
	c.g = &c.v->list.items[c.v->cursor];
	gi_gather(a, c.owner, c.g, &c.gi);
	c.st.accent = a->sys.systems[c.owner].accent;
	/* Without this the panel sized itself to its own content, and its heading
	 * is a GAME TITLE - so it drew 976px wide beside every other menu's 800.
	 * info_preview next door was given the shared width and this was not,
	 * which is the same one-of-two-paths miss as every other bug of the day. */
	c.st.fixed_w = menu_std_width(a);

	menu_run(a, &c.st, info_build, info_key, &c);

	/* The card may now hold art it did not before. */
	free_all_textures(a);
}

/* One frame of it, from the real gatherer where possible: the fixture is the
 * game actually under the cursor, so a shot is a picture of this library
 * rather than of numbers somebody typed. */
void info_preview(app *a, bool net)
{
	sysview   *v = &a->view[a->sys_cursor];
	menu_row   rows[GI_MAX];
	game_info  gi;
	int        owner, n, sel;

	if (v->list.count == 0) return;
	owner = shelf_owner(a, a->sys_cursor, v->cursor);
	gi_gather(a, owner, &v->list.items[v->cursor], &gi);
	n = gi_rows(rows, &gi, net);
	/* Where the screen actually opens, asked the way menu_run_body asks it
	 * rather than hardcoded. It was a literal 5, which was the Box Art action
	 * until the rows were reordered and then was Saves - so the shot showed a
	 * highlight on a dead row, which the real screen cannot do. */
	sel = rows[0].live ? 0 : menu_step_sel(rows, n, 0, +1);

	draw_shelf(a);
	SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(a->r, 0, 0, 0, 120);
	SDL_RenderFillRect(a->r, NULL);
	menu_draw(a, v->list.items[v->cursor].title, rows, n, sel, menu_std_width(a),
	          a->sys.systems[owner].accent);
}

/* Bluetooth: pair a headset, connect it, forget it.
 *
 * The three things this stack punishes you for getting wrong are all handled
 * in bt.c rather than here - the agent, judging by `info` instead of by the
 * return of `connect`, and where the bonds actually live. What is left here is
 * a list and four buttons.
 *
 * The list is refreshed on an INTERVAL, never per frame: every query is a fork
 * out of a 119 MB process, and this loop runs every frame. Same two seconds
 * the Wi-Fi row uses, for the same reason.
 */
#define BT_VISIBLE 7
#define BT_SCAN_S  20

/* After connecting `mac` from the Bluetooth screen: wait - five seconds at
 * most - until launch.sh has published it and the sound has reached it, so
 * the headset about to be bumped is not pulled out from under the song. Doing
 * that first sent every switch through the speaker: the old headset went,
 * Muse's write failed, it fell back, and only then was it moved - seen
 * 2026-09-26. Routing keeps running while it waits, which is what moves it. */
static void bt_await_route(const char *mac)
{
	char want[32];
	unsigned until = plat_now_ms() + 5000;
	int i;

	snprintf(want, sizeof want, "bt_%s", mac);
	for (i = 3; want[i]; i++) if (want[i] == ':') want[i] = '_';
	while ((int)(until - plat_now_ms()) > 0) {
		muse_poll();
		aout_apply(false);
#if defined(PLATFORM_GKD)
		if (plat_bt_audio()) return;        /* PipeWire already moved it */
#else
		if (!strcasecmp(aout_bt_sink(), want) &&
		    (!musec_heard() || !strcasecmp(musec_sink_now(), want)))
			return;
#endif
		SDL_Delay(50);
	}
}

static void bt_screen(app *a)
{
	bt_ui u = { 0 };
	menu_row rows[BT_VISIBLE + 4];
	unsigned next_refresh = 0, scan_until = 0, note_until = 0;
	int sel = 0;                  /* ROW space: 0 is the toggle, then devices */
	bool done = false;

	while (!done && !want_quit && a->running) {
		unsigned now = plat_now_ms();
		int nrows, i;

		if (next_refresh == 0 || now >= next_refresh) {
			u.state = bt_status();
			u.n = u.state == BT_READY ? bt_visible(u.dev, BT_MAX) : 0;
			for (i = 0; i < u.n; i++)
				bt_label(&u.dev[i], u.vals[i], sizeof u.vals[0]);
			next_refresh = now + 2000;
		}
		if (scan_until && now >= scan_until) scan_until = 0;
		/* A message says what just happened; it is not the state of anything,
		 * so it goes away on its own. "Forgotten" used to sit there until the
		 * screen was left and re-entered, which made it read as a condition
		 * rather than an event. */
		if (note_until && now >= note_until) { note_until = 0; u.note[0] = '\0'; }
		u.scanning = scan_until != 0;

		if (sel > u.n) sel = u.n;
		if (sel < 0) sel = 0;
		u.cursor = sel == 0 ? -1 : u.top + sel - 1;
		if (u.cursor >= u.n) u.cursor = u.n - 1;
		nrows = bt_menu_build(&u, rows, (int)(sizeof rows / sizeof rows[0]),
		                      BT_VISIBLE);

		plat_input_poll(&a->in);
		muse_poll();                        /* see menu_run_body */
		aout_apply(false);
		if (a->in.quit_requested) { a->running = false; return; }
		{
			pwr_action pa = power_check(a);
			if (pa == PWR_POWEROFF) { power_off(a); return; }
		}
		if (menu_leaving(a)) done = true;
		if (a->in.pressed[IN_SELECT]) muse_open(a, NULL, NULL);

		if (in_repeat(&a->in, IN_DOWN) && sel < u.n) sel++;
		if (in_repeat(&a->in, IN_UP)   && sel > 0)   sel--;
		/* The window follows the cursor, so a long list scrolls a row at a
		 * time from either end. */
		if (u.cursor >= 0) {
			if (u.cursor < u.top) u.top = u.cursor;
			if (u.cursor >= u.top + BT_VISIBLE) u.top = u.cursor - BT_VISIBLE + 1;
		}

		if (a->in.pressed[IN_Y] && u.state == BT_READY) {
			/* TWENTY seconds, not ten. Classic inquiry runs in cycles of
			 * about 10.24 s and a device answers probabilistically within
			 * one, so a single cycle misses devices that are plainly there:
			 * measured 2026-09-06 with an OpenRun Pro in pairing mode, ten
			 * seconds found nothing twice and twenty found it first try.
			 *
			 * It costs nothing to wait. The list refreshes every two seconds
			 * while the scan runs, so results appear as they arrive rather
			 * than at the end - a longer window only means it keeps
			 * filling. */
			bt_scan(BT_SCAN_S);
			scan_until = now + BT_SCAN_S * 1000;
			next_refresh = now + 1500;
			u.note[0] = '\0';
			note_until = 0;
		}

		if (a->in.pressed[IN_ACCEPT]) {
			char err[64];

			if (u.cursor < 0) {
				bool want = u.state != BT_READY;

				/* The preference either way: launch.sh reads it at boot and
				 * that is what starts or stops the stack. */
				db_set_int(db_dev(), "bluetooth", want ? 1 : 0);
				db_write_boot_env();

				if (u.state == BT_NO_ADAPTER) {
					/* Nothing to power. hciattach, bluetoothd and bluealsa
					 * are launch.sh's to start, and on a card that booted
					 * with Bluetooth off there is no adapter for
					 * bluetoothctl to talk to - it waits for one that is
					 * never coming rather than failing. Saying so beats
					 * spawning a call that cannot succeed. */
					snprintf(u.note, sizeof u.note,
					         "Saved. Bluetooth starts at the next boot");
					note_until = now + 4000;
				} else {
					wait_panel(a, "Bluetooth",
					           want ? "Turning on..." : "Turning off...");
					bt_power(want);
					snprintf(u.note, sizeof u.note, "%s", want ? "On" : "Off");
					note_until = now + 4000;
				}
				next_refresh = 0;
			} else if (u.cursor < u.n) {
				bt_device *d = &u.dev[u.cursor];

				if (d->connected) {
					wait_panel(a, "Bluetooth", "Disconnecting...");
					if (bt_disconnect(d->mac)) d->connected = false;
					snprintf(u.note, sizeof u.note, "Disconnected"); note_until = now + 4000;
				} else {
					bool ok = true;

					if (!d->bonded) {
						wait_panel(a, "Bluetooth", "Pairing...");
						ok = bt_pair(d->mac, err, sizeof err);
						if (!ok) {
							snprintf(u.note, sizeof u.note, "%s", err);
							note_until = now + 4000;
						} else {
							/* A PCM for the new bond. The emulator and
							 * Muse already running see it at their next
							 * open - see bt.h. */
							bt_asoundrc(P_ROOT, P_USERDATA);
						}
					}
					if (ok) {
						wait_panel(a, "Bluetooth", "Connecting...");
						if (bt_connect(d->mac, err, sizeof err)) {
							/* Connecting one here is choosing it, so any other
							 * goes back to paired: "connected" on this screen then
							 * means "where the sound is", and switching back is one
							 * press rather than a disconnect and a connect. Eric's
							 * call, 2026-09-26. Only from here: a headset that
							 * connects by itself does not bump another, or two that
							 * both do could knock each other off forever. Neither
							 * headset reconnected by itself within a minute of the
							 * Brick disconnecting it, so paired stays paired. */
							int bumped;

							bt_await_route(d->mac);
							bumped = bt_disconnect_others(d->mac);

							d->connected = true;
							snprintf(u.note, sizeof u.note, "%s",
							         bumped ? "Connected. The other headset is paired now"
							                : "Connected");
							note_until = now + 4000;
						} else {
							snprintf(u.note, sizeof u.note, "%s", err); note_until = now + 4000;
						}
					}
				}
				next_refresh = 0;
			}
		}

		if (a->in.pressed[IN_X] && u.cursor >= 0 && u.cursor < u.n &&
		    u.dev[u.cursor].bonded) {
			wait_panel(a, "Bluetooth", "Forgetting...");
			if (bt_forget(u.dev[u.cursor].mac)) {
				bt_asoundrc(P_ROOT, P_USERDATA);
				snprintf(u.note, sizeof u.note, "Forgotten"); note_until = now + 4000;
			} else {
				snprintf(u.note, sizeof u.note, "It would not unpair"); note_until = now + 4000;
			}
			next_refresh = 0;
		}

		if (in_repeat(&a->in, IN_VOLUP))    plat_volume_nudge(+1);
		if (in_repeat(&a->in, IN_VOLDN))    plat_volume_nudge(-1);
		if (in_repeat(&a->in, IN_BRIGHTUP)) plat_brightness_nudge(+1);
		if (in_repeat(&a->in, IN_BRIGHTDN)) plat_brightness_nudge(-1);

		tick_tint(a);
		draw_shelf(a);
		SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(a->r, 0, 0, 0, 120);
		SDL_RenderFillRect(a->r, NULL);
		menu_draw(a, "Bluetooth", rows, nrows, sel, menu_std_width(a), MENU_ACCENT);
		draw_chrome(a->r);
		plat_present(a->r);
		SDL_Delay(8);
	}

	/* On the way out, not while the list is up: `bluetoothctl devices` reads
	 * the same cache, so sweeping mid-screen would empty the list the player
	 * is looking at. */
	bt_sweep_cache(NULL);
}

/* Play time, most played first.
 *
 * Summarized once on entry rather than every frame: it is a fold over every
 * session ever recorded, which measured 6.3 ms cold on the card - nothing at
 * all once, and a waste sixty times a second. Nothing here writes.
 */
/* The most the list can ever be. The number actually shown is asked of
 * menu_list_fit at entry, because the answer moves with Text Size. */
#define STATS_VISIBLE 8

/* The title a game's shelf shows, so a gamelist's name reads the same in the
 * play-time list as on the card; NULL when the game is not on the card. */
static const char *shelf_title(const app *a, const char *tag, const char *file)
{
	int k, j;

	for (k = 0; k < a->sys.count; k++) {
		const game_list *gl = &a->view[k].list;

		if (strcmp(a->sys.systems[k].tag, tag)) continue;
		for (j = 0; j < gl->count; j++)
			if (!strcmp(gl->items[j].file, file)) return gl->items[j].title;
		return NULL;
	}
	return NULL;
}

/* Returns true if it sent the player to a shelf, so the menu above it closes
 * rather than redrawing over a screen that has moved on. */
static bool stats_screen(app *a)
{
	char vals[STATS_VISIBLE + 1][32], labels[STATS_VISIBLE + 1][80];
	/* +3: the total row above the list, then a rule and two notes below it. */
	menu_row rows[STATS_VISIBLE + 4];
	/* The total row says more than a duration, so it gets its own buffer
	 * rather than borrowing a game's - which the device compiler caught as a
	 * truncation the host compiler let through. */
	char total[32], total_val[64], detail[192];
	stats_window win = STATS_ALL;
	bool by_sys = false;
	long now = (long)time(NULL);
	int ngames = stats_summarize(win, by_sys, now);
	int cursor = 0, top = 0;
	/* The total above the list, then a rule and two notes below it. What is
	 * left over is the list, and it is asked rather than assumed. */
	int vis = menu_list_fit(1, 1, 2);

	/* The buffers are fixed, so however much room the panel has, the list
	 * stops where they do. */
	if (vis > STATS_VISIBLE) vis = STATS_VISIBLE;
	bool done = false;

	stats_format(stats_total_seconds(), total, sizeof total);

	while (!done && !want_quit && a->running) {
		int shown = 0, i;

		/* The total is a row rather than a heading, so it lines up with the
		 * games under it and reads as one table. */
		/* The window's name IS the total's label. A row already sat here
		 * saying "Total", and a heading that also said which slice would
		 * have said the same thing twice in two type sizes. */
		snprintf(labels[0], sizeof labels[0], "%s", stats_window_name(win));
		/* "over" spelled out put "This Month  2h 16m over 143 launches" at
		 * 764px against 704 of content, and menu_draw answered by cutting
		 * the LABEL - so the row said "This Y...". The separator is the
		 * cheapest 60px on the screen. */
		snprintf(total_val, sizeof total_val, "%s · %d launch%s",
		         total, stats_total_launches(),
		         stats_total_launches() == 1 ? "" : "es");
		rows[shown++] = (menu_row){ labels[0], total_val, false };

		for (i = top; i < ngames && shown <= vis; i++) {
			const char *tag, *file, *shelf;
			long secs;
			int launches;
			const char *dot;
			int len;

			if (!stats_at(i, &tag, &file, &secs, &launches)) break;
			if (by_sys) {
				/* stats.c knows the tag and nothing else - the display name
				 * lives in systems.cfg, which is this side of the seam. A
				 * system no longer on the card keeps its tag rather than
				 * disappearing: the time was still spent. */
				int k, found = 0;

				for (k = 0; k < a->sys.count; k++)
					if (!strcmp(a->sys.systems[k].tag, tag)) {
						snprintf(labels[shown], sizeof labels[0], "%s",
						         a->sys.systems[k].name);
						found = 1;
						break;
					}
				if (!found)
					snprintf(labels[shown], sizeof labels[0], "%s", tag);
			} else if ((shelf = shelf_title(a, tag, file))) {
				/* Cut to the row, as lib_title cuts the fallback below. */
				snprintf(labels[shown], sizeof labels[0], "%.*s",
				         (int)sizeof labels[0] - 1, shelf);
			} else {
			/* The extension is noise in a list of names. Last dot only, so a
			 * title with dots of its own keeps them. */
			dot = strrchr(file, '.');
			len = dot && dot != file ? (int)(dot - file) : (int)strlen(file);
			{
				/* Extension off, then the same title rule the shelf uses, so
				 * "Contra (USA).zip" reads as "Contra" here too. It was
				 * printing the cataloging: a column of names where every one
				 * ends in a region and a revision is a column you cannot
				 * scan. */
				char bare[LIB_NAME];

				snprintf(bare, sizeof bare, "%.*s", len, file);
				lib_title(bare, labels[shown], sizeof labels[0]);
			}
			}
			stats_format(secs, vals[shown], sizeof vals[0]);
			/* live, not selected. The selection is menu_draw's `sel`
			 * argument; passing it here instead drew every other row
			 * quiet and highlighted none of them. */
			rows[shown] = (menu_row){ labels[shown], vals[shown], true };
			shown++;
		}

		if (ngames == 0) {
			snprintf(labels[1], sizeof labels[1], "Nothing played yet");
			rows[1] = (menu_row){ labels[1], NULL, false };
			shown = 2;
			detail[0] = '\0';
		} else {
			/* What the selected row knows beyond its total. This is where
			 * the extra columns went: three numbers beside a game name in
			 * 704px reads as a spreadsheet nobody can scan, and only one row
			 * is being looked at anyway. */
			long longest = 0, last = 0, secs = 0;
			int lost = 0, n = 0;
			char lg[16], ag[16], tail[24];

			stats_at(cursor, NULL, NULL, &secs, &n);
			stats_extra(cursor, &longest, &last, &lost);
			stats_format(longest, lg, sizeof lg);
			stats_ago(last, now, ag, sizeof ag);
			/* Only when it happened. A "0 lost" on every row would train the
			 * eye to skip the one place the number matters. */
			if (lost) snprintf(tail, sizeof tail, "  ·  %d lost", lost);
			else      tail[0] = '\0';
			/* Written in full and left long. Four facts do not fit 704px and
			 * are not meant to: the note scrolls. */
			snprintf(detail, sizeof detail,
			         "%d launch%s  ·  longest %s  ·  %s%s",
			         n, n == 1 ? "" : "es", lg, ag, tail);
		}

		if (detail[0]) {
			rows[shown++] = MENU_RULE;
			rows[shown++] = MENU_NOTE(detail);
		}
		rows[shown++] = MENU_NOTE(by_sys ? "A: go    Y: by game    L/R: window"
		                                 : "A: go    Y: by system    L/R: window");

		plat_input_poll(&a->in);
		if (a->in.quit_requested) { a->running = false; return false; }
		{
			pwr_action pa = power_check(a);
			if (pa == PWR_POWEROFF) { power_off(a); return false; }
		}
		if (menu_leaving(a)) done = true;
		if (a->in.pressed[IN_SELECT]) muse_open(a, NULL, NULL);

		/* A goes to the row under the cursor: the game on its own shelf, or in
		 * by-system mode that system's. Not a launch.
		 *
		 * This screen is a still of what has been played, and it knows a tag
		 * and a file. That is not enough to start a game - the core, the
		 * folder, the display mode and the save paths all hang off the shelf.
		 * It could resolve every one of them, and then there would be a second
		 * launch path to keep in step with the first. Landing on the shelf
		 * costs one button and arrives where launching already works, with the
		 * art, the autosave dot and the game's own menu.
		 *
		 * A row can outlive its ROM - these are keyed on tag and file and
		 * survive the card changing - so a miss does nothing rather than
		 * guessing, which is the answer the boot-resume path gives too. */
		if (a->in.pressed[IN_ACCEPT] && ngames > 0) {
			const char *tag = NULL, *file = NULL;
			int si, gi;

			if (stats_at(cursor, &tag, &file, NULL, NULL))
				for (si = 0; si < a->sys.count; si++) {
					if (strcmp(a->sys.systems[si].tag, tag) != 0) continue;
					if (a->view[si].list.count == 0) break;
					a->sys_cursor = si;
					/* By system there is no file to look for, so the shelf
					 * keeps whichever game it was already on. */
					if (!by_sys)
						for (gi = 0; gi < a->view[si].list.count; gi++)
							if (!strcmp(a->view[si].list.items[gi].file, file)) {
								a->view[si].cursor = gi;
								break;
							}
					enter_system(a);
					return true;
				}
		}

		{	/* A view change re-folds the same rows under a new question, so
			 * the cursor cannot survive it - row 3 of "This Week by system"
			 * is not row 3 of anything else. */
			bool changed = false;

			if (in_repeat(&a->in, IN_RIGHT)) {
				win = (win + 1) % STATS_WINDOWS; changed = true;
			}
			if (in_repeat(&a->in, IN_LEFT)) {
				win = (win + STATS_WINDOWS - 1) % STATS_WINDOWS;
				changed = true;
			}
			if (a->in.pressed[IN_Y]) { by_sys = !by_sys; changed = true; }
			if (changed) {
				now = (long)time(NULL);
				ngames = stats_summarize(win, by_sys, now);
				stats_format(stats_total_seconds(), total, sizeof total);
				cursor = top = 0;
			}
		}

		if (ngames > 0) {
			if (in_repeat(&a->in, IN_DOWN) && cursor < ngames - 1) cursor++;
			if (in_repeat(&a->in, IN_UP)   && cursor > 0)          cursor--;
			/* The window follows the cursor rather than the other way round,
			 * so a long list scrolls one row at a time from either end. */
			if (cursor < top) top = cursor;
			if (cursor >= top + vis) top = cursor - vis + 1;
		}

		if (in_repeat(&a->in, IN_VOLUP))    plat_volume_nudge(+1);
		if (in_repeat(&a->in, IN_VOLDN))    plat_volume_nudge(-1);
		if (in_repeat(&a->in, IN_BRIGHTUP)) plat_brightness_nudge(+1);
		if (in_repeat(&a->in, IN_BRIGHTDN)) plat_brightness_nudge(-1);

		tick_tint(a);
		draw_shelf(a);
		SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(a->r, 0, 0, 0, 120);
		SDL_RenderFillRect(a->r, NULL);
		/* +1 because the total is row 0 and the games start under it. */
		menu_draw(a, "Play Time", rows, shown,
		          ngames ? cursor - top + 1 : -1, menu_std_width(a),
		          MENU_ACCENT);
		draw_chrome(a->r);
		plat_present(a->r);
		SDL_Delay(8);
	}
	return false;
}

static void about_screen(app *a)
{
	char ver[48], addr[80], batt[32], up[48];
	menu_row rows[4];
	char ssid[WIFI_SSID_MAX], ip[64];
	int pct = 0;
	bool charging = false;
	unsigned secs;
	bool done = false;

	snprintf(ver, sizeof ver, "%s", TORTOS_VERSION);

	while (!done && !want_quit && a->running) {
		static unsigned next_check;
		unsigned now = plat_now_ms();

		/* Same two-second cache as the menu row, and for the same reason:
		 * this loop runs every frame and wifi_status forks wpa_cli. */
		if (next_check == 0 || now >= next_check) {
			wifi_state ws = wifi_status(ssid, sizeof ssid, ip, sizeof ip);
			next_check = now + 2000;
			if (ws == WIFI_CONNECTED && ip[0])
				snprintf(addr, sizeof addr, "%s", ip);
			else if (ws == WIFI_CONNECTED)
				snprintf(addr, sizeof addr, "no address yet");
			else
				snprintf(addr, sizeof addr, "not connected");
			if (plat_battery(&pct, &charging))
				snprintf(batt, sizeof batt, "%d%%%s", pct,
				         charging ? " charging" : "");
			else
				snprintf(batt, sizeof batt, "unknown");
		}
		secs = now / 1000;
		snprintf(up, sizeof up, "%uh %02um", secs / 3600, (secs / 60) % 60);

		rows[0] = (menu_row){ "Version",   ver,  false };
		rows[1] = (menu_row){ "Address",   addr, false };
		rows[2] = (menu_row){ "Battery",   batt, false };
		rows[3] = (menu_row){ "Awake for", up,   false };

		plat_input_poll(&a->in);
		if (a->in.quit_requested) { a->running = false; return; }
		{
			pwr_action pa = power_check(a);
			if (pa == PWR_POWEROFF) { power_off(a); return; }
		}
		if (menu_leaving(a)) done = true;
		if (a->in.pressed[IN_SELECT]) muse_open(a, NULL, NULL);

		if (in_repeat(&a->in, IN_VOLUP))    plat_volume_nudge(+1);
		if (in_repeat(&a->in, IN_VOLDN))    plat_volume_nudge(-1);
		if (in_repeat(&a->in, IN_BRIGHTUP)) plat_brightness_nudge(+1);
		if (in_repeat(&a->in, IN_BRIGHTDN)) plat_brightness_nudge(-1);

		tick_tint(a);
		draw_shelf(a);
		SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(a->r, 0, 0, 0, 120);
		SDL_RenderFillRect(a->r, NULL);
		menu_draw(a, "About", rows, 4, -1, menu_std_width(a), MENU_ACCENT);
		draw_chrome(a->r);
		plat_present(a->r);
		SDL_Delay(8);
	}
}

/* Which page the shelf's controls are described on. See controls.h: the page
 * follows UI Direction, because which axis moves is what that setting does. */
static ctl_dir controls_dir(void)
{
	if (CARD_DIRS[g_dir].vertical) return CTL_VERTICAL;
	return CTL_HORIZONTAL;
}

/* MENU > Controls: what every button does, a page per place. Read-only like
 * About, and paged with left and right the way Play Time changes its window -
 * the whole list at once is thirty rows nobody reads. */
static void controls_screen(app *a)
{
	ctl_page page = CTL_SHELF;
	bool done = false;

	while (!done && !want_quit && a->running) {
		menu_row rows[CTL_MAX_ROWS];
		char head[80];
		int n = ctl_rows(page, controls_dir(), rows);

		snprintf(head, sizeof head, "Controls: %s", ctl_page_name(page));

		plat_input_poll(&a->in);
		if (a->in.quit_requested) { a->running = false; return; }
		{
			pwr_action pa = power_check(a);
			if (pa == PWR_POWEROFF) { power_off(a); return; }
		}
		if (menu_leaving(a)) done = true;
		/* SELECT opens Muse here as it does on every other menu screen -
		 * which is one of the things this page exists to tell you. */
		if (a->in.pressed[IN_SELECT]) muse_open(a, NULL, NULL);

		if (in_repeat(&a->in, IN_RIGHT)) page = (ctl_page)((page + 1) % CTL_PAGES);
		if (in_repeat(&a->in, IN_LEFT))
			page = (ctl_page)((page + CTL_PAGES - 1) % CTL_PAGES);
		if (in_repeat(&a->in, IN_VOLUP))    plat_volume_nudge(+1);
		if (in_repeat(&a->in, IN_VOLDN))    plat_volume_nudge(-1);
		if (in_repeat(&a->in, IN_BRIGHTUP)) plat_brightness_nudge(+1);
		if (in_repeat(&a->in, IN_BRIGHTDN)) plat_brightness_nudge(-1);

		tick_tint(a);
		draw_shelf(a);
		SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(a->r, 0, 0, 0, 120);
		SDL_RenderFillRect(a->r, NULL);
		menu_draw(a, head, rows, n, -1, menu_std_width(a), MENU_ACCENT);
		draw_chrome(a->r);
		plat_present(a->r);
		SDL_Delay(8);
	}
}

/* The widest row of one built menu. */
static int menu_measure(const menu_row *rows, int n, const char *heading)
{
	TTF_Font *fm = ui_font(UI_F_MENU), *fh = ui_font(UI_F_LABEL);
	int gap = menu_row_h(), w = 0, i;
	bool two_col = false;

	/* The same three row kinds menu_draw knows about. This function is a
	 * second copy of that measuring pass and the two must agree, or a panel is
	 * sized by one rule and drawn by another. */
	for (i = 0; i < n; i++)
		if (rows[i].value && !ROW_IS_NOTE(rows[i])) two_col = true;
	for (i = 0; i < n; i++) {
		int rw;

		if (!rows[i].label) continue;          /* a rule measures nothing */
		rw = ui_text_width(fm, rows[i].label);
		if (two_col && rows[i].value && !ROW_IS_NOTE(rows[i]))
			rw += gap + ui_text_width(fm, rows[i].value);
		if (rw > w) w = rw;
	}
	if (heading) {
		int hw = ui_text_width(fh, heading);
		if (hw > w) w = hw;
	}
	return w;
}

/* One width for both shelf menus, every system, and every value their rows can
 * cycle to. The panel is a frame the lists sit inside rather than something
 * that resizes to whatever is selected: without this, cycling display mode from
 * "Aspect" to "Integer" widens the slab under the cursor, and walking from
 * the systems row into a system resizes it again.
 *
 * Measured across all of that rather than picked, so a longer label, another
 * system or a larger font scale widens the frame instead of overflowing it.
 * Cached because it is a few hundred text measurements and none of its inputs
 * change while a menu is open. */
static int menu_shelf_width(app *a)
{
	menu_row rows[MENU_MAX_ROWS];
	menu_bufs bufs;
	const char *heading;
	int w, n, i, k;

	if (a->menu_w) return a->menu_w;

	n = menu_build(a, SCREEN_SYSTEMS, a->sys_cursor, rows, &bufs, &heading);
	w = menu_measure(rows, n, heading);
	/* And the plorpOS menu's two settings submenus, whose rows stood in it
	 * until plorpos-z0d.1 and set this width - Mute Switch's "muse button
	 * lock" the widest. Measured here so the frame did not narrow under
	 * every panel when they moved a level down. */
	{
		char ss[WIFI_SSID_MAX];
		sys_ui u;
		int mw;

		menu_ui(a, SCREEN_SYSTEMS, 0, &u, ss, sizeof ss);
		n = sys_menu_system_build(&u, rows, &bufs, &heading);
		mw = menu_measure(rows, n, heading);
		if (mw > w) w = mw;
		n = sys_menu_ui_build(&u, rows, &heading);
		mw = menu_measure(rows, n, heading);
		if (mw > w) w = mw;
	}

	/* Every value that can be cycled on this menu, at its widest, so the
	 * panel does not resize under the row being cycled. Sort By joined
	 * Display Mode in that category the moment it stopped being a
	 * placeholder - "Recently Added" is 187px wider than "Name".
	 *
	 * Muse's menu and Favorites' are their own rows, Sort By second and no
	 * Display Mode, so the enum's row numbers are only the rest's. Written
	 * through them on Muse's, this measured Rescan Folder carrying a sort
	 * order, a row no menu has. */
	for (i = 0; i < a->sys.count; i++) {
		bool muse = is_muse(&a->sys.systems[i]);
		bool own  = muse || a->view[i].owner;
		int  srow = own ? 1 : SM_SORT, show = -1;

		n = menu_build(a, SCREEN_GAMES, i, rows, &bufs, &heading);
		if (muse) {
			sm_muse_row ids[SM_MUSE_ROWS];
			int both = ml_count(&g_muse, true) && ml_count(&g_muse, false);
			int nn = sys_menu_muse_rows(muse_books_shown(), both, ids);

			for (k = 0; k < nn; k++) {
				if (ids[k] == SMM_SORT) srow = k;
				if (ids[k] == SMM_SHOW) show = k;
			}
			/* The longer of Show's two values, so turning it over does not
			 * change the menu's width under the cursor. */
			if (show >= 0) {
				int mw;

				rows[show].value = "Audiobooks";
				mw = menu_measure(rows, n, heading);
				if (mw > w) w = mw;
			}
		}
		for (k = 0; !own && k < DMODE_COUNT; k++) {
			int mw;
			rows[SM_DISPLAY].value = DMODES[k].label;
			mw = menu_measure(rows, n, heading);
			if (mw > w) w = mw;
		}
		if (!own) rows[SM_DISPLAY].value = DMODES[0].label;
		for (k = 0; !own && is_pico8(&a->sys.systems[i]) && k < 2; k++) {
			int mw;
			rows[SM_CORE].value = ENGINE_LABEL(k);
			mw = menu_measure(rows, n, heading);
			if (mw > w) w = mw;
		}
		for (k = 0; k < (muse ? ML_ORDERS : SORT_COUNT); k++) {
			int mw;
			rows[srow].value = muse ? ml_order_label((ml_order)k, false) : SORTS[k].label;
			mw = menu_measure(rows, n, heading);
			if (mw > w) w = mw;
			if (muse) {
				rows[srow].value = ml_order_label((ml_order)k, true);
				mw = menu_measure(rows, n, heading);
				if (mw > w) w = mw;
			}
		}
	}
	a->menu_w = w;
	return w;
}

/* The width every menu reached from the shelf uses.
 *
 * Menus sized themselves to their own rows, so the panel changed size on
 * nearly every screen. Measured on the device 2026-09-08: the TortOS menu drew
 * a 794px panel, Play Time 791, Bluetooth 737, Wi-Fi 640 - so opening Wi-Fi
 * from the menu shrank the panel by 154px, about 15% of the display, and grew
 * it again on the way out.
 *
 * Sizing to content was right while a label that did not fit was cut, because
 * cutting a name is a loss. A long label scrolls now, so a panel no longer has
 * to grow to hold one, and the trade is a little empty air in a narrow menu
 * against a panel that stops jumping.
 *
 * 800px of PANEL, stated rather than measured, so it does not drift when a
 * row's text changes. What menu_draw takes is the content width and it adds
 * menu_pad() on each side, so the pad comes off here - by calling the same
 * function menu_draw calls, because a hardcoded 48 would be right only at one
 * text size.
 *
 * The floor is not decoration. menu_shelf_width is the widest row in the
 * shelf menus, and a panel narrower than that sets one of them scrolling. It
 * is "Bluetooth" and the paired headset's name today - a row that grows with
 * a device nobody here chose the name of - measured at 699px against the 704
 * of content 800 leaves. That it clears is not an estimate: the panel draws at
 * exactly 800 on the device, and had the row been wider max() would have
 * widened the panel rather than let it scroll. Five pixels is not much, and
 * max() is the whole reason a longer name costs air instead of a cut.
 *
 * Not for the one-row notices - "Scanning...", "Could not start". Those are
 * messages, and a message stretched to menu width reads as a menu that failed
 * to load. Not for the in-game menu either, which sits over a paused game. */
static int menu_std_width(app *a)
{
	int content = 800 - menu_pad() * 2;
	int floor   = menu_shelf_width(a);

	return floor > content ? floor : content;
}

static void tortos_menu_draw(app *a, int sel)
{
	menu_row rows[MENU_MAX_ROWS];
	menu_bufs bufs;
	const char *heading;
	int n = menu_build(a, a->screen, a->sys_cursor, rows, &bufs, &heading);

	SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(a->r, 0, 0, 0, 120);
	SDL_RenderFillRect(a->r, NULL);
	/* The shelf's menu is TortOS's own on the systems screen and a system's on
	 * a game list, which is where it gains rows that belong to that system. */
	menu_draw(a, heading, rows, n, sel, menu_shelf_width(a),
	          a->screen == SCREEN_SYSTEMS ? MENU_ACCENT : a->tint);
}

/* The system menu's context. It holds the app rather than a copy of what was
 * on screen when it opened, so `a->screen` and `a->sys_cursor` are read live
 * exactly as the hand-written loop read them. Neither can change while the
 * menu is up; reading them is simply one fewer thing that could drift. */
typedef struct {
	app      *a;
	menu_bufs bufs;   /* the built rows point into this, so it outlives them */
	/* Which of the two menus to build. Normally the screen you are on, but
	 * Both has no screens to be on: there MENU means the firmware and B
	 * means this system, and neither can be inferred from where you are. */
	int       screen;
} sysmenu_ctx;

/* Muse Settings, the fifth row of Muse's menu (TortOS-28l): whether a button
 * pressed while music plays in the dark wakes the screen - see music_dark,
 * which reads it - and how long music plays untouched before the screen goes
 * off - see idle_due. */
static const char *const MUSE_SCREEN_OFF_LABEL[] =
	{ "5s", "10s", "15s", "30s", "1m", "Never" };

static int muse_screen_off_at(void)
{
	int k, v = db_get_int(db_dev(), "muse.screenoff", 10);

	for (k = 0; k < MUSE_SCREEN_OFF_COUNT; k++)
		if (MUSE_SCREEN_OFF[k] == v) return k;
	return 1;                             /* 10s, should it be off-ladder */
}

static int muse_set_build(void *ctx, menu_row *rows, int max,
                          const char **heading)
{
	(void)ctx;
	(void)max;
	*heading = "Muse Settings";
	rows[0] = (menu_row){ "Wake Screen On Press",
	                      db_get_int(db_dev(), "muse.wake", 1) ? "Yes" : "No",
	                      true };
	rows[1] = (menu_row){ "Screen Off",
	                      MUSE_SCREEN_OFF_LABEL[muse_screen_off_at()], true };
#if defined(PLATFORM_GKD)
	/* The GKD has no switch to hold, so this is it (gkd.34) - see
	 * plat_hold_switch. The Brick's Mute Switch row's db key. */
	rows[2] = (menu_row){ "Sleep Button Lock",
	                      plat_hold_switch() ? "On" : "Off", true };
	return 3;
#else
	return 2;
#endif
}

/* Wake is a toggle, so A flips it as well as left/right - as Mute Switch.
 * Screen Off is a ladder: left/right step along it, A steps forward and
 * wraps, as Display Mode's A does. */
static menu_result muse_set_key(app *a, void *ctx, in_button key, int sel)
{
	int at;

	(void)a;
	(void)ctx;
	if (sel == 0 && (key == IN_LEFT || key == IN_RIGHT || key == IN_ACCEPT))
		db_set_int(db_dev(), "muse.wake",
		           !db_get_int(db_dev(), "muse.wake", 1));
	if (sel == 1 && (key == IN_LEFT || key == IN_RIGHT || key == IN_ACCEPT)) {
		at = muse_screen_off_at();
		if (key == IN_ACCEPT)     at = (at + 1) % MUSE_SCREEN_OFF_COUNT;
		else if (key == IN_LEFT)  at = at > 0 ? at - 1 : 0;
		else if (at < MUSE_SCREEN_OFF_COUNT - 1) at++;
		db_set_int(db_dev(), "muse.screenoff", MUSE_SCREEN_OFF[at]);
	}
#if defined(PLATFORM_GKD)
	/* Sleep Button Lock: a toggle, like Wake. */
	if (sel == 2 && (key == IN_LEFT || key == IN_RIGHT || key == IN_ACCEPT)) {
		bool lock = !plat_hold_switch();

		db_set_int(db_dev(), "muteswitch", lock);
		plat_mute_switch_lock(lock);
	}
#endif
	return MENU_STAY;
}

static void muse_settings_screen(app *a)
{
	menu_run(a, &(menu_style){ .accent = MENU_ACCENT,
	                           .fixed_w = menu_std_width(a) },
	         muse_set_build, muse_set_key, NULL);
}

/* Settings > Scraping (TortOS-mh0): Box Art and the ScreenScraper account,
 * moved here from the top menu, and gamelist.xml import. The rows are
 * src/sys_menu.c's, so tools/menu-check.c can state what they say. */
static int scraping_build(void *ctx, menu_row *rows, int max,
                          const char **heading)
{
	char ss[WIFI_SSID_MAX];
	sys_ui u = { 0 };

	(void)ctx;
	(void)max;
	u.wifi    = menu_wifi(ss, sizeof ss);
	u.ss_have = ss_have_dev();
	u.ss_in   = ss_signed_in();
	u.ss_name = u.ss_in ? ss_user() : NULL;
	return sys_menu_scraping_build(&u, rows, heading);
}

/* Every system's gamelist.xml into the games table. Fill In Missing leaves a
 * game that already has metadata - a ScreenScraper hit, an earlier import -
 * alone; Replace All writes over it. The same rule as `--meta`. */
static void gamelist_import_screen(app *a)
{
	static const char *const OPTS[] = { "Fill in missing", "Replace all" };
	const char *heading = "Import gamelist.xml";
	char l0[64], l1[96], l2[CFG_STR + 48];
	menu_row rows[4];
	gl_result r;
	int pick, n = 0;

	pick = pick_panel(a, heading, "Games that already have metadata:", OPTS, 2);
	if (pick < 0) return;
	wait_panel(a, heading, "Importing...");
	gl_import(db_lib(), P_ROMS, &a->sys, pick == 1, &r);
	fprintf(stderr, "gamelist: %d list(s), %d written, %d skipped, %d rejected, "
	        "%d unreadable\n", r.lists, r.wrote, r.skipped, r.bad, r.unreadable);
	/* A list's names are the shelves' titles, and a shelf takes its titles
	 * when it is scanned (titles_apply). An import adds and removes no game,
	 * so every system stays where it was and this menu can stay open. */
	if (r.wrote) rescan_all(a);

	if (!r.lists && !r.unreadable) {
		rows[n++] = (menu_row){ "No gamelist.xml in any system folder", NULL, false };
	} else {
		snprintf(l0, sizeof l0, "Imported %d game%s", r.wrote, r.wrote == 1 ? "" : "s");
		rows[n++] = (menu_row){ l0, NULL, false };
		if (r.skipped) {
			snprintf(l1, sizeof l1, "Skipped %d (already had metadata)", r.skipped);
			rows[n++] = (menu_row){ l1, NULL, false };
		}
		if (r.unreadable) {
			if (r.unreadable == 1)
				snprintf(l2, sizeof l2, "Could not read %s's gamelist",
				         r.first_unreadable);
			else
				snprintf(l2, sizeof l2, "Could not read %d gamelists, first %s",
				         r.unreadable, r.first_unreadable);
			rows[n++] = (menu_row){ l2, NULL, false };
		}
	}
	rows[n++] = (menu_row){ "B to close", NULL, false };
	note_panel(a, heading, rows, n);
}

static menu_result scraping_key(app *a, void *ctx, in_button key, int sel)
{
	(void)ctx;
	if (key != IN_ACCEPT) return MENU_STAY;
	switch (sel) {
	case SC_BOXART: art_screen(a, NULL, NULL, NULL, NULL, MENU_ACCENT); break;
	case SC_SS:     ss_signin_screen(a); break;
	case SC_IMPORT: gamelist_import_screen(a); break;
	default: break;
	}
	return MENU_STAY;
}

static void scraping_screen(app *a)
{
	menu_run(a, &(menu_style){ .accent = MENU_ACCENT,
	                           .fixed_w = menu_std_width(a) },
	         scraping_build, scraping_key, NULL);
}

/* Settings > System Settings and UI Settings (plorpos-z0d.1): rows that
 * stood in the plorpOS menu itself until then, with the keys they always had.
 * The rows are src/sys_menu.c's, so tools/menu-check.c can state them. */
typedef struct { app *a; menu_bufs bufs; } subset_ctx;

static int system_settings_build(void *ctx, menu_row *rows, int max,
                                 const char **heading)
{
	subset_ctx *c = ctx;
	char ss[WIFI_SSID_MAX];
	sys_ui u;

	(void)max;
	menu_ui(c->a, SCREEN_SYSTEMS, 0, &u, ss, sizeof ss);
	return sys_menu_system_build(&u, rows, &c->bufs, heading);
}

static menu_result system_settings_key(app *a, void *ctx, in_button key, int sel)
{
	int d = key == IN_RIGHT ? 1 : key == IN_LEFT ? -1 : 0;

	(void)ctx;
	/* Auto Sleep, on the left/right idiom Display mode uses. */
	if (d && sel == ST_SLEEP) {
		int k, at = AUTO_OFF_COUNT - 1;

		/* The first rung at or above, not an exact match: a value from an
		 * older ladder (45, 90, 240...) steps from the next rung, not from
		 * never. */
		for (k = 0; k < AUTO_OFF_COUNT; k++)
			if (AUTO_OFF[k] >= a->auto_off) { at = k; break; }
		at += d;
		if (at < 0) at = 0;
		if (at >= AUTO_OFF_COUNT) at = AUTO_OFF_COUNT - 1;
		a->auto_off = AUTO_OFF[at];
		auto_off_save(a->auto_off);
		/* Mutually exclusive with Auto Off, matching NextUI: no two-tier
		 * escalation ladder, so at most one of the pair is ever armed. */
		if (a->auto_off && a->auto_poweroff) {
			a->auto_poweroff = 0;
			auto_poweroff_save(0);
		}
		/* From now, not from whenever the last countdown began: choosing 30s
		 * should not inherit two minutes of an old one already spent. */
		a->idle.since_ms = plat_now_ms();
		return MENU_STAY;
	}
	/* Auto Off, same idiom as Auto Sleep above and mutually exclusive with
	 * it - see that block's comment. */
	if (d && sel == ST_AUTO_OFF) {
		int k, at = 0;

		for (k = 0; k < AUTO_POWEROFF_COUNT; k++)
			if (AUTO_POWEROFF[k] == a->auto_poweroff) { at = k; break; }
		at += d;
		if (at < 0) at = 0;
		if (at >= AUTO_POWEROFF_COUNT) at = AUTO_POWEROFF_COUNT - 1;
		a->auto_poweroff = AUTO_POWEROFF[at];
		auto_poweroff_save(a->auto_poweroff);
		if (a->auto_poweroff && a->auto_off) {
			a->auto_off = 0;
			auto_off_save(0);
		}
		a->idle.since_ms = plat_now_ms();
		return MENU_STAY;
	}
	/* Suspend Timeout, same idiom, and independent of both rows above: it
	 * governs light sleep however it began. Clamped at both ends - there is
	 * no "never", as in NextUI. */
	if (d && sel == ST_SUSPEND) {
		int k, at = SUSPEND_TIMEOUT_COUNT - 1;

		/* The first rung at or above, as Auto Sleep: an older ladder's 5 or
		 * 45 steps from 30 or 60. */
		for (k = 0; k < SUSPEND_TIMEOUT_COUNT; k++)
			if (SUSPEND_TIMEOUT[k] >= plat_suspend_timeout_secs()) { at = k; break; }
		at += d;
		if (at < 0) at = 0;
		if (at >= SUSPEND_TIMEOUT_COUNT) at = SUSPEND_TIMEOUT_COUNT - 1;
		db_set_int(db_dev(), "suspendtimeout", SUSPEND_TIMEOUT[at]);
		return MENU_STAY;
	}
	/* Battery Percentage: a toggle, A or left/right, as Mute Switch. */
	if (sel == ST_BATTPCT && (d || key == IN_ACCEPT)) {
		db_set_int(db_dev(), "battpct", db_get_int(db_dev(), "battpct", 0) != 1);
		battery_indicator_changed();
		return MENU_STAY;
	}
	/* Mute Switch: a toggle, so A flips it as well as left/right
	 * (TortOS-ib9). */
#if !defined(PLATFORM_GKD) && !defined(PLATFORM_H700)
	if (sel == ST_MUTESW && (d || key == IN_ACCEPT)) {
		bool lock = db_get_int(db_dev(), "muteswitch", 0) != 1;

		db_set_int(db_dev(), "muteswitch", lock);
		plat_mute_switch_lock(lock);
		return MENU_STAY;
	}
#endif
	return MENU_STAY;
}

static void system_settings_screen(app *a)
{
	subset_ctx c = { .a = a };

	menu_run(a, &(menu_style){ .accent = MENU_ACCENT,
	                           .fixed_w = menu_std_width(a) },
	         system_settings_build, system_settings_key, &c);
}

static int ui_settings_build(void *ctx, menu_row *rows, int max,
                             const char **heading)
{
	subset_ctx *c = ctx;
	char ss[WIFI_SSID_MAX];
	sys_ui u;

	(void)max;
	menu_ui(c->a, SCREEN_SYSTEMS, 0, &u, ss, sizeof ss);
	return sys_menu_ui_build(&u, rows, heading);
}

static menu_result ui_settings_key(app *a, void *ctx, in_button key, int sel)
{
	int d = key == IN_RIGHT ? 1 : key == IN_LEFT ? -1 : 0;

	(void)ctx;
	/* Text size, same idiom. Changing it reopens every font, so the whole UI
	 * is rebuilt: the panel's cached width is measured from font metrics, and
	 * any card generated for a game with no box art has its title baked in at
	 * the old size. Both are dropped here rather than left subtly wrong. */
	/* The UI theme. No font is reopened, so only the
	 * card textures are dropped - but the same shape: change it, throw away
	 * what was drawn from the old value, draw it again. A steps as well as
	 * left and right, so the row can be cycled without leaving the thumb.
	 *
	 * Themes the systems shelf today. The name is deliberately wider than
	 * that, so menu colors or the accent rule can join without the row having
	 * to be renamed a second time. */
	if (sel == US_THEME && (d || key == IN_ACCEPT)) {
		int k = cards_step(g_cards, d ? d : 1);

		if (k != g_cards) {
			g_cards = k;
			free_all_textures(a);
			prime_sys_window(a);
			db_set_str(db_dev(), "cards", CARD_SETS[g_cards].id);
		}
		return MENU_STAY;
	}

	/* Which way both shelves run. Only the cards' positions change, not the
	 * textures, so nothing is dropped and there is nothing to rebuild. */
	if (sel == US_DIR && (d || key == IN_ACCEPT)) {
		g_dir = cards_dir_step(g_dir, d ? d : 1);
		db_set_str(db_dev(), "cards_dir", CARD_DIRS[g_dir].id);
		return MENU_STAY;
	}

	return MENU_STAY;
}

static void ui_settings_screen(app *a)
{
	subset_ctx c = { .a = a };

	menu_run(a, &(menu_style){ .accent = MENU_ACCENT,
	                           .fixed_w = menu_std_width(a) },
	         ui_settings_build, ui_settings_key, &c);
}

static int sysmenu_build(void *ctx, menu_row *rows, int max,
                         const char **heading)
{
	sysmenu_ctx *c = ctx;
	int n = menu_build(c->a, c->screen, c->a->sys_cursor, rows, &c->bufs,
	                   heading);

	return n > max ? max : n;
}

/* Left and right cycle the value on a row that has one; A opens whatever the
 * row leads to. Everything else the runner has already dealt with. */
static menu_result sysmenu_key(app *a, void *ctx, in_button key, int sel)
{
	int d = key == IN_RIGHT ? 1 : key == IN_LEFT ? -1 : 0;

	(void)ctx;

	if (a->screen == SCREEN_GAMES) {
		/* Muse's menu is its own rows - see SM_MUSE_ROWS - and which is
		 * where depends on what its shelf shows, so the enum's row numbers
		 * below mean nothing on it: sys_menu_muse_rows says. Sort By goes on
		 * to the Sort By below like any shelf's. Album Art is a dead row off
		 * the network, and the runner does not land on dead rows. Muse
		 * Settings is the fork's own row (TortOS-28l), always last. */
		if (is_muse(&a->sys.systems[a->sys_cursor])) {
			sm_muse_row ids[SM_MUSE_ROWS];
			int n = sys_menu_muse_rows(muse_books_shown(),
			                           ml_count(&g_muse, true) && ml_count(&g_muse, false),
			                           ids);
			sm_muse_row id = sel >= 0 && sel < n ? ids[sel] : SMM_COUNT;

			if (id == SMM_SORT) {
				sel = SM_SORT;
			} else if (id == SMM_SHOW) {
				/* Two values, so left, right and A all turn it over. */
				if (d || key == IN_ACCEPT)
					muse_show(&a->view[a->sys_cursor], !muse_books_shown());
				return MENU_STAY;
			} else {
				if (key != IN_ACCEPT) return MENU_STAY;
				if (id == SMM_ART) { album_art_screen(a); return MENU_STAY; }
				if (id == SMM_SETTINGS) { muse_settings_screen(a); return MENU_STAY; }
				if (id != SMM_RESCAN) return MENU_STAY;
				wait_panel(a, "Muse", "Scanning...");
				rescan_all(a);
				return MENU_DONE;
			}
		}
		/* Favorites' menu is its own rows too, Games and Sort By - see
		 * SM_FAV_ROWS - so its Sort By is row 1 and not SM_SORT. Nothing here
		 * knew that from the day the menu was cut to two rows, and left and
		 * right on Favorites' Sort By did nothing at all. */
		if (a->view[a->sys_cursor].owner) sel = sel == 1 ? SM_SORT : SM_GAMES;
		/* PICO-8's engine, fake08 or the owner's pico8_64. Two values, so
		 * left, right and A all turn it over, saved at once like Display
		 * Mode. No check for the binary here: a missing one says so at
		 * launch, the same as Splore. */
		if (sel == SM_CORE && (d || key == IN_ACCEPT) &&
		    is_pico8(&a->sys.systems[a->sys_cursor])) {
			sysview *v = &a->view[a->sys_cursor];

			v->native = !v->native;
			engine_save(a, a->sys_cursor);
			return MENU_STAY;
		}
		/* Display mode, saved the moment it changes because there is no
		 * confirm step to hang the write off. */
		if (d && sel == SM_DISPLAY) {
			sysview *v = &a->view[a->sys_cursor];

			v->dmode = (v->dmode + d + DMODE_COUNT) % DMODE_COUNT;
			display_save(a);
			return MENU_STAY;
		}
		/* Sort order, on the same left/right idiom.
		 *
		 * The cursor follows the GAME, not its index. Re-sorting under a
		 * cursor left where it sat means closing the menu onto a different
		 * game than the one that was selected when it opened - the shelf
		 * would appear to have jumped on its own. Which game you were
		 * looking at is the thing that survives a reorder; where it happened
		 * to sit in the old order is not. On Muse's shelf the same goes for
		 * the album, whose folder is its `file`. */
		if (d && sel == SM_SORT) {
			sysview *v = &a->view[a->sys_cursor];
			char keep[LIB_PATH];
			int i, keep_owner = -1;

			keep[0] = '\0';
			if (v->cursor >= 0 && v->cursor < v->list.count) {
				snprintf(keep, sizeof keep, "%s",
				         v->list.items[v->cursor].file);
				/* On Favorites a file name is not enough: the same one can
				 * be on the shelf twice, from two systems. */
				if (v->owner) keep_owner = v->owner[v->cursor];
			}

			v->sort = is_muse(&a->sys.systems[a->sys_cursor])
			        ? (v->sort + d + ML_ORDERS) % ML_ORDERS
			        : sort_step(v->sort, d);
			sort_shelf(a, a->sys_cursor);
			sort_save(a);

			/* The textures are indexed by position, so they moved with
			 * nothing. Dropping them lets the next frame fetch each card's
			 * art for where it now sits. */
			free_view_textures(v);

			v->cursor = 0;
			for (i = 0; i < v->list.count; i++)
				if (!strcmp(v->list.items[i].file, keep) &&
				    (!v->owner || v->owner[i] == keep_owner)) {
					v->cursor = i;
					break;
				}
			cf_reset(&v->cf, v->cursor);
			return MENU_STAY;
		}
		if (key != IN_ACCEPT) return MENU_STAY;
		/* The system's own color, not the menu's. This acts on the shelf you
		 * are looking at, and menu_draw already follows that rule everywhere
		 * else. */
		if (sel == SM_BOXART)
			art_screen(a, a->sys.systems[a->sys_cursor].folder, NULL, NULL,
			           NULL, a->sys.systems[a->sys_cursor].accent);
		if (sel == SM_RESCAN) {
			wait_panel(a, a->sys.systems[a->sys_cursor].name, "Scanning...");
			rescan_all(a);
			/* Close, rather than redraw this menu over a shelf that may have
			 * just lost the system it was built for. Acting and closing is
			 * also what pressing it means. */
			return MENU_DONE;
		}
		return MENU_STAY;
	}

	/* Two positions, so left and right and A all do the same thing: there is
	 * nothing to step through, only something to turn off and on. */
	if (sel == PM_AUDIO && (d || key == IN_ACCEPT)) {
		g_aout_policy = g_aout_policy == AOUT_AUTO ? AOUT_SPEAKER : AOUT_AUTO;
		aout_save(g_aout_policy);
		/* Forced: the resolved DEVICE may not have changed - pinning Speaker
		 * with a cable in is still the codec - but the setting did, and the
		 * next unplug must act on the new policy rather than on a cached
		 * device string that happens to match. */
		aout_apply(true);
		return MENU_STAY;
	}

	if (key != IN_ACCEPT) return MENU_STAY;
	switch (sel) {
	case PM_WIFI:         wifi_screen(a); break;
	case PM_SYSTEM:       system_settings_screen(a); break;
	case PM_UI:           ui_settings_screen(a); break;
	case PM_SCRAPING:     scraping_screen(a); break;
	case PM_BT:           bt_screen(a); break;
	case PM_STATS:        if (stats_screen(a)) return MENU_DONE; break;
	case PM_CONTROLS:     controls_screen(a); break;
	case PM_ABOUT:        about_screen(a); break;
	default: break;
	}
	return MENU_STAY;
}

static void tortos_menu_for(app *a, int screen)
{
	sysmenu_ctx c;

	memset(&c, 0, sizeof c);
	c.a = a;
	c.screen = screen;
	menu_style st = {
		/* Measured across every system and every display mode, so the panel
		 * does not resize while Display Mode is being cycled on it. */
		.fixed_w     = menu_std_width(a),
		.accent      = MENU_ACCENT,
		/* On a game shelf this menu is about THAT system, so it wears the
		 * shelf's color - which tick_tint is still moving while it is open. */
		.follow_tint = screen != SCREEN_SYSTEMS,
	};

	menu_run(a, &st, sysmenu_build, sysmenu_key, &c);
}

static void tortos_menu(app *a)
{
	tortos_menu_for(a, a->screen);
}

/* ---------- launching ----------------------------------------------------- */

static char env_buf[10][CFG_STR * 2];
static const char *child_env[24];

static void build_child_env(void)
{
	size_t i;
	int n = 0;
	for (i = 0; plat_child_env[i]; i++) child_env[n++] = plat_child_env[i];
	snprintf(env_buf[0], sizeof env_buf[0], "ROMS_PATH=%s", P_ROMS);
	snprintf(env_buf[1], sizeof env_buf[1], "SYSTEM_PATH=%s", P_ROOT);
	snprintf(env_buf[2], sizeof env_buf[2], "CORES_PATH=%s/cores", P_ROOT);
	snprintf(env_buf[3], sizeof env_buf[3], "USERDATA_PATH=%s", P_USERDATA);
	snprintf(env_buf[4], sizeof env_buf[4], "SHARED_USERDATA_PATH=%s", P_SHARED);
	snprintf(env_buf[5], sizeof env_buf[5], "LOGS_PATH=%s/logs", P_USERDATA);
	snprintf(env_buf[6], sizeof env_buf[6], "HOME=%s", P_USERDATA);
	snprintf(env_buf[7], sizeof env_buf[7], "LD_LIBRARY_PATH=%s/lib%s", P_ROOT,
	         plat_child_libpath);
	for (i = 0; i <= 7; i++) child_env[n++] = env_buf[i];
	child_env[n] = NULL;
}

/* The in-game menu - the launcher's, over Diatom's pause.
 *
 * MENU in a game makes Diatom write a preview of the frame, stop presenting,
 * and say PAUSED. From that word the display is ours: the menu is that frame
 * dimmed, with the rows a player expects - Continue, Save, Load, Reset, Quit
 * - drawn with the launcher's own font and glow. The menu is the launcher's,
 * not something patched into the emulator.
 *
 * Every row acts through one protocol line. Save and Load use the same autosave
 * paths the launch handed over, so the autosave funnel stays one thing. */
/* Display sits with the things you do to the game rather than the things you do
 * to a save, because it is the one row whose effect you judge by looking at the
 * game behind the menu. */
/* gm_row, gm_ui and gm_rows: src/game_menu.h */

/* The paused frame, drawn where the game actually is.
 *
 * Diatom reports its rect with every DISPLAY message (its ADR-0022 put it there
 * so a launcher need not recompute it from geometry it does not have), and the
 * preview it writes is the CORE'S frame - 256x224 for an NES, no display mode
 * applied. Stretching that to the panel, which is what this used to do, showed
 * a game that looked nothing like the one paused underneath at any mode that
 * does not fill the screen, and made the picture jump size the moment MENU was
 * pressed.
 *
 * Falls back to filling the panel when Diatom has not said - the standalone
 * path, and the first moments of a launch. */
static void draw_paused_frame(app *a, SDL_Texture *bg)
{
	if (bg) plat_draw_paused(a->r, bg);
}

/* Copy the paused frame's preview beside a manual save, so the slot strip can
 * show what is inside each slot. The pause preview IS the frame the save
 * serializes - Diatom wrote it on the way into the menu - so a straight copy
 * is the truthful thumbnail, no protocol round trip needed. */
/* The SOURCE is opened first, and the destination only if that worked.
 *
 * Opening both up front truncates the destination whenever the source is
 * missing - so a copy that cannot happen destroys the file it was going to
 * replace. Harmless while the only caller copied a preview a pause had just
 * written; not harmless once the Auto card is written this way, where the
 * thing being overwritten is the last good picture of a save. */
static void copy_file(const char *from, const char *to)
{
	FILE *a, *b;
	char buf[16384];
	size_t n;

	if (!from || !*from || !to || !*to) return;
	if (!(a = fopen(from, "rb"))) return;
	if (!(b = fopen(to, "wb"))) { fclose(a); return; }

	while ((n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, b);
	fclose(a);
	fclose(b);
}

/* The slot carousel: Auto plus the manual slots for Load, the manual slots for
 * Save. One slot at a
 * time, large - the paused frame at a size you can actually read - with the
 * save's own timestamp under it and a dot rail for where you are. Left and
 * right cycle; Load skips slots with nothing behind them, Save cannot aim at
 * Auto, which belongs to the exit funnel alone. */
typedef struct {
	SDL_Texture *thumb[GM_SLOTS + 1];   /* [0]=Auto, [1..GM_SLOTS] */
	int have[GM_SLOTS + 1];
	char when[GM_SLOTS + 1][40];
	/* Every slot of one game holds the same machine's frame, so one aspect
	 * describes them all and the picture can be framed exactly rather than
	 * dropped into a fixed box with bars down its sides. 4:3 until a slot
	 * with a picture in it says otherwise. */
	float aspect;
	int saving;
} slot_view;

/* Drawing only, so one frame of it can be rendered by --shot without a game
 * running or a device in hand. */
static void slot_draw(app *a, const slot_view *sv, int sel)
{
	/* `area` is the layout slot the picture is fitted into; nothing is ever
	 * drawn to it. The border is the picture's own edge in the system's
	 * color - a 240x160 GBA frame and a 256x224 NES frame are different
	 * shapes, and neither should be padded out into the same rectangle. */
	const SDL_Rect area = { (TORTOS_SCREEN_W - 700) / 2, STAGE_Y + 129, 700, 451 };
	const int bw = 12;
	SDL_Rect img = area, frame;
	char slotname[16];
	int line_menu = ui_font_line(UI_F_MENU), line_meta = ui_font_line(UI_F_META);
	int i;

	/* The heading sits close to the top edge so the picture gets the middle of
	 * the screen. Everything below hangs off the IMAGE rather than off `area`,
	 * so a frame shorter than the layout slot pulls its own caption up with it
	 * instead of leaving a gap.
	 *
	 * The margins are mirrored: the heading's top sits as far from the top of
	 * the screen as the marker rail's bottom sits from the bottom of it, and
	 * the heading is centered in the gap above the frame. That fixes every
	 * number here to one another rather than to taste, so changing the image
	 * size moves the rest to match instead of drifting into something. */
	ui_text(a->r, ui_font(UI_F_LABEL), sv->saving ? "Save to" : "Load from",
	        TORTOS_SCREEN_W / 2, 39, 0, UI_TEXT_DIM);

	if ((float)area.w / sv->aspect <= (float)area.h) {
		img.w = area.w;
		img.h = (int)(area.w / sv->aspect + 0.5f);
	} else {
		img.h = area.h;
		img.w = (int)(area.h * sv->aspect + 0.5f);
	}
	img.x = area.x + (area.w - img.w) / 2;
	img.y = area.y + (area.h - img.h) / 2;
	frame = (SDL_Rect){ img.x - bw, img.y - bw, img.w + bw * 2, img.h + bw * 2 };

	ui_glow(a->r, &img, a->tint, 85, 1.35f);
	SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
	/* Opaque, matching the menu panel's border. At alpha 150 the frame let the
	 * background through and read as a darker accent than the system's own. The
	 * thumbnail itself is drawn at full brightness below - it never was
	 * darkened, only the frame around it.
	 *
	 * Rounded outside, square inside: the corner radius belongs to the chrome,
	 * and rounding the image would mean clipping the game's own pixels. */
	ui_round_rect(a->r, &frame, bw, (SDL_Color){
		(Uint8)(a->tint >> 16), (Uint8)(a->tint >> 8), (Uint8)a->tint, 255 });
	if (sv->thumb[sel]) {
		SDL_SetTextureColorMod(sv->thumb[sel], 255, 255, 255);
		SDL_RenderCopy(a->r, sv->thumb[sel], NULL, &img);
	} else {
		/* An empty slot is the same frame with nothing in it, so the strip
		 * does not change shape as you cycle past one. */
		SDL_SetRenderDrawColor(a->r, 12, 13, 18, 238);
		SDL_RenderFillRect(a->r, &img);
		ui_text(a->r, ui_font(UI_F_MENU), "Empty", img.x + img.w / 2,
		        img.y + (img.h - line_menu) / 2, 0, UI_TEXT_DIM);
	}

	if (sel == 0) snprintf(slotname, sizeof slotname, "Auto");
	else          snprintf(slotname, sizeof slotname, "Slot %d", sel);
	ui_text(a->r, ui_font(UI_F_MENU), slotname,
	        TORTOS_SCREEN_W / 2, img.y + img.h + 36, 0, UI_TEXT);
	ui_text(a->r, ui_font(UI_F_META),
	        sv->have[sel] ? sv->when[sel] : (sv->saving ? "\xE2\x80\x94" : ""),
	        TORTOS_SCREEN_W / 2, img.y + img.h + 42 + line_menu, 0, UI_TEXT_DIM);

	/* The dot rail: where you are among the slots, without showing every
	 * picture.
	 * A hollow-dim dot is a slot you cannot land on. */
	{
		int dots = GM_SLOTS + 1, dw = 36;
		int x0 = (TORTOS_SCREEN_W - dots * dw) / 2 + dw / 2;
		int y  = img.y + img.h + 62 + line_menu + line_meta;

		/* One size for every marker. Sizing the selected one larger meant the
		 * rail changed shape as you cycled, and color already says which slot
		 * you are on - two signals for one fact, one of them moving. */
		for (i = 0; i < dots; i++) {
			int can = sv->saving ? i >= 1 : sv->have[i];
			int r2  = 12;
			SDL_Rect d = { x0 + i * dw - r2, y - r2, r2 * 2, r2 * 2 };
			SDL_Color c = i == sel
				? (SDL_Color){ (Uint8)(a->tint >> 16), (Uint8)(a->tint >> 8),
				               (Uint8)a->tint, 255 }
				: (SDL_Color){ 90, 94, 110, can ? 255 : 90 };
			ui_round_rect(a->r, &d, r2 / 2, c);
		}
	}
}

/* Returns the chosen slot (SLOT_AUTO, or 1..GM_SLOTS) or 0 for backed out. */
static int slot_strip(app *a, SDL_Texture *bg, int saving)
{
	slot_view sv = { .aspect = 4.0f / 3.0f, .saving = saving };
	sysview *v = &a->view[a->sys_cursor];
	game_entry *g = &v->list.items[v->cursor];
	/* The game's own system, not the shelf: from Favorites the shelf is
	 * Favorites, which has no folder, and the slots came out loose in .tortos/
	 * - a second set the system's shelf never saw. Reported 2026-09-24. */
	int o = shelf_owner(a, a->sys_cursor, v->cursor);
	int i, sel = -1, chosen = 0, done = 0;
	bool   have_b[GM_SLOTS + 1] = { false };
	char pth[LIB_PATH * 2];

	for (i = 0; i <= GM_SLOTS; i++) {
		int slot = gm_slot_at(i);
		struct stat st;

		slot_state_path(a, o, g, slot, pth, sizeof pth);
		sv.have[i] = (stat(pth, &st) == 0 && st.st_size > 0);
		if (sv.have[i]) {
			have_b[i] = true;
			/* The state's own mtime: when this moment was captured. The
			 * device clock is only as good as the device clock, and showing
			 * what the filesystem says beats pretending to know better. */
			struct tm *tm = localtime(&st.st_mtime);
			if (tm) {
				/* "Aug 23 9:21:05 AM". Built from the fields rather than with
				 * strftime's "%-I", which drops the leading zero but is a GNU
				 * extension and does nothing on the BSD libc the host build
				 * links against - it would have read right on the device and
				 * wrong in every screenshot. The month still comes from
				 * strftime so it stays whatever the locale calls it. */
				char mon[8];
				int h12 = tm->tm_hour % 12;
				strftime(mon, sizeof mon, "%b", tm);
				if (!h12) h12 = 12;
				snprintf(sv.when[i], sizeof sv.when[i], "%s %d %d:%02d:%02d %s",
				         mon, tm->tm_mday, h12, tm->tm_min, tm->tm_sec,
				         tm->tm_hour < 12 ? "AM" : "PM");
			}

			slot_preview_path(a, o, g, slot, pth, sizeof pth);
			SDL_Surface *sf = IMG_Load(pth);
			if (sf) {
				sv.thumb[i] = SDL_CreateTextureFromSurface(a->r, sf);
				SDL_FreeSurface(sf);
			}
		}
		/* Loading starts on the newest thing there is to load, which is
		 * almost always Auto. Saving is decided after the loop, below,
		 * because it needs to know about every slot before it can choose. */
		if (sel < 0 && !saving && sv.have[i]) sel = i;
	}

	/* Save lands on the first empty slot, or the last one when there is none -
	 * the rule and its reasoning are in game_menu.h, where a check can reach
	 * them. */
	if (saving) sel = gm_save_slot(have_b);

	for (i = 0; i <= GM_SLOTS; i++) {
		int tw = 0, th = 0;
		if (!sv.thumb[i]) continue;
		SDL_QueryTexture(sv.thumb[i], NULL, NULL, &tw, &th);
		if (tw > 0 && th > 0) { sv.aspect = (float)tw / (float)th; break; }
	}
	if (sel < 0) sel = saving ? 1 : -1;
	if (sel < 0) done = -1;                     /* nothing to load at all */

	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);

	while (!done && !want_quit) {
		plat_input_poll(&a->in);

		if (in_repeat(&a->in, IN_LEFT) || in_repeat(&a->in, IN_RIGHT)) {
			int dir = in_repeat(&a->in, IN_RIGHT) ? 1 : -1, next = sel;
			do {
				next = (next + dir + GM_SLOTS + 1) % (GM_SLOTS + 1);
			} while ((saving ? next == 0 : !sv.have[next]) && next != sel);
			sel = next;
		}
		if (menu_leaving(a)) done = -1;
		/* SLOT_AUTO, not 9. Commit 4208619 renamed the resume slot from
		 * MinUI's number to `auto` and replaced it everywhere the constant
		 * was used - but here the old number was written out rather than
		 * referenced, so it survived the rename. Loading Auto from the menu
		 * has asked for a `.9.state` that has not existed since 2026-08-27,
		 * and every one of those was a state_rejected. */
		if (a->in.pressed[IN_ACCEPT]) { chosen = gm_slot_at(sel); done = 1; }
		/* This screen used to ignore the power button outright - the one
		 * screen in the launcher that did. Stop the game and close with
		 * nothing chosen; game_menu sees the flag and closes behind us. */
		{
			pwr_action pa = power_check(a);
			if (pa == PWR_POWEROFF) {
				plat_note_power_pressed();
				plat_resident_line("STOP");
				done = -1;
			}
		}

		SDL_SetRenderDrawColor(a->r, 0, 0, 0, 255);
		SDL_RenderClear(a->r);
		draw_paused_frame(a, bg);
		SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(a->r, 0, 0, 0, 185);
		SDL_RenderFillRect(a->r, NULL);

		slot_draw(a, &sv, sel);
		draw_battery(a->r);
		plat_present(a->r);
		SDL_Delay(8);
	}

	for (i = 0; i <= GM_SLOTS; i++)
		if (sv.thumb[i]) SDL_DestroyTexture(sv.thumb[i]);
	return done == 1 ? chosen : 0;
}

/* Change the mode, keep it, tell the running game, and take back the rect so
 * the backdrop behind the menu redraws where the game is about to be. */
static void gm_cycle_display(app *a, int d)
{
	/* The OWNER's, so a mode chosen while playing a favorite is set for the
	 * system that game belongs to - which is what a per-system mode means.
	 * Set on the Favorites shelf it would have been filed under FAV and worn
	 * by every game launched from there, whatever machine it came from. */
	sysview *v = owner_view(a);

	v->dmode = (v->dmode + d + DMODE_COUNT) % DMODE_COUNT;
	display_save(a);
	plat_resident_line("SETDISPLAY\tmode=%s", DMODES[v->dmode].name);
	plat_resident_sync_rect(150);
}

/* What the in-game menu needs from the app, handed to gm_rows to become rows.
 * Same split as everywhere else - see ADR-0001. */
static int gm_build(app *a, menu_row *out, gm_bufs *b)
{
	gm_ui u;

	/* The owner's, the mode gm_cycle_display changes: on Favorites the
	 * shelf's own is never set, so the label read Stretch whatever the
	 * game was playing at (plorpos-gkd.64). */
	u.dmode  = DMODES[owner_view(a)->dmode].label;
	u.shader  = a->shaders.e[owner_view(a)->shader].name;
	u.shaders = a->shaders.count > 1;
	{
		sysview *sv = &a->view[a->sys_cursor];
		int o = shelf_owner(a, a->sys_cursor, sv->cursor);
		int pal = palette_of(a, o, &sv->list.items[sv->cursor]);
		u.palette = pal < 0 ? NULL : gbpal_label(pal);
	}
	u.disc   = g_disc.count > 1 ? g_disc.label : NULL;
	u.earned = chv_earned();
	u.total  = chv_count();
	return gm_rows(&u, out, b);
}

/* The list itself, over the paused frame. menu_draw already windows a list
 * longer than the screen, which a set of 166 certainly is.
 *
 * Earned rows are drawn live and unearned quiet - the same distinction
 * menu_draw makes for a placeholder, and it reads correctly here: what you
 * have is bright, what is still ahead of you is not. */
/* 255 characters at ~30 to a line is nine, and a line of capitals is half
 * that. Twelve is past the worst real description with room over; the card
 * scrolls when the lines outrun the panel, so depth costs height rather
 * than text. */
#define CHV_WRAP_LINES 12
#define CHV_WRAP_COLS  96

/* Break `src` into lines no wider than `w`, at whitespace. Returns how many.
 *
 * menu_draw marquees a note that does not fit, which is right for a footer
 * carrying four facts and wrong for a sentence: the reader has to wait for the
 * text to come back around to re-read a clause. Three still lines beat one
 * moving one, so a description is wrapped here rather than handed over long.
 *
 * A word wider than the panel is cut instead of hunted for a space, because
 * the alternative is a line that overflows and there is nowhere else for it to
 * go. Measuring per prefix is quadratic in the line, which nobody will notice
 * at 191 characters, once, when the card opens.
 *
 * LINE BREAKS IN THE SOURCE ARE PARAGRAPHS, and this used to walk straight past
 * them: only ' ' broke a line and only ' ' was skipped, so a '\n' rode into the
 * middle of a line and SDL_ttf drew it as nothing - a hole two characters wide
 * after "Gunstar-9!!" with the next paragraph running on behind it. Of the 78
 * ScreenScraper synopses on this card 47 carry a break and 42 carry a blank
 * line; splitting at every break gives 191 pieces with a median of 276
 * characters, which is paragraphs rather than a source that hard-wraps. So any
 * run of whitespace containing a break ends the paragraph and gets one empty
 * row, however many blank lines the run actually held. Never at the top, where
 * it would only push the first line down off the rule. */
static int wrap_text(TTF_Font *f, const char *src, int w,
                     char out[][CHV_WRAP_COLS], int max)
{
	int n = 0;

	while (n < max) {
		char buf[CHV_WRAP_COLS];
		int take = 0, i;
		bool para = false;

		while (*src == ' ' || *src == '\n' || *src == '\r' || *src == '\t') {
			if (*src == '\n' || *src == '\r') para = true;
			src++;
		}
		if (!*src) break;
		if (para && n > 0) {
			out[n++][0] = '\0';
			if (n >= max) break;
		}
		for (i = 1; src[i - 1] && src[i - 1] != '\n' && src[i - 1] != '\r' &&
		            src[i - 1] != '\t'; i++) {
			if (i >= (int)sizeof buf) break;
			memcpy(buf, src, (size_t)i);
			buf[i] = '\0';
			if (ui_text_width(f, buf) > w) break;
			if (src[i] == ' ' || src[i] == '\n' || src[i] == '\r' ||
			    src[i] == '\t' || src[i] == '\0') take = i;
		}
		if (!take) {                    /* one word longer than the panel */
			take = i > 1 ? i - 1 : 1;
			if (take >= (int)sizeof buf) take = sizeof buf - 1;
		}
		memcpy(out[n], src, (size_t)take);
		out[n][take] = '\0';
		n++;
		src += take;
	}
	return n;
}

/* One achievement, opened with A from the list.
 *
 * The list has room for a title and a number, and a title is a NAME RATHER
 * THAN AN INSTRUCTION - it says which achievement, not what to do for it. The
 * description that says has been in memory the whole time, parsed by chv_add
 * beside the title and shown nowhere.
 *
 * Drawn with sel = -1, so nothing highlights. This is a card rather than a
 * list: there is nothing here to choose between, and a cursor would invite
 * pressing A on a row that does not answer.
 *
 * Returns true if the caller should close too, which is what MENU and power
 * mean anywhere else in the launcher. */
/* What the two achievement screens sit on.
 *
 * In a game that is the paused frame under a heavy dim, which is what makes a
 * menu read as being over something that has stopped. On the shelf it is the
 * shelf under the SAME lighter dim the game details screen uses, so opening
 * Cheevos from that screen does not darken the room on the way. */
static void chv_backdrop(app *a, SDL_Texture *bg, bool over_shelf)
{
	if (over_shelf) {
		draw_shelf(a);
	} else {
		SDL_SetRenderDrawColor(a->r, 0, 0, 0, 255);
		SDL_RenderClear(a->r);
		draw_paused_frame(a, bg);
	}
	SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(a->r, 0, 0, 0, over_shelf ? 120 : 185);
	SDL_RenderFillRect(a->r, NULL);
}

static bool cheevo_detail_screen(app *a, SDL_Texture *bg, bool over_shelf,
                                 const cheevo *c)
{
	char lines[CHV_WRAP_LINES][CHV_WRAP_COLS];
	menu_row rows[CHV_WRAP_LINES + 3];
	unsigned vcols[CHV_WRAP_LINES + 3] = { 0 };
	char pts[24];
	bool got = c->earned || c->earned_now;
	int fixed = menu_std_width(a);
	int n = 0, nl, i, done = 0;
	bool close_all = false;

	nl = wrap_text(ui_font(UI_F_MENU),
	               c->desc[0] ? c->desc : "The set carries no description.",
	               fixed, lines, CHV_WRAP_LINES);
	/* Live, unlike an ordinary note. A caption under a setting is a caption;
	 * here the description IS the content, and UI_TEXT_DIM is too quiet to be
	 * the only thing on the card worth reading. */
	for (i = 0; i < nl; i++)
		rows[n++] = (menu_row){ lines[i], MENU_NOTE_MARK, true };
	rows[n++] = MENU_RULE;
	snprintf(pts, sizeof pts, "%d", c->points);
	rows[n++] = (menu_row){ "Points", pts, true };
	rows[n]   = (menu_row){ "Status", got ? "Earned" : "Not earned", true };
	vcols[n]  = got ? UI_EARNED_RGB : 0u;
	n++;

	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);

	while (!done && !want_quit) {
		plat_input_poll(&a->in);

		if (a->in.pressed[IN_BACK] || a->in.pressed[IN_ACCEPT]) done = 1;
		if (a->in.pressed[IN_MENU]) { done = 1; close_all = true; g_menu_closing = true; }
		/* Only over the shelf. Over a game this card is the in-game menu's,
		 * and Muse would need that menu's power rule, which this screen does
		 * not have to give it. SELECT from the in-game menu itself works. */
		if (over_shelf && a->in.pressed[IN_SELECT]) muse_open(a, NULL, NULL);
		/* Same reasoning as the list below: this screen is not inside
		 * plat_resident_wait, so nothing else is watching power for it. Over
		 * the shelf there is no game to stop, and stopping one is not what
		 * power means there. */
		{
			pwr_action pa = power_check(a);
			if (pa == PWR_POWEROFF) {
				if (over_shelf) { power_off(a); break; }
				plat_note_power_pressed();
				plat_resident_line("STOP");
				done = 1;
				close_all = true;
			}
		}

		chv_backdrop(a, bg, over_shelf);
		menu_draw_ex(a, c->title, rows, n, -1, fixed, a->tint, vcols, false, 0);
		draw_battery(a->r);
		plat_present(a->r);
		SDL_Delay(8);
	}

	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);
	return close_all;
}

/* The synopsis, opened with A from the game info screen.
 *
 * The same shape as the achievement card above and for the same reason: there
 * is nothing here to choose between, so it is drawn with no cursor and scrolls
 * itself. A synopsis averages 670 characters and the longest on the test card
 * is 2,221 (BACKLOG item 27), so most of them fit and the long ones walk.
 *
 * Over the shelf rather than a paused frame: this is reached from the shelf,
 * where the info screen already dims the same way. */
#define SYN_WRAP_LINES 80
/* Room for the text twice over, plus the four blanks and the rule between the
 * copies. The rows are the wrapped lines POINTED AT twice, not wrapped twice,
 * so the line buffer stays SYN_WRAP_LINES - this is the only array that has to
 * grow, and 165 menu_rows is four kilobytes. */
#define SYN_ROWS (SYN_WRAP_LINES * 2 + 5)

/* The rows a synopsis is drawn as, and where they start repeating.
 *
 * Shared by the screen and by --synopsis, which used to build its own: the
 * harness then drew a card the device does not draw, which is the one thing a
 * harness must never do. Returns the row count; `rows` must hold SYN_ROWS. */
static int syn_layout(const char *text, int fixed,
                      char (*lines)[CHV_WRAP_COLS], menu_row *rows,
                      int *loop_at)
{
	int n, i, k;

	*loop_at = 0;
	n = wrap_text(ui_font(UI_F_MENU), text, fixed, lines, SYN_WRAP_LINES);
	for (i = 0; i < n; i++)
		rows[i] = (menu_row){ lines[i], MENU_BODY_MARK, true };

	/* A SYNOPSIS TOO TALL FOR THE PANEL IS LAID OUT TWICE, with two blank
	 * lines, a rule and two blank lines between the copies, and menu_draw is
	 * told where the repeat starts.
	 *
	 * Both spacings were looked at on the device 2026-09-17. The rule carries
	 * air of its own - it is the bar the heading uses, with the heading's gap
	 * under it - and on its own it reads as a line ruled THROUGH the prose,
	 * with the last sentence and the first pressed against it. The blank lines
	 * are what make it the end of something.
	 *
	 * That is what turns the scroll from a ping-pong into a scroll-through.
	 * Running backwards is fine for a row of text too wide for its column,
	 * where the rewind is obviously a rewind; over fifteen lines of prose it
	 * means reading the end in reverse to get back to the beginning. Coming
	 * round instead needs somewhere to come round FROM, which is the rule: it
	 * says the text ended and is starting again, rather than leaving a reader
	 * to work out that the sentence they are on is one they have read.
	 *
	 * Only when it does not fit. A short synopsis sits still, and doubling it
	 * would make a page that fits scroll for no reason. */
	if (n <= menu_notes_fit()) return n;

	k = n;
	rows[k++] = MENU_BODY("");
	rows[k++] = MENU_BODY("");
	rows[k++] = MENU_HR;
	rows[k++] = MENU_BODY("");
	rows[k++] = MENU_BODY("");
	*loop_at = k;
	for (i = 0; i < n && k < SYN_ROWS; i++)
		rows[k++] = (menu_row){ lines[i], MENU_BODY_MARK, true };
	return k;
}

/* ---- Muse: covers --------------------------------------------------------- */

static bool file_nonempty(const char *p)
{
	struct stat st;

	return stat(p, &st) == 0 && st.st_size > 0;
}

/* Album `al`'s cover, when there is one on the card: true, with where in
 * `out`.
 *
 * The first time an album is asked about, a cover already on the card - from
 * an earlier visit - is found by looking. Otherwise the daemon is asked for the
 * one the music carries, and this answers false until cover_answers hears
 * back, a few milliseconds later. */
static bool cover_file(int al, char *out, size_t n)
{
	char base[LIB_PATH * 2];
	cover_state *c;
	unsigned now = plat_now_ms();

	if (!g_cov || al < 0 || al >= g_muse.nalbums) return false;
	c = &g_cov[al];
	ml_cover_base(g_muse_root, &g_muse, al, base, sizeof base);
	if (!base[0]) return false;
	if (c->st == COV_JPG || c->st == COV_PNG) {
		snprintf(out, n, "%s.%s", base, c->st == COV_JPG ? "jpg" : "png");
		return true;
	}
	if (c->st == COV_FOLDER) return ml_folder_image(g_muse_root, &g_muse, al, out, n);
	if (c->st == COV_NONE) return false;
	if (c->st == COV_ASKED && now - c->asked_ms < 5000) return false;

	snprintf(out, n, "%s.jpg", base);
	if (file_nonempty(out)) { c->st = COV_JPG; return true; }
	snprintf(out, n, "%s.png", base);
	if (file_nonempty(out)) { c->st = COV_PNG; return true; }
	if (musec_cover_ask(g_muse.tracks[g_muse.albums[al].first].path, base)) {
		c->st = COV_ASKED;
		c->asked_ms = now;
	}
	return false;
}

/* What the daemon said about the covers it was asked for. Matched by path, not
 * by album number, so an answer that lands after a rescan finds the album it
 * was about or none at all. True when anything landed, so a shelf that only
 * draws when something changes knows it has. */
static bool cover_answers(void)
{
	char base[LIB_PATH * 2], file[LIB_PATH * 2 + 8], b[LIB_PATH * 2];
	bool landed = false;
	size_t fl;
	int i;

	while (musec_cover_take(base, sizeof base, file, sizeof file)) {
		landed = true;
		fl = strlen(file);
		for (i = 0; g_cov && i < g_muse.nalbums; i++) {
			char pic[LIB_PATH * 2 + 8];

			ml_cover_base(g_muse_root, &g_muse, i, b, sizeof b);
			if (strcmp(b, base)) continue;
			/* Nothing in the files: a picture in the album's folder, which
			 * is where a book's cover usually is, before the card. */
			g_cov[i].st = fl > 0 ? (fl > 4 && !strcmp(file + fl - 4, ".png") ? COV_PNG : COV_JPG)
			            : ml_folder_image(g_muse_root, &g_muse, i, pic, sizeof pic)
			            ? COV_FOLDER : COV_NONE;
			break;
		}
	}
	return landed;
}

/* A cover's shape, wherever it is drawn: square, and square-cornered - Eric's
 * call, 2026-09-18; a sleeve is a square. An ARGB8888 surface, replaced when
 * it had to be cropped.
 *
 * Square by taking the middle of it. Covers are square nearly always, and the
 * ones that are not are a scan with a few pixels of scanner either side - the
 * first card has one, 245 by 225 - which cropping loses and stretching would
 * put into the picture. */
static void cover_shape(SDL_Surface **ps)
{
	SDL_Surface *s = *ps, *sq;
	int side = s->w < s->h ? s->w : s->h;

	if (s->w != s->h &&
	    (sq = SDL_CreateRGBSurfaceWithFormat(0, side, side, 32,
	                                         SDL_PIXELFORMAT_ARGB8888))) {
		SDL_SetSurfaceBlendMode(s, SDL_BLENDMODE_NONE);
		SDL_BlitSurface(s, &(SDL_Rect){ (s->w - side) / 2, (s->h - side) / 2,
		                                side, side }, sq, NULL);
		SDL_FreeSurface(s);
		*ps = sq;
	}
}

/* A cover as Now Playing draws it. NULL when the file will not decode. */
static SDL_Texture *load_cover(SDL_Renderer *r, const char *path)
{
	SDL_Surface *raw = IMG_Load(path), *s;
	SDL_Texture *t;

	if (!raw) return NULL;
	s = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_ARGB8888, 0);
	SDL_FreeSurface(raw);
	if (!s) return NULL;
	cover_shape(&s);
	t = SDL_CreateTextureFromSurface(r, s);
	SDL_FreeSurface(s);
	if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
	return t;
}

/* Bring the Now Playing cover up to date with album `al`, which may be -1 for
 * nothing. Called every frame; it costs a comparison once the cover is in. */
static void np_cover(app *a, int al)
{
	char path[LIB_PATH * 2 + 8];
	int w, h;

	if (al != g_np.album) { np_forget(); g_np.album = al; }
	if (g_np.done || al < 0) return;
	if (cover_file(al, path, sizeof path)) {
		unsigned t0 = plat_now_ms();

		g_np.tex = load_cover(a->r, path);
		fprintf(stderr, "muse: cover for %s in %u ms\n", g_muse.albums[al].name,
		        plat_now_ms() - t0);
		/* There, and not a picture: the card, as for an album with none. */
		if (!g_np.tex) g_cov[al].st = COV_NONE;
	}
	if (!g_np.tex && g_cov[al].st == COV_NONE) {
		g_np.tex = ui_make_cover(a->r, g_muse.albums[al].name, MUSE_ACCENT, &w, &h);
	}
	g_np.done = g_np.tex != NULL;
}

/* Card `i` on Muse's shelf - game_get_tex's half for Muse. Its album's cover
 * when the card holds one, decoded on the worker like any box art; the
 * generated card when the music carries none; nothing yet while the daemon is
 * being asked, which cover_answers settles a few milliseconds later. */
static void muse_card_want(app *a, int s, int i)
{
	sysview *v = &a->view[s];
	char path[LIB_PATH * 2 + 8];
	int al;

	if (!v->album || i < 0 || i >= v->list.count) return;
	al = v->album[i];
	if (al < 0 || al >= g_muse.nalbums) return;
	if (cover_file(al, path, sizeof path)) {
		if (texload_want(s, i, path, NULL)) return;
		/* No worker - a shot - so here, as game_get_tex does without one. */
		v->tex[i] = load_cover(a->r, path);
		if (v->tex[i]) {
			SDL_QueryTexture(v->tex[i], NULL, NULL, &v->tw[i], &v->th[i]);
			v->cb[i] = 1.0f;
			return;
		}
		g_cov[al].st = COV_NONE;             /* there, and not a picture */
	}
	if (g_cov && g_cov[al].st == COV_NONE) {
		v->tex[i] = ui_make_cover(a->r, v->list.items[i].title, MUSE_ACCENT,
		                          &v->tw[i], &v->th[i]);
		v->cb[i] = 1.0f;
	}
}

/* ---- Muse: the play mode --------------------------------------------------- */

/* Y ON NOW PLAYING, AND NOWHERE ELSE IN MUSE. Eric's call, 2026-09-19.
 *
 * Y used to cycle the mode on every Muse screen, and because a mark on its own
 * is a new shape to learn, each press named the mode on a pill in the middle
 * of the screen for 1400 ms. On Now Playing that pill said what the mark
 * beside the track count was already saying, a moment later and larger.
 *
 * The mode is now set where it is shown. That leaves the shelf and the lists
 * with nothing to announce, so the pill is gone with them, and Y is free there
 * the way X is. The cost is that the mode cannot be changed before something
 * plays, since Now Playing is a screen about a track - and it is kept across
 * restarts, so it is not a thing anyone sets often. */
static void muse_cycle_mode(void)
{
	muq_mode m = (muq_mode)((musec_mode() + 1) % MUQ_MODES);

	musec_set_mode(m);
	db_set_str(db_dev(), "muse.mode", MUSE_MODES[m].key);
}

/* ---- Muse: what it is open over ------------------------------------------ */

/* NULL is the shelf. The in-game menu hands over its style for as long as Muse
 * is up, because a game is paused underneath: the list is drawn over that
 * game's frame rather than a shelf it is not on, and power stops the game
 * rather than the device - which is what that menu's power rule does, and why
 * it has one. Muse is modal and never opens itself, so one of each is enough. */
static const menu_style *g_muse_over;
static void             *g_muse_over_ctx;
static bool              g_muse_gone;    /* that rule ended what was underneath */

static void muse_backdrop(app *a)
{
	draw_shelf(a);
	SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(a->r, 0, 0, 0, 120);
	SDL_RenderFillRect(a->r, NULL);
}

/* The power button or Auto Off, inside Muse. It closes Muse either way; what
 * else it does is up to whatever Muse is open over. */
static void muse_power(app *a)
{
	if (g_muse_over && g_muse_over->on_power) {
		if (g_muse_over->on_power(a, g_muse_over_ctx) == MENU_DONE)
			g_muse_gone = true;
		return;
	}
	power_off(a);
}

/* ---- Muse: Now Playing ---------------------------------------------------- */

/* How far one step of left or right seeks: ten seconds a tap, and further the
 * longer it is held - a minute a step after a second, five after three.
 * Eric's, 2026-09-29, for books: at ten seconds a step, eleven steps a second,
 * crossing a seventeen-hour book took nine minutes of holding the d-pad, and
 * now takes about twenty seconds. A song is over long before the steps grow,
 * so music is as it was. */
static double seek_step(const in_state *in, in_button b)
{
	Uint32 held = SDL_GetTicks() - in->down_since[b];

	if (in->pressed[b] || held < 1000) return 10;
	return held < 3000 ? 60 : 300;
}

static void mmss(char *out, size_t n, double sec)
{
	int t = sec > 0 ? (int)(sec + 0.5) : 0;

	/* Hours past the hour: a seventeen-hour book read "-1014:46" in minutes. */
	if (t >= 3600) snprintf(out, n, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
	else           snprintf(out, n, "%d:%02d", t / 60, t % 60);
}

/* The cover where the shelf puts the card it is looking at, in the same kind
 * of light, and the words beside it. Its own screen and not a panel over the
 * shelf: it is the one screen left open for as long as an album lasts, and
 * what is behind a panel is a picture of games. */
#define NP_SIDE 432                              /* the cover, on screen */
#define NP_X    80
#define NP_Y    ((TORTOS_SCREEN_H - NP_SIDE) / 2 - 28)
#define NP_TX   (NP_X + NP_SIDE + 56)            /* the words beside it */
#define NP_TW   (TORTOS_SCREEN_W - NP_TX - 64)

/* A chapter as Now Playing names it: its own title when it has one worth
 * reading, "Chapter 3" when the title is only its number - Dungeon Crawler
 * Carl's thirty-seven are "001" to "037" - or missing. */
static void chapter_label(int i, char *out, size_t n)
{
	const char *t = musec_chapter_title(i);
	const char *p;

	for (p = t; *p && (isdigit((unsigned char)*p) || *p == ' '); p++) { }
	if (t[0] && *p) snprintf(out, n, "%s", t);
	else            snprintf(out, n, "Chapter %d", i + 1);
}

/* One line of the words, sliding when it is wider than the column. */
static void np_line(SDL_Renderer *r, TTF_Font *f, const char *s, int y,
                    unsigned phase, SDL_Color col, unsigned *wait)
{
	unsigned w = ui_pingpong_wait(ui_text_width(f, s) - NP_TW, phase);

	ui_text_marquee(r, f, s, NP_TX, y, NP_TW, phase, col);
	if (w < *wait) *wait = w;
}

/* Returns how long until the picture would change by itself, in ms - 0 while
 * a line is sliding - so the loop can leave the screen alone until then. */
static unsigned np_draw(app *a, const mu_now *mn, const char *next, bool lock)
{
	SDL_Renderer *r = a->r;
	SDL_Rect cov = { NP_X, NP_Y, NP_SIDE, NP_SIDE };
	TTF_Font *fs = ui_font(UI_F_META);
	/* Muse's green for everything that is not the cover or the words - the
	 * light, the bar, the mode's mark - the same green its shelf card and its
	 * menus wear. It was the cover's own most vivid color once, which made one
	 * mark two colors a screen apart; Eric's call, 2026-09-19. */
	unsigned rgb = MUSE_ACCENT, wait = (unsigned)-1, phase;
	SDL_Color acc = { (Uint8)(rgb >> 16), (Uint8)(rgb >> 8), (Uint8)rgb, 255 };
	const char *state = mn->state == MU_PLAYING ? NULL
	                  : mn->state == MU_PAUSED  ? "Paused" : "Stopped";
	int book = musec_is_book() ? ml_album_of(&g_muse, musec_track(mn->index)) : -1;
	double k = mn->len > 0 ? mn->at / mn->len : 0;
	char t0[16], t1[16], line[200], fit[200];
	int y, bar, fill, left, right;

	/* A book played to its end says so, rather than that it stopped. */
	if (mn->state == MU_STOPPED && book >= 0 && g_book_done && g_book_done[book])
		state = "Finished";

	SDL_SetRenderDrawColor(r, UI_BG_R, UI_BG_G, UI_BG_B, 255);
	SDL_RenderClear(r);
	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
	/* The shelf's wash along the foot, and the focused card's glow around
	 * the cover. */
	ui_glow(r, &(SDL_Rect){ 0, TORTOS_SCREEN_H - 240, TORTOS_SCREEN_W, 480 },
	        rgb, 34, 1.7f);
	ui_glow(r, &cov, rgb, 90, 1.7f);
	if (g_np.tex) SDL_RenderCopy(r, g_np.tex, NULL, &cov);

	/* Title, artist and album slide together when they are too long, on one
	 * clock that starts over with each track. */
	phase = mq_phase(MQ_NP, mn->index, (int)strlen(mn->title));
	y = NP_Y + 36;
	{
		/* Where in the queue, and the mode it is playing in beside it. The
		 * count is the order being HEARD, so shuffled it still runs 1 to n.
		 *
		 * The mark half again the text's height, centered on it: at the
		 * text's own height the 1 in repeat one and shuffle's crossing were
		 * too small to read at arm's length. Eric, 2026-09-19.
		 *
		 * Centered on the ink, not the em box: the box keeps a descender's
		 * depth below the baseline that "1 of 12" never uses, so its middle
		 * sat the mark 3.5px low on the device. Cap height from 'H', as the
		 * Wi-Fi panel does. */
		int th = ui_font_height(UI_F_META), gs = th * 3 / 2, gx = NP_TX;
		/* No mark on a book: it plays in order, whatever the mode. */
		int g = musec_is_book() ? -1 : MUSE_MODES[musec_mode()].glyph;
		int asc = fs ? ui_font_ascent(fs) : th;
		int cap = fs ? ui_font_cap(fs) : asc;
		/* A book with chapters counts those instead of its one file. */
		if (musec_is_book() && musec_chapters() > 1) {
			snprintf(line, sizeof line, "Chapter %d of %d",
			         musec_chapter_now() + 1, musec_chapters());
			gx += ui_text(r, fs, line, NP_TX, y, -1, UI_TEXT_DIM) + 18;
		} else if (mn->count > 1) {
			snprintf(line, sizeof line, "%d of %d", mn->index + 1, mn->count);
			/* Not tabular: it changes with the track, not every second,
			 * and a 1 in a fixed-width cell read as "1 of  14". */
			gx += ui_text(r, fs, line, NP_TX, y, -1, UI_TEXT_DIM) + 18;
		}
		if (g >= 0) {
			ui_glyph_draw(r, (ui_glyph)g, gx + gs / 2, y + asc - cap / 2, gs, acc);
			gx += gs + 12;
		}
		/* A book's speed where music's mode mark would be, and only when it
		 * is not the ordinary one. */
		if (musec_is_book() && fabs(musec_speed() - 1.0) > 0.01) {
			snprintf(line, sizeof line, "%gx", musec_speed());
			ui_text(r, fs, line, gx, y, -1, acc);
			gx += ui_text_width(fs, line) + 12;
		}
		/* The Mute Switch down in muse button lock: what the buttons will
		 * do once the screen goes dark, which is nothing. Only while it is
		 * down, as an iPod shows its hold. TortOS-7cv. */
		if (lock) ui_glyph_draw(r, UI_GLYPH_LOCK, gx + gs / 2, y + asc - cap / 2, gs, acc);
	}
	/* Room under it for the larger mark and for the title to stand clear of
	 * the line above - which read as one block with it at 6px. */
	y += ui_font_line(UI_F_META) + 24;
	{
		/* A chapter with a title of its own is what is playing; one that
		 * is only a number leaves the book's title there. */
		char chap[128];
		const char *top = mn->title;

		if (musec_is_book() && musec_chapters() > 1) {
			chapter_label(musec_chapter_now() < 0 ? 0 : musec_chapter_now(), chap, sizeof chap);
			if (strncmp(chap, "Chapter ", 8) != 0) top = chap;
		}
		np_line(r, ui_font(UI_F_TITLE), top, y, phase, UI_TEXT, &wait);
	}
	y += ui_font_line(UI_F_TITLE) + 10;
	np_line(r, ui_font(UI_F_MENU), mn->artist, y, phase, UI_TEXT_SOFT, &wait);
	y += ui_font_line(UI_F_MENU);
	np_line(r, ui_font(UI_F_MENU), mn->album, y, phase, UI_TEXT_DIM, &wait);

	/* Where in the track. What is left rather than the whole length on the
	 * right, because the question is usually how long until the next one. */
	bar = NP_Y + NP_SIDE - 132;
	ui_round_rect(r, &(SDL_Rect){ NP_TX, bar, NP_TW, UI_BAR_H }, UI_BAR_H / 2,
	              (SDL_Color){ 255, 255, 255, 34 });
	fill = (int)(NP_TW * (k < 0 ? 0 : k > 1 ? 1 : k) + 0.5);
	if (fill > 0)
		ui_round_rect(r, &(SDL_Rect){ NP_TX, bar, fill, UI_BAR_H }, UI_BAR_H / 2,
		              state ? ui_fade(acc, 0.45f) : acc);
	/* Tabular, so the clock ticking does not move what sits beside it. */
	mmss(t0, sizeof t0, mn->at);
	left = NP_TX + ui_text_tabular(r, fs, t0, NP_TX, bar + 16, -1, UI_TEXT_SOFT);
	right = NP_TX + NP_TW;
	if (mn->len > 0) {
		mmss(t1, sizeof t1, mn->len > mn->at ? mn->len - mn->at : 0);
		snprintf(line, sizeof line, "-%s", t1);
		right -= ui_text_tabular(r, fs, line, NP_TX + NP_TW, bar + 16, 1, UI_TEXT_SOFT);
	}
	/* Between the two times rather than on the column's middle, which a book's
	 * hours pushed them into: "16:54:47Stopped". */
	if (state) ui_text(r, fs, state, (left + right) / 2, bar + 16, 0, acc);

	if (next && next[0]) {
		snprintf(line, sizeof line, "Next: %s", next);
		ui_fit_text(fs, line, fit, sizeof fit, NP_TW);
		ui_text(r, fs, fit, NP_TX, NP_Y + NP_SIDE - ui_font_height(UI_F_META), -1,
		        UI_TEXT_DIM);
	}

	ui_text(r, fs, musec_is_book()
	        ? (musec_chapters() > 1
	           ? (state ? "A: play    L1/R1: chapter    Left/Right: seek    Y: speed"
	                    : "A: pause    L1/R1: chapter    Left/Right: seek    Y: speed")
	           : (state ? "A: play    L1/R1: file    Left/Right: seek    Y: speed"
	                    : "A: pause    L1/R1: file    Left/Right: seek    Y: speed"))
	        : (state ? "A: play    L1/R1: track    Left/Right: seek    Y: mode"
	                 : "A: pause    L1/R1: track    Left/Right: seek    Y: mode"),
	        TORTOS_SCREEN_W / 2, TORTOS_SCREEN_H - 72, 0, UI_TEXT_DIM);
	draw_battery(r);
	return wait;
}

/* MENU, from any Muse screen: Muse's own menu, the one its shelf has always
 * had. One button, one meaning, wherever in Muse it is pressed - it used to
 * close Muse from Now Playing and the tracks, and open this on the shelf.
 * Eric's call, 2026-09-23.
 *
 * Rescan Folder inside it rebuilds the library, and every album index with it,
 * so a screen holding one cannot carry on: MUSE_REBUILT tells it to let go,
 * and the shelf finds itself again the way it does after its own MENU. */
static muse_exit muse_menu(app *a)
{
	unsigned gen = g_muse_gen;

	tortos_menu_for(a, SCREEN_GAMES);
	/* MENU inside it closed it, and only it: Muse is still up, and may itself
	 * be over a menu SELECT opened it from, which has to stay open. */
	g_menu_closing = false;
	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);
	if (want_quit || !a->running) return MUSE_CLOSE;
	return g_muse_gen != gen ? MUSE_REBUILT : MUSE_BACK;
}

/* MUSE_CLOSE for SELECT and the power button, which leave Muse altogether;
 * MUSE_BACK for B, which is the album's tracks underneath; MUSE_REBUILT when
 * MENU's Rescan Folder replaced the library. MENU otherwise comes back here. */
static muse_exit muse_now_screen(app *a)
{
	struct {
		mu_state st;
		int at, len, index, count, mode, chapter, speed;
		bool lock;
		char title[128], artist[128], album[128];
		SDL_Texture *tex;
	} shown, drawn;
	unsigned due = 0;
	muse_exit how = MUSE_BACK;

	mq_reset(MQ_NP);
	memset(&drawn, 0, sizeof drawn);
	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);

	while (!want_quit && a->running) {
		const mu_now *mn;
		const char *nt;
		char next[128] = "";
		unsigned now;
		bool touched = false;
		int b;

		muse_screen_poll();
		cover_answers();
		mn = musec_now();
		plat_input_poll(&a->in);
		if (a->in.quit_requested) { a->running = false; how = MUSE_CLOSE; break; }
		{
			pwr_action pa = power_check(a);
			if (pa == PWR_POWEROFF) { muse_power(a); how = MUSE_CLOSE; break; }
		}
		if (a->in.pressed[IN_BACK]) break;
		if (a->in.pressed[IN_SELECT]) { how = MUSE_CLOSE; break; }
		if (a->in.pressed[IN_MENU]) {
			how = muse_menu(a);
			if (how != MUSE_BACK) break;
			memset(&drawn, 0, sizeof drawn);     /* the menu drew over it */
			continue;
		}

		/* A, and only A. X did the same thing here and pausing in the list,
		 * which made one button mean two things a screen apart; it is left
		 * free in Muse for something that needs it. Eric's call, 2026-09-19. */
		if (a->in.pressed[IN_ACCEPT])       musec_toggle();
		/* The mode, on music; on a book, which plays in order whatever the
		 * mode says, its speed. */
		if (a->in.pressed[IN_Y]) {
			if (musec_is_book()) book_cycle_speed();
			else                 muse_cycle_mode();
		}
		if (in_repeat(&a->in, IN_L1))       musec_prev();
		if (in_repeat(&a->in, IN_R1))       musec_next();
		if (in_repeat(&a->in, IN_LEFT))     musec_seek_by(-seek_step(&a->in, IN_LEFT));
		if (in_repeat(&a->in, IN_RIGHT))    musec_seek_by(+seek_step(&a->in, IN_RIGHT));
		if (in_repeat(&a->in, IN_VOLUP))    plat_volume_nudge(+1);
		if (in_repeat(&a->in, IN_VOLDN))    plat_volume_nudge(-1);
		if (in_repeat(&a->in, IN_BRIGHTUP)) plat_brightness_nudge(+1);
		if (in_repeat(&a->in, IN_BRIGHTDN)) plat_brightness_nudge(-1);

		/* The queue's track, not musec_path: that goes blank when an album
		 * ends, and the screen should still show the album that just did. */
		np_cover(a, ml_album_of(&g_muse, musec_track(mn->index)));
		/* What the mode will actually play next. Repeat one says so with
		 * its mark, and "Next:" naming the same song reads as a mistake. */
		/* In a book with chapters, the next chapter, until the last. */
		if (musec_is_book() && musec_chapters() > 1 &&
		    musec_chapter_now() + 1 < musec_chapters())
			chapter_label(musec_chapter_now() + 1, next, sizeof next);
		else if ((musec_is_book() || musec_mode() != MUQ_REPEAT_ONE) && (nt = musec_upcoming()))
			ml_track_name(strrchr(nt, '/') ? strrchr(nt, '/') + 1 : nt,
			              next, sizeof next);

		/* Drawn when what it shows has changed, not every pass. This is the
		 * screen left open for the length of an album, and a present a frame
		 * for forty minutes of one still picture is battery spent on nothing.
		 * The clock is kept in whole seconds, which is all it shows. */
		memset(&shown, 0, sizeof shown);
		shown.st = mn->state;
		shown.at = (int)(mn->at + 0.5);
		shown.len = (int)(mn->len + 0.5);
		shown.index = mn->index;
		shown.count = mn->count;
		memcpy(shown.title, mn->title, sizeof shown.title);
		memcpy(shown.artist, mn->artist, sizeof shown.artist);
		memcpy(shown.album, mn->album, sizeof shown.album);
		shown.tex = g_np.tex;
		shown.mode = musec_is_book() ? -1 : (int)musec_mode();
		shown.lock = plat_hold_switch();
		shown.chapter = musec_chapter_now();
		shown.speed = (int)(musec_speed() * 100 + 0.5);
		for (b = 0; b < IN_COUNT && !touched; b++)
			touched = a->in.pressed[b] || a->in.down[b];
		now = plat_now_ms();

		if (touched || memcmp(&shown, &drawn, sizeof shown) || (int)(now - due) >= 0) {
			unsigned wait = np_draw(a, mn, next, shown.lock);
			Uint32 osd;

			/* Asked after the draw, which is what retires a line whose
			 * time is up - asked before, it names a moment already past. */
			draw_chrome(a->r);
			osd = plat_osd_until();
			plat_present(a->r);
			drawn = shown;
			/* At least once a second whatever happens, which is also how
			 * often the clock changes. The volume line needs one more frame
			 * at the moment it is due to go. */
			due = now + (wait < 1000 ? wait : 1000);
			if (osd != UINT32_MAX && (int)(osd - due) < 0) due = osd;
			SDL_Delay(8);
		} else {
			SDL_Delay(16);
		}
	}

	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);
	return how;
}

/* ---- Muse: an album's tracks ---------------------------------------------- */

/* The album the queue is on, and the row its track sits at. -1 when nothing
 * is loaded, or when what is loaded is not on the card any more. */
static int muse_playing_album(int *row)
{
	const char *t = musec_track(musec_now()->index);
	int al = ml_album_of(&g_muse, t), i;

	if (row) *row = 0;
	if (al < 0) return -1;
	if (row)
		for (i = 0; i < g_muse.albums[al].n; i++)
			if (!strcmp(g_muse.tracks[g_muse.albums[al].first + i].path, t))
				*row = i;
	return al;
}

/* One album's tracks, over the shelf its cover sits on.
 *
 * THIS USED TO BE THE WHOLE OF MUSE: artists, then their albums, then tracks,
 * with Now Playing as a row at the top, because the covers it browsed by had
 * no shelf of their own yet. They have one now - see muse_shelf_screen - and
 * SELECT lands there, so the two levels above this one had nothing left to
 * do. Eric's, 2026-09-20: the rest of the launcher is pictures, and a list
 * was the wrong front door for the one screen that is all covers.
 *
 * A plays a track and opens Now Playing; on the track already playing it just
 * opens Now Playing, rather than starting it over. B goes back to the shelf.
 * SELECT closes Muse from here, and MENU is Muse's menu, as everywhere in Muse.
 * L1 and R1 are the previous and next track, left and right seek ten seconds.
 * X and Y are free. Leaving does not stop the music - that is what the daemon
 * is for.
 *
 * `now` opens straight onto Now Playing, with these tracks underneath, so B
 * from there lands on the album it is playing. Returns how it was left, so
 * the shelf can close too when SELECT closed Muse - it used to be told
 * nothing, and SELECT from Now Playing landed on the shelf instead. */
static muse_exit muse_tracks(app *a, int album, bool now)
{
	int sel = 0, done = 0, art, i;
	muse_exit how = MUSE_BACK;
	menu_row *rows;
	char (*vals)[24];
	char heading[300], note[300], t0[16], t1[16];
	const ml_album *al;

	if (album < 0 || album >= g_muse.nalbums || g_muse.nartists == 0) return MUSE_BACK;
	al = &g_muse.albums[album];
	art = muse_artist_of(album);
	rows = calloc((size_t)al->n + 4, sizeof *rows);
	vals = calloc((size_t)al->n + 4, sizeof *vals);
	if (!rows || !vals) { free(rows); free(vals); return MUSE_BACK; }

	/* On the track that is playing, when it is one of these. */
	if (muse_playing_album(&i) == album) sel = i;

	if (now) {
		how = muse_now_screen(a);
		if (how != MUSE_BACK) done = 1;
		else if (muse_playing_album(&i) == album) sel = i;
	}

	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);

	while (!done && !want_quit && a->running) {
		const mu_now *mn;
		bool loaded, open_now = false;
		int n = 0, items;
		const char *playing;

		muse_screen_poll();
		cover_answers();
		mn = musec_now();
		loaded = mn->state == MU_PLAYING || mn->state == MU_PAUSED;
		playing = musec_path();
		plat_input_poll(&a->in);

		snprintf(heading, sizeof heading, "%s\n%s", al->name,
		         g_muse.artists[art].name);
		for (i = 0; i < al->n; i++, n++) {
			const ml_track *t = &g_muse.tracks[al->first + i];
			/* Every row a value, if only an empty one: a list with a value
			 * anywhere is laid out in two columns, left-aligned, and one
			 * without is centered. Without this the list sat centered until
			 * one of its tracks played and then jumped left, every name
			 * moving the moment A was pressed. Eric, 2026-09-19. */
			const char *v = "";

			if (playing[0] && !strcmp(playing, t->path))
				v = mn->state == MU_PAUSED ? "paused" : "playing";
			rows[n] = (menu_row){ t->name, v, true };
		}
		items = n;
		if (sel >= items) sel = items ? items - 1 : 0;

		/* What is playing, under a rule, even when it is another album's -
		 * the list is a way to reach music, and the music is the point. */
		if (loaded) {
			mmss(t0, sizeof t0, mn->at);
			mmss(t1, sizeof t1, mn->len);
			snprintf(note, sizeof note, "%s%s  -  %s   %s / %s",
			         mn->state == MU_PAUSED ? "Paused: " : "", mn->title,
			         mn->artist[0] ? mn->artist : mn->album, t0, t1);
			rows[n++] = MENU_RULE;
			rows[n++] = MENU_NOTE(note);
		}

		if (items > 0) {
			if (in_repeat(&a->in, IN_UP))   sel = (sel + items - 1) % items;
			if (in_repeat(&a->in, IN_DOWN)) sel = (sel + 1) % items;
		}
		if (in_repeat(&a->in, IN_LEFT))     musec_seek_by(-seek_step(&a->in, IN_LEFT));
		if (in_repeat(&a->in, IN_RIGHT))    musec_seek_by(+seek_step(&a->in, IN_RIGHT));
		if (in_repeat(&a->in, IN_L1))       musec_prev();
		if (in_repeat(&a->in, IN_R1))       musec_next();
		if (in_repeat(&a->in, IN_VOLUP))    plat_volume_nudge(+1);
		if (in_repeat(&a->in, IN_VOLDN))    plat_volume_nudge(-1);
		if (in_repeat(&a->in, IN_BRIGHTUP)) plat_brightness_nudge(+1);
		if (in_repeat(&a->in, IN_BRIGHTDN)) plat_brightness_nudge(-1);

		if (a->in.pressed[IN_ACCEPT] && items > 0) {
			/* The track already playing is not started over: A on it is the
			 * way to its Now Playing, the same as the row. */
			if (!loaded || strcmp(playing, g_muse.tracks[al->first + sel].path)) {
				const char **paths = calloc((size_t)al->n, sizeof *paths);

				if (paths) {
					for (i = 0; i < al->n; i++)
						paths[i] = g_muse.tracks[al->first + i].path;
					/* A book's file from its start: choosing one is choosing
					 * where to listen from, and the place moves with it. */
					muse_play(paths, al->n, sel, 0, al->book,
					          al->book ? book_speed(album) : 1.0,
					          g_muse.artists[art].name, al->name);
					free(paths);
				}
			}
			open_now = true;
		}
		if (a->in.pressed[IN_BACK]) done = 1;
		if (a->in.pressed[IN_SELECT]) { how = MUSE_CLOSE; break; }
		{
			pwr_action pa = power_check(a);
			if (pa == PWR_POWEROFF) { muse_power(a); how = MUSE_CLOSE; break; }
		}
		/* After a rescan `al` is gone with the old library: not one more
		 * frame from it. */
		if (a->in.pressed[IN_MENU]) {
			how = muse_menu(a);
			if (how != MUSE_BACK) break;
			continue;
		}

		/* Back from Now Playing onto the track it had reached, which may be
		 * several past the one chosen. */
		if (open_now) {
			how = muse_now_screen(a);
			if (how != MUSE_BACK) break;
			if (muse_playing_album(&i) == album) sel = i;
			continue;
		}

		muse_backdrop(a);
		menu_draw(a, heading, rows, n, sel, menu_std_width(a), MUSE_ACCENT);
		draw_chrome(a->r);
		plat_present(a->r);
		SDL_Delay(8);
	}

	free(rows);
	free(vals);
	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);
	return how;
}

/* SELECT, from anywhere but a running game - the shelf, every menu, and the
 * in-game menu, where the game waits paused underneath: Muse, and straight to
 * Now Playing when something is playing or paused. Nothing on a card with no
 * music, which is also a card with no Muse on its shelf.
 *
 * `over` is the menu it opened over, or NULL for the shelf and the screens
 * that draw it. True when that menu's power rule ended it too. */
static bool muse_open(app *a, const menu_style *over, void *ctx)
{
	const mu_now *mn = musec_now();

	if (g_muse.ntracks == 0) return false;
	g_muse_over = over;
	g_muse_over_ctx = ctx;
	g_muse_gone = false;
	muse_shelf_screen(a, mn->state == MU_PLAYING || mn->state == MU_PAUSED);
	g_muse_over = NULL;
	g_muse_over_ctx = NULL;
	/* Nothing Muse did closes the menu it may have been opened over. */
	g_menu_closing = false;
	return g_muse_gone;
}

/* A on a book: where it was left, straight onto Now Playing with its files
 * underneath. The beginning for a book never started or finished; nothing new
 * for the book already playing or paused, which A only opens. */
static muse_exit muse_book(app *a, int al)
{
	const mu_now *mn = musec_now();
	bool loaded = mn->state == MU_PLAYING || mn->state == MU_PAUSED;

	if (!loaded || muse_playing_album(NULL) != al) {
		const ml_album *b = &g_muse.albums[al];
		const char **paths = calloc((size_t)b->n, sizeof *paths);
		double at;
		int i, start = book_place(al, &at);

		if (!paths) return MUSE_BACK;
		for (i = 0; i < b->n; i++) paths[i] = g_muse.tracks[b->first + i].path;
		fprintf(stderr, "muse: %s from file %d at %.1f\n", b->name, start + 1, at);
		muse_play(paths, b->n, start, at, true, book_speed(al),
		          g_muse.artists[muse_artist_of(al)].name, b->name);
		free(paths);
	}
	return muse_tracks(a, al, true);
}

/* A on card `k` of Muse's shelf: its album's tracks, over the shelf, or a
 * book where it was left. */
static muse_exit muse_album(app *a, const sysview *v, int k)
{
	if (!v->album || k < 0 || k >= v->list.count) return MUSE_BACK;
	if (g_muse.albums[v->album[k]].book) return muse_book(a, v->album[k]);
	return muse_tracks(a, v->album[k], false);
}

/* Muse's shelf again after its menu, whose Rescan Folder rebuilds every view
 * on the card, so the view being held is found again rather than trusted.
 * NULL when there is no Muse left to show. */
static sysview *muse_shelf_refind(app *a)
{
	sysview *v;
	int i, muse = -1;

	for (i = 0; i < a->sys.count; i++)
		if (is_muse(&a->sys.systems[i])) { muse = i; break; }
	if (muse < 0 || a->view[muse].list.count <= 0) return NULL;
	a->sys_cursor = muse;
	a->screen = SCREEN_GAMES;
	v = &a->view[muse];
	if (v->cursor >= v->list.count) v->cursor = v->list.count - 1;
	cf_reset(&v->cf, v->cursor);
	return v;
}

/* Which card on Muse's shelf is that album, or -1. */
static int muse_card_of(const sysview *v, int album)
{
	int k;

	for (k = 0; v->album && k < v->list.count; k++)
		if (v->album[k] == album) return k;
	return -1;
}

/* MUSE'S SHELF, OPENED ON TOP OF WHATEVER WAS ON SCREEN. Eric's, 2026-09-20.
 *
 * SELECT used to open a list of artists, because this shelf belongs to the
 * main loop - it is the games shelf, drawn for Muse's own view - and SELECT
 * can be pressed inside a menu, on Play Time, or in the in-game menu over a
 * paused game, none of which the main loop is driving. A list could draw over
 * any of those and the shelf could not, so the front door to a screen made
 * entirely of covers was text. The rest of the launcher is pictures.
 *
 * So the shelf is driven from here as well: the same view, the same cards, the
 * same movement, and draw_shelf doing the drawing - the one loop in this file
 * that borrows another screen's. What it does NOT borrow is update_games'
 * input, because B has to come back here rather than walk out to the systems
 * row, and MENU has to survive a Rescan rebuilding every view underneath it.
 *
 * The screen and the cursor are put back on the way out, so whatever opened
 * Muse is still there when it closes. */
static void muse_shelf_screen(app *a, bool now)
{
	int prev_screen = a->screen, prev_sys = a->sys_cursor;
	bool vert = CARD_DIRS[g_dir].vertical;
	in_button back = vert ? IN_UP : IN_LEFT, fwd = vert ? IN_DOWN : IN_RIGHT;
	in_button jup = vert ? IN_LEFT : IN_UP, jdn = vert ? IN_RIGHT : IN_DOWN;
	sysview *v;
	int muse = -1, i, playing;
	bool done = false;

	for (i = 0; i < a->sys.count; i++)
		if (is_muse(&a->sys.systems[i])) { muse = i; break; }
	if (muse < 0 || a->view[muse].list.count <= 0) return;

	a->sys_cursor = muse;
	enter_system(a);                      /* the screen, the coverflow, the window */
	v = &a->view[muse];

	/* On the album that is playing, so SELECT lands where the music is - on
	 * the books when it is a book. */
	playing = muse_playing_album(NULL);
	if (playing >= 0 && g_muse.albums[playing].book != muse_books_shown())
		muse_show(v, g_muse.albums[playing].book);
	if (playing >= 0) {
		int k = muse_card_of(v, playing);

		if (k >= 0) { v->cursor = k; cf_reset(&v->cf, k); }
	}
	if (now && playing >= 0) {
		muse_exit how = muse_tracks(a, playing, true);

		if (how == MUSE_CLOSE || g_muse_gone) done = true;
		else if (how == MUSE_REBUILT) {
			sysview *nv = muse_shelf_refind(a);

			if (nv) v = nv; else done = true;
		}
	}

	/* 0: the first pass draws (away). */
	Uint32 last_pass = 0, last_render = 0;

	while (!done && !want_quit && a->running) {
		int n = v->list.count, dir = 0;

		muse_screen_poll();
		if (cover_answers()) redraw_now();
		plat_input_poll(&a->in);
		if (a->in.quit_requested) { a->running = false; break; }
		{
			pwr_action pa = power_check(a);
			if (pa == PWR_POWEROFF) { muse_power(a); break; }
		}
		/* B and SELECT both leave, which is the rule everywhere in Muse: one
		 * button in, the same button out, and B for the level below. */
		if (a->in.pressed[IN_BACK] || a->in.pressed[IN_SELECT]) break;

		if (n > 0) {
			if (in_repeat(&a->in, back)) { v->cursor = (v->cursor - 1 + n) % n; dir = -1; }
			if (in_repeat(&a->in, fwd))  { v->cursor = (v->cursor + 1) % n; dir = +1; }
			if (in_repeat(&a->in, jdn))  v->cursor = shelf_letter_jump(v, +1);
			if (in_repeat(&a->in, jup))  v->cursor = shelf_letter_jump(v, -1);
			if (in_repeat(&a->in, IN_L1)) v->cursor = ((v->cursor - CF_WINDOW) % n + n) % n;
			if (in_repeat(&a->in, IN_R1)) v->cursor = (v->cursor + CF_WINDOW) % n;
			cf_set_cursor_dir(&v->cf, v->cursor, n, dir);
		}
		if (in_repeat(&a->in, IN_VOLUP))    plat_volume_nudge(+1);
		if (in_repeat(&a->in, IN_VOLDN))    plat_volume_nudge(-1);
		if (in_repeat(&a->in, IN_BRIGHTUP)) plat_brightness_nudge(+1);
		if (in_repeat(&a->in, IN_BRIGHTDN)) plat_brightness_nudge(-1);

		if (a->in.pressed[IN_ACCEPT]) {
			/* SELECT from the tracks or Now Playing closes all of Muse, and
			 * a rescan from their MENU leaves this shelf to be found again. */
			muse_exit how = muse_album(a, v, v->cursor);

			if (how == MUSE_CLOSE || g_muse_gone) break;
			if (how == MUSE_REBUILT) {
				sysview *nv = muse_shelf_refind(a);

				if (!nv) break;
				v = nv;
			}
		} else if (a->in.pressed[IN_MENU]) {
			/* Muse's own menu. Sort By reorders this shelf and Rescan
			 * Folder rebuilds it, so it is found again either way. */
			sysview *nv;

			if (muse_menu(a) == MUSE_CLOSE) break;
			if (!(nv = muse_shelf_refind(a))) break;
			v = nv;
		}

		if (shelf_draw_due(a, &last_pass, last_render)) {
			tick_tint(a);
			render(a);
			last_render = plat_now_ms();
		} else {
			SDL_Delay(IDLE_POLL_MS);
		}
	}

	evict_far(v, TEX_KEEP_FAR);
	a->screen = prev_screen;
	a->sys_cursor = prev_sys;
	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);
}

/* ---- Muse: Album Art -------------------------------------------------------- */

/* The artist an album is under, and its folder relative to the Music folder. */
static int muse_artist_of(int al)
{
	int ar;

	for (ar = 0; ar < g_muse.nartists - 1; ar++)
		if (al < g_muse.artists[ar].first + g_muse.artists[ar].n) break;
	return ar;
}

static void muse_album_dir(int al, char *out, size_t n)
{
	const char *p = g_muse.albums[al].n > 0 ? g_muse.tracks[g_muse.albums[al].first].path : "";
	const char *slash = strrchr(p, '/');

	snprintf(out, n, "%.*s", slash ? (int)(slash - p) : 0, p);
}

/* Which albums Album Art asks about: every one with no cover on the card, or
 * with one smaller on its short side than the shelf draws it - the album
 * frame's height, 461px - which the service has not already supplied. Eric's
 * rule, 2026-09-19. A cover the service gave is remembered, because its own
 * can be small too - La Strada's is 360px - and asking again would only fetch
 * the same one; an album it could not find is asked about on every run. */
static int museart_jobs(museart_job *jobs, int max)
{
	int crisp = (int)(CF_STAGE_H * CF_LAYOUT_ALBUMS.size + 0.5f);
	int i, n = 0;

	for (i = 0; i < g_muse.nalbums && n < max; i++) {
		char dir[LIB_PATH], key[LIB_PATH + 16], p[LIB_PATH * 2 + 8];
		museart_job *j = &jobs[n];
		int w = 0, h = 0;
		bool have;

		/* Not a book: MusicBrainz knows records, and a book's cover is its
		 * own file or the picture in its folder. */
		if (g_muse.albums[i].n <= 0 || g_muse.albums[i].book) continue;
		muse_album_dir(i, dir, sizeof dir);
		snprintf(key, sizeof key, "museart.%s", dir);
		if (db_has(db_lib(), key)) continue;
		ml_cover_base(g_muse_root, &g_muse, i, j->base, sizeof j->base);
		snprintf(p, sizeof p, "%s.jpg", j->base);
		have = file_nonempty(p);
		if (!have) { snprintf(p, sizeof p, "%s.png", j->base); have = file_nonempty(p); }
		if (have && museart_image_size(p, &w, &h) && (w < h ? w : h) >= crisp) continue;
		snprintf(j->artist, sizeof j->artist, "%s", g_muse.artists[muse_artist_of(i)].name);
		snprintf(j->album, sizeof j->album, "%s", g_muse.albums[i].name);
		j->tracks = g_muse.albums[i].n;
		j->id = i;
		n++;
	}
	return n;
}

/* A cover arrived for album `al`: remember where it came from, and drop the old
 * one from the shelf and from Now Playing so both draw the new one. The whole
 * worker queue is disowned, not just this card, because a decode of the OLD
 * file may already be in flight and would otherwise land on top. */
static void album_art_landed(app *a, int al, const char *rg)
{
	char dir[LIB_PATH], key[LIB_PATH + 16];
	int s;

	if (al < 0 || al >= g_muse.nalbums) return;
	muse_album_dir(al, dir, sizeof dir);
	snprintf(key, sizeof key, "museart.%s", dir);
	db_set_str(db_lib(), key, rg[0] ? rg : "fetched");
	if (g_cov) g_cov[al].st = COV_JPG;
	for (s = 0; s < a->sys.count; s++) {
		sysview *v = &a->view[s];
		int k;

		if (!is_muse(&a->sys.systems[s]) || !v->tex || !v->album) continue;
		for (k = 0; k < v->list.count; k++)
			if (v->album[k] == al && v->tex[k]) {
				SDL_DestroyTexture(v->tex[k]);
				v->tex[k] = NULL;
			}
	}
	texload_bump();
	if (g_np.album == al) np_forget();
}

/* Album Art, from Muse's menu: MusicBrainz and the Cover Art Archive, for the
 * albums museart_jobs chooses. The covers change on the shelf behind the panel
 * as they arrive. B stops. */
static void album_art_screen(app *a)
{
	museart_job *jobs;
	museart_status st;
	menu_row rows[3];
	char head[300], fit[200], where[64], counts[96];
	bool working, done = false;
	int n;

	if (!net_online()) {
		menu_row row = { "No network", NULL, false };

		draw_shelf(a);
		menu_draw(a, "Album Art", &row, 1, -1, 0, MUSE_ACCENT);
		draw_chrome(a->r);
		plat_present(a->r);
		SDL_Delay(1600);
		plat_input_flush();
		memset(&a->in, 0, sizeof a->in);
		return;
	}
	jobs = calloc((size_t)(g_muse.nalbums > 0 ? g_muse.nalbums : 1), sizeof *jobs);
	if (!jobs) return;
	n = museart_jobs(jobs, g_muse.nalbums);
	/* The one async slot, which an account answer from a game just quit may
	 * still be holding - the same reason Box Art gives it up first. */
	sync_abandon();
	working = museart_begin(jobs, n, "/tmp/tortos-musicbrainz.json");
	free(jobs);
	fprintf(stderr, "album art: %d album%s to ask about\n", n, n == 1 ? "" : "s");

	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);
	while (!done && !want_quit && a->running) {
		if (working && museart_step(plat_now_ms()) == 0) {
			working = false;
			museart_status_get(&st);
			fprintf(stderr, "album art: %d found, %d not found, %d failed%s%s\n",
			        st.found, st.missing, st.failed, st.problem[0] ? " - " : "",
			        st.problem);
		}
		museart_status_get(&st);
		if (st.landed >= 0) album_art_landed(a, st.landed, st.rg);

		ui_fit_text(ui_font(UI_F_LABEL),
		            working ? (st.now[0] ? st.now : "starting")
		            : st.problem[0] ? st.problem
		            : n == 0 ? "Every album has a crisp cover" : "Done",
		            fit, sizeof fit, TORTOS_SCREEN_W * 3 / 4);
		snprintf(head, sizeof head, "Album Art\n%s", fit);
		snprintf(where, sizeof where, "%d of %d", st.done, st.n);
		if (st.failed)
			snprintf(counts, sizeof counts, "%d found, %d not found, %d failed",
			         st.found, st.missing, st.failed);
		else
			snprintf(counts, sizeof counts, "%d found, %d not found",
			         st.found, st.missing);
		rows[0] = (menu_row){ "Albums", where,  false };
		rows[1] = (menu_row){ "Covers", counts, false };
		rows[2] = (menu_row){ working ? "B to stop" : "B to close", NULL, false };

		plat_input_poll(&a->in);
		if (a->in.quit_requested) { museart_cancel(); a->running = false; return; }
		{
			pwr_action pa = power_check(a);
			if (pa == PWR_POWEROFF) {
				museart_cancel();
				power_off(a);
				return;
			}
		}
		if (menu_leaving(a)) done = true;
		if (in_repeat(&a->in, IN_VOLUP))    plat_volume_nudge(+1);
		if (in_repeat(&a->in, IN_VOLDN))    plat_volume_nudge(-1);
		if (in_repeat(&a->in, IN_BRIGHTUP)) plat_brightness_nudge(+1);
		if (in_repeat(&a->in, IN_BRIGHTDN)) plat_brightness_nudge(-1);

		/* Idle does not count while covers are arriving: it is the device
		 * doing what it was asked, and powering off halfway loses the rest. */
		if (working) a->idle.since_ms = plat_now_ms();

		tick_tint(a);
		draw_shelf(a);
		SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(a->r, 0, 0, 0, 120);
		SDL_RenderFillRect(a->r, NULL);
		menu_draw(a, head, rows, 3, -1, menu_std_width(a), MUSE_ACCENT);
		draw_chrome(a->r);
		plat_present(a->r);
		SDL_Delay(8);
	}
	museart_cancel();
	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);
}

static void synopsis_screen(app *a, const char *title, const char *text,
                            unsigned accent)
{
	char (*lines)[CHV_WRAP_COLS];
	menu_row *rows;
	int fixed = menu_std_width(a);
	int n, done = 0, loop_at = 0;

	if (!text || !*text) return;
	lines = calloc(SYN_WRAP_LINES, sizeof *lines);
	rows  = calloc(SYN_ROWS, sizeof *rows);
	if (!lines || !rows) { free(lines); free(rows); return; }

	n = syn_layout(text, fixed, lines, rows, &loop_at);

	/* Opened, so its scroll starts at the top. Without this, closing a long
	 * synopsis and opening it again resumed halfway down: the clock's keys -
	 * the row count and the panel height - are identical on the way back in,
	 * so nothing told it a reader had arrived. */
	mq_reset(MQ_VSCROLL);
	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);

	while (!done && !want_quit && a->running) {
		plat_input_poll(&a->in);
		if (a->in.quit_requested) break;
		if (menu_leaving(a) || a->in.pressed[IN_ACCEPT] || a->in.pressed[IN_X]) done = 1;
		if (a->in.pressed[IN_SELECT]) muse_open(a, NULL, NULL);
		/* Nothing else watches power for this screen, the same as every other
		 * loop the launcher runs outside plat_resident_wait. */
		{
			pwr_action pa = power_check(a);
			if (pa == PWR_POWEROFF) { power_off(a); break; }
		}

		draw_shelf(a);
		SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(a->r, 0, 0, 0, 120);
		SDL_RenderFillRect(a->r, NULL);
		menu_draw_ex(a, title, rows, n, -1, fixed, accent, NULL, false, loop_at);
		draw_battery(a->r);
		plat_present(a->r);
		SDL_Delay(8);
	}

	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);
	free(lines);
	free(rows);
}

static void cheevos_screen(app *a, SDL_Texture *bg, bool over_shelf)
{
	int n = chv_count(), sel = 0, i, done = 0;
	menu_row *rows;
	unsigned *vcols;
	char (*vals)[16];
	char heading[192];

	if (n <= 0) return;
	rows  = calloc((size_t)n, sizeof *rows);
	vals  = calloc((size_t)n, sizeof *vals);
	vcols = calloc((size_t)n, sizeof *vcols);
	if (!rows || !vals || !vcols) { free(rows); free(vals); free(vcols); return; }

	for (i = 0; i < n; i++) {
		const cheevo *c = chv_at(i);

		snprintf(vals[i], sizeof vals[i], "%d", c->points);
		rows[i].label = c->title;
		rows[i].value = vals[i];
		rows[i].live  = c->earned || c->earned_now;
		/* Said by color on EVERY earned row, not only the one under the
		 * cursor. live already lifts the label one step, which alone was too
		 * little to scan a hundred-entry set by. */
		vcols[i] = rows[i].live ? UI_EARNED_RGB : 0u;
	}
	/* Two lines: the game on one, the counts on the other.
	 *
	 * On one line this was "Hagane: The Final Conflict   0/36   0/415 points",
	 * 1031 pixels against a 778 pixel panel - and menu_draw centers a heading,
	 * so it lost BOTH ends: the H and the word "points". Dropping the title
	 * fixed the clipping and threw away something worth keeping. Eric's
	 * suggestion, and it is better than either. */
	snprintf(heading, sizeof heading, "%s\n%d/%d cheevos   %d/%d points",
	         chv_game_title(), chv_earned(), n,
	         chv_points_earned(), chv_points_total());

	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);

	while (!done && !want_quit) {
		plat_input_poll(&a->in);

		if (in_repeat(&a->in, IN_UP))   sel = (sel + n - 1) % n;
		if (in_repeat(&a->in, IN_DOWN)) sel = (sel + 1) % n;
		/* A set runs to well over a hundred entries, so the shoulder buttons
		 * page it the same way they page a shelf. */
		if (in_repeat(&a->in, IN_L1))   sel = sel > 8 ? sel - 8 : 0;
		if (in_repeat(&a->in, IN_R1))   sel = sel < n - 9 ? sel + 8 : n - 1;
		if (menu_leaving(a)) done = 1;
		if (over_shelf && a->in.pressed[IN_SELECT]) muse_open(a, NULL, NULL);
		/* A used to close this screen. It opens the achievement instead,
		 * which is the only thing on it there was ever anything more to say
		 * about; BACK and MENU still close, so nothing lost a way out. */
		if (a->in.pressed[IN_ACCEPT] && cheevo_detail_screen(a, bg, over_shelf, chv_at(sel)))
			done = 1;
		/* Power still stops the game from in here. Not trapping it would
		 * make this screen the one place in the launcher that ignores it.
		 *
		 * The note is what was missing: stopping the game is not the same as
		 * powering off, and only the evdev watchdog inside plat_resident_wait
		 * sets that flag - a loop which is not running while this screen is
		 * up. So power here stopped the game and then went back to the shelf,
		 * with the menu still drawn over it until something was pressed.
		 *
		 * Opened from the game details screen there is no game to stop, and
		 * power means what it means everywhere else on the shelf. */
		{
			pwr_action pa = power_check(a);
			if (pa == PWR_POWEROFF) {
				if (over_shelf) { power_off(a); break; }
				plat_note_power_pressed();
				plat_resident_line("STOP");
				done = 1;
			}
		}

		chv_backdrop(a, bg, over_shelf);
		/* visits_all: every achievement can be opened, earned or not, so
		 * `live` here is about color and not about reach. */
		menu_draw_ex(a, heading, rows, n, sel, menu_std_width(a), a->tint,
		             vcols, true, 0);
		draw_battery(a->r);
		plat_present(a->r);
		SDL_Delay(8);
	}

	free(rows);
	free(vals);
	free(vcols);
	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);
}

/* The hotkey submenu (sibling Diatom feature, its ADR-0035): which face
 * button or shoulder, if any, held with SELECT triggers fast-forward, rewind,
 * a quicksave or a quickload - the same set Diatom's hotkeys_set accepts, so a
 * binding made here can never be one Diatom would refuse. Display mode and
 * filter were bindable too, from plorpos-gkd.22 until plorpos-gkd.73 left the
 * mode to the shelf and in-game menus and nearest as the only filter; a saved
 * binding naming either is dropped by hk_parse and skipped by Diatom.
 *
 * A Hotkey Modifier row (global - which key is held for all of them, diatom's
 * ADR-0038) above four fixed action rows, cycled left and right the way
 * Display Mode already is -
 * not a "press any button to capture it" flow, which this codebase has
 * never built anywhere and would have been the highest-risk new interaction
 * to write with no way to run it. A button already bound to one row is
 * cleared from whichever OTHER row held it rather than refusing the change:
 * the wire format itself refuses a spec with a button claimed twice
 * (diatom's hotkeys_set), so allowing that here would mean silently failing
 * to persist instead of a clear "last choice wins" - friendlier for a menu
 * than for a protocol. The parser/serializer (hk_parse/hk_serialize) live
 * in hkbind.c/.h, split out under ADR-0001 so a check can drive them with
 * no SDL. */
/* Rewind Speed, the row under the bindings (plorpos-gkd.40): one
 * setting for every system, sent to Diatom as SETREWINDSPEED after each RUN
 * (platform.c) and live from here. `every` is Diatom's capture cadence,
 * which IS the speed - one snapshot replayed per displayed frame - and 0 is
 * off. The ring's memory is fixed, so a slower speed holds less history;
 * the player chose that trade. 5 matches both builds' default. */
static const int  RW_EVERY[] = { 1, 2, 3, 5, 10, 0 };
static const char *const RW_NAME[] = { "1x", "2x", "3x", "5x", "10x", "Disabled" };
#define RW_COUNT ((int)(sizeof RW_EVERY / sizeof RW_EVERY[0]))
#define HK_SCREEN_ROWS (HK_ROW_COUNT + 1)

/* Press-to-bind's inputs, the stick ahead of the d-pad: a stick push also
 * sets the plain direction in the same frame, and the first match wins. */
static const struct { in_button b; int hk; } HK_CAPTURE[] = {
	{ IN_L1, HK_IN_L1 }, { IN_R1, HK_IN_R1 }, { IN_L2, HK_IN_L2 }, { IN_R2, HK_IN_R2 },
	{ IN_ACCEPT, HK_IN_A }, { IN_BACK, HK_IN_B }, { IN_X, HK_IN_X }, { IN_Y, HK_IN_Y },
	{ IN_SUP, HK_IN_SUP }, { IN_SDOWN, HK_IN_SDOWN },
	{ IN_RSUP, HK_IN_RSUP }, { IN_RSDOWN, HK_IN_RSDOWN },
	{ IN_RSLEFT, HK_IN_RSLEFT }, { IN_RSRIGHT, HK_IN_RSRIGHT },
	{ IN_SLEFT, HK_IN_SLEFT }, { IN_SRIGHT, HK_IN_SRIGHT },
	{ IN_UP, HK_IN_UP }, { IN_DOWN, HK_IN_DOWN },
	{ IN_LEFT, HK_IN_LEFT }, { IN_RIGHT, HK_IN_RIGHT },
};

/* The launcher key behind a modifier's wire name (plat_hotkey_modifiers). */
static in_button hk_modifier_button(const char *wire)
{
	if (!strcmp(wire, "select")) return IN_SELECT;
	if (!strcmp(wire, "l3"))     return IN_L3;
	if (!strcmp(wire, "r3"))     return IN_R3;
	if (!strcmp(wire, "home"))   return IN_HOME;
	return IN_MENU;
}

static void hotkeys_screen(app *a, SDL_Texture *bg, const char *tag)
{
	/* Row 0 is the modifier (global, diatom's ADR-0038); the action rows
	 * follow it, action i at row i + 1; on the GKD, Rewind Speed is last. The
	 * rule and legend are drawn after them and are never selected. */
	enum { MOD_ROW = 0, FIRST_ACT = 1, ROWS = HK_SCREEN_ROWS + 1 };
	int trig_for_row[HK_ROW_COUNT];
	menu_row rows[ROWS + 2];
	char vals[HK_ROW_COUNT][32];
	const char *const *mod_wire, *const *mod_label;
	int nmods = plat_hotkey_modifiers(&mod_wire, &mod_label);
	int mod = 0, sel = 0, done = 0, i;
	/* Press-to-bind (plorpos-gkd.43.3): A on an action row waits for the
	 * next trigger. Only a lone MENU tap cancels - B is bindable, and MENU
	 * held is the default modifier - so it is judged on the release. */
	bool capturing = false, menu_tap = false;
	bool changed = false;
	char spec[128];

	hk_parse(plat_hotkey_map(tag), trig_for_row);
	const int RW_ROW = FIRST_ACT + HK_ROW_COUNT;
	int rw = 0, every = db_get_int(db_dev(), "rewindspeed", 5);

	while (rw < RW_COUNT - 1 && RW_EVERY[rw] != every) rw++;
	{
		const char *cur = plat_hotkey_modifier();
		while (mod < nmods - 1 && strcmp(mod_wire[mod], cur)) mod++;
	}

	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);

	while (!done && !want_quit) {
		plat_input_poll(&a->in);

		if (capturing) {
			int act = sel - FIRST_ACT, t = 0;
			bool held = a->in.down[hk_modifier_button(mod_wire[mod])];
			size_t k;

			for (k = 0; k < sizeof HK_CAPTURE / sizeof HK_CAPTURE[0] && !t; k++)
				if (a->in.pressed[HK_CAPTURE[k].b])
					t = hk_trig_from(HK_CAPTURE[k].hk, held);
			if (t) {
				/* Taken from whichever row held it: one trigger, one action. */
				for (i = 0; i < HK_ROW_COUNT; i++)
					if (trig_for_row[i] == t) trig_for_row[i] = 0;
				trig_for_row[act] = t;
				capturing = false;
				changed = true;
			} else if (a->in.pressed[IN_MENU]) {
				menu_tap = true;
			} else if (a->in.down[IN_MENU]) {
				for (k = 0; k < IN_COUNT; k++)
					if (k != IN_MENU && a->in.pressed[k]) menu_tap = false;
			} else if (menu_tap) {
				capturing = menu_tap = false;   /* a lone tap, released */
			}
		} else {
			if (in_repeat(&a->in, IN_UP))   sel = (sel + ROWS - 1) % ROWS;
			if (in_repeat(&a->in, IN_DOWN)) sel = (sel + 1) % ROWS;

			if (sel == MOD_ROW) {
				int d = 0;

				if (in_repeat(&a->in, IN_LEFT))  d = -1;
				if (in_repeat(&a->in, IN_RIGHT)) d = 1;
				if (d) {
					mod = (mod + d + nmods) % nmods;
					plat_hotkey_modifier_set(mod_wire[mod]);
					changed = true;
				}
			} else if (sel == RW_ROW) {
				int d = 0;

				if (in_repeat(&a->in, IN_LEFT))  d = -1;
				if (in_repeat(&a->in, IN_RIGHT)) d = 1;
				if (d) {
					rw = (rw + d + RW_COUNT) % RW_COUNT;
					db_set_int(db_dev(), "rewindspeed", RW_EVERY[rw]);
					plat_resident_line("SETREWINDSPEED\tevery=%d", RW_EVERY[rw]);
				}
			} else if (a->in.pressed[IN_ACCEPT]) {
				capturing = true;
				menu_tap = false;
			} else if (a->in.pressed[IN_X] && trig_for_row[sel - FIRST_ACT]) {
				trig_for_row[sel - FIRST_ACT] = 0;
				changed = true;
			}

			if (menu_leaving(a)) done = 1;
		}

		if (changed) {
			hk_serialize(trig_for_row, spec, sizeof spec);
			plat_hotkey_set(tag, spec);
			/* Live, not only persisted: this screen is only ever open
			 * mid-session (reached from the in-game menu), so the change
			 * should take hold without the player having to quit and
			 * relaunch to see it. */
			plat_resident_line("SETHOTKEYS\thotkeys=%s\tmodifier=%s",
			                   spec, mod_wire[mod]);
			changed = false;
		}

		{
			pwr_action pa = power_check(a);
			if (pa == PWR_POWEROFF) {
				plat_note_power_pressed();
				plat_resident_line("STOP");
				done = 1;
			}
		}

		rows[MOD_ROW] = (menu_row){ "Hotkey Modifier", mod_label[mod], true };
		for (i = 0; i < HK_ROW_COUNT; i++) {
			int t = trig_for_row[i];

			if (capturing && i == sel - FIRST_ACT)
				snprintf(vals[i], sizeof vals[i], "Press...");
			else
				snprintf(vals[i], sizeof vals[i], "%s%s%s%s",
				         hk_trig_mod(t) ? mod_label[mod] : "",
				         hk_trig_mod(t) ? " + " : "",
				         hk_trig_stick(t) && plat_two_sticks() ? "L " : "",
				         HK_TRIG_NAME[t]);
			rows[FIRST_ACT + i] = (menu_row){ HK_ACTION_LABEL[i], vals[i], true };
		}
		rows[RW_ROW] = (menu_row){ "Rewind Speed", RW_NAME[rw], true };
		rows[ROWS] = MENU_RULE;
		rows[ROWS + 1] = MENU_NOTE(capturing ? "Press a button    Menu: cancel"
		                                     : "A: set    X: clear");

		chv_backdrop(a, bg, false);
		menu_draw(a, "Hotkeys", rows, ROWS + 2, sel, 0, MENU_ACCENT);
		draw_battery(a->r);
		plat_present(a->r);
		SDL_Delay(8);
	}

	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);
}

/* Measured across every mode label, so cycling the row does not resize the
 * panel under the cursor - the same reason the shelf menus have a fixed width. */
/* The width of the rows as drawn: compacted, as gm_menu_build shows them. */
static int gm_measure(const menu_row *full)
{
	menu_row rows[GM_ROWS];
	int ids[GM_ROWS];

	memcpy(rows, full, sizeof rows);
	return menu_measure(rows, gm_compact(rows, GM_ROWS, ids), NULL);
}

static int gm_width(app *a)
{
	menu_row rows[GM_ROWS];
	gm_bufs b;
	int w = 0, k;

	gm_build(a, rows, &b);
	for (k = 0; k < DMODE_COUNT; k++) {
		int mw;
		rows[GM_DISPLAY].value = DMODES[k].label;
		mw = gm_measure(rows);
		if (mw > w) w = mw;
	}
	/* And every shader name, for the same reason: picking one must not
	 * resize the panel the player comes back to. */
	for (k = 0; k < a->shaders.count && rows[GM_SHADER].live; k++) {
		int mw;
		rows[GM_SHADER].value = a->shaders.e[k].name;
		mw = gm_measure(rows);
		if (mw > w) w = mw;
	}
	for (k = 0; k < GBPAL_COUNT && rows[GM_PALETTE].live; k++) {
		int mw;
		rows[GM_PALETTE].value = gbpal_label(k);
		mw = gm_measure(rows);
		if (mw > w) w = mw;
	}
	for (k = 0; k < g_disc.count && rows[GM_DISC].live; k++) {
		char l[16];
		int mw;
		snprintf(l, sizeof l, "Disc %d", k + 1);
		rows[GM_DISC].value = l;
		mw = gm_measure(rows);
		if (mw > w) w = mw;
	}
	return w;
}

/* The in-game menu's context. `resume` is what the epilogue turns on: whether
 * the player is going back to the game or the game is over. */
typedef struct {
	app         *a;
	SDL_Texture *bg;       /* the paused frame, as Diatom last wrote it */
	gm_bufs      bufs;
	menu_style   st;
	bool         resume;
	int          ids[GM_ROWS];   /* drawn row -> gm_row, from gm_compact */
} gm_ctx;

static int gm_menu_build(void *ctx, menu_row *rows, int max,
                         const char **heading)
{
	gm_ctx *c = ctx;

	(void)max;
	*heading = NULL;       /* no title: the game behind it is the title */
	return gm_compact(rows, gm_build(c->a, rows, &c->bufs), c->ids);
}

/* The paused game, not the shelf. Diatom reports its rect with every DISPLAY
 * message, so the preview lands where the game actually is. */
static void gm_backdrop(app *a, void *ctx)
{
	gm_ctx *c = ctx;

	SDL_SetRenderDrawColor(a->r, 0, 0, 0, 255);
	SDL_RenderClear(a->r);
	draw_paused_frame(a, c->bg);
	SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(a->r, 0, 0, 0, 120);
	SDL_RenderFillRect(a->r, NULL);
}

/* Power here stops the GAME. The device is not going anywhere, and the wait
 * loop under this menu has to be told or it goes straight back to waiting on
 * a game nobody is running. */
static menu_result gm_power(app *a, void *ctx)
{
	(void)a; (void)ctx;
	plat_note_power_pressed();
	plat_resident_line("STOP");
	return MENU_DONE;
}

/* BEGIN PolyForm-Noncommercial-1.0.0 - NextUI-derived: the checkpoint before sleep, NextUI's Menu_beforeSleep. See NOTICE. */
/* NextUI's Menu_beforeSleep: before the device sleeps or powers off with a
 * game loaded, write it out - a battery that dies asleep loses whatever
 * sleep did not save. SLOT_AUTO, silently: a safety net, not a save the
 * player manages. Waits for Diatom's SAVED (its main.c answers every SAVE),
 * as State_autosave finishes before PWR_sleep begins; a save that never
 * answers costs three seconds, not the sleep. Nothing on the shelf: no game
 * is loaded there, and the cursor's game is not the one that was running.
 * The resume-into-game marker, NextUI's AUTO_RESUME_PATH, is .playing, which
 * already stands for the whole of a game session. */
static void checkpoint_game(app *a)
{
	sysview *sv = &a->view[a->sys_cursor];
	int o;
	char sp[LIB_PATH * 2], pp[LIB_PATH * 2];

	if (!a->game_on || !plat_resident_ready()) return;
	o = shelf_owner(a, a->sys_cursor, sv->cursor);
	slot_state_path(a, o, &sv->list.items[sv->cursor], SLOT_AUTO,
	                sp, sizeof sp);
	plat_resident_line("SAVE\tpath=%s", sp);
	slot_preview_path(a, o, &sv->list.items[sv->cursor], SLOT_AUTO,
	                  pp, sizeof pp);
	copy_file(plat_resident_last_preview(), pp);
	plat_resident_saved(sp, 3000);
}

/* END PolyForm-Noncommercial-1.0.0 */
/* Sleep with music playing: the screen goes off and the album plays on.
 * TortOS's own, asked for 2026-09-28 (TortOS-a5k) - a deliberate divergence
 * from NextUI, whose PWR_enterSleep pauses the music (SND_pauseAudio) and
 * sleeps as ever, the same kind of departure as Auto Off.
 *
 * Its own loop rather than light sleep's, because the launcher, not Muse,
 * moves the queue on: musec_poll hands the daemon the next track at each END
 * (musec.c's advance), so anything that stopped polling would end the album
 * with the track it was on. The same poll carries a headset's buttons.
 *
 * iPod-style, the user's call: Muse's own buttons do what they do on Now
 * Playing AND wake the screen; any other button only wakes; the volume keys
 * act without waking. A POWER tap wakes, a hold powers off. Whichever wakes
 * it, the press is used up - A in the dark toggles the music and does not
 * also open the game under the cursor. Muse Settings' Wake Screen On Press
 * set to No (TortOS-28l) keeps it dark instead: Muse's buttons act, every
 * other button is ignored, and only POWER wakes.
 *
 * Once the music stops - paused here, by a headset, or the album's end - it
 * stays dark with the same buttons live for the Suspend Timeout, so a pause
 * in the dark can be undone in the dark (TortOS-28l); music again cancels
 * the countdown. Only when it runs out does whatever asked for sleep carry
 * on: suspend, at once - *waited is the time already spent, which light
 * sleep counts toward the same timeout - or Auto Off's power-off. So the
 * Suspend Timeout is ALSO Auto Off's grace after the music stops: one
 * setting rather than a new one, the user's call 2026-09-28, and in the
 * README. Never suspend while playing: suspend is where the sound would
 * stop. */
typedef enum { DARK_WOKE, DARK_STOPPED, DARK_POWEROFF } dark_end;

static dark_end music_dark(app *a, unsigned *waited)
{
	static const char *const said[] = { "woke", "music stopped", "power off" };
	dark_end end = DARK_STOPPED;
	unsigned t0 = plat_now_ms();
	unsigned grace = (unsigned)plat_suspend_timeout_secs() * 1000u;
	unsigned stopped = 0;                 /* when it stopped; 0 while playing */
	bool wake = db_get_int(db_dev(), "muse.wake", 1);
	int b;

	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);
	plat_screen(false);
	for (;;) {
		pwr_action pa;
		unsigned now;

		musec_poll();
		/* Routed in the dark too, as Muse's own screens route: a headset
		 * that drops and comes back with the screen off took the song to
		 * the speaker and left it there until a wake (2026-10-06). */
		if (musec_heard()) aout_apply(false);
		now = plat_now_ms();
		if (musec_playing())               stopped = 0;
		else if (!stopped)                 stopped = now ? now : 1;
		else if (now - stopped >= grace)   break;
		plat_input_poll(&a->in);
		pa = plat_power_tap_or_hold(a->in.down[IN_POWER]);
		if (pa != PWR_NONE) {
			end = pa == PWR_POWEROFF ? DARK_POWEROFF : DARK_WOKE;
			break;
		}
		/* The switch as an iPod's hold (TortOS-ib9): read every tick, so
		 * flipping it here takes effect at once. Presses made while it is
		 * down are dropped, not saved for later. POWER, above, and headset
		 * keys, in musec_poll, stay live. */
		if (plat_hold_switch()) { SDL_Delay(50); continue; }
		if (in_repeat(&a->in, IN_VOLUP)) plat_volume_nudge(+1);
		if (in_repeat(&a->in, IN_VOLDN)) plat_volume_nudge(-1);
		for (b = 0; b < IN_COUNT; b++)
			if (a->in.pressed[b] && b != IN_POWER &&
			    b != IN_VOLUP && b != IN_VOLDN) break;
		if (b < IN_COUNT) {
			/* Now Playing's bindings - see muse_now_screen. */
			if (b == IN_ACCEPT)     musec_toggle();
			else if (b == IN_Y)     muse_cycle_mode();
			else if (b == IN_L1)    musec_prev();
			else if (b == IN_R1)    musec_next();
			else if (b == IN_LEFT)  musec_seek_by(-10);
			else if (b == IN_RIGHT) musec_seek_by(+10);
			if (wake) { end = DARK_WOKE; break; }
		}
		SDL_Delay(50);
	}
	/* Stopped stays dark: suspend or the power-off comes next. */
	if (end != DARK_STOPPED) plat_screen(true);
	if (waited) *waited = end == DARK_STOPPED ? plat_now_ms() - stopped : 0;
	fprintf(stderr, "sleep: music dark %us, %s%s\n",
	        (plat_now_ms() - t0) / 1000, said[end],
	        a->game_on ? " (in game)" : "");
	stats_asleep(plat_now_ms() - t0);
	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);
	a->idle.since_ms = plat_now_ms();
	return end;
}

/* BEGIN PolyForm-Noncommercial-1.0.0 - NextUI-derived: the sleep sequence, NextUI's PWR_update sleep (music_dark above is this project's own). See NOTICE. */
/* NextUI's one sleep, from its PWR_update: before_sleep, PWR_sleep,
 * after_sleep - whatever asked for it, a tap, Auto Sleep's idle, or the game
 * menu's Sleep row. Before: the game checkpointed (Menu_beforeSleep). NextUI
 * pauses the music here too (SND_pauseAudio); TortOS instead turns only the
 * screen off until it stops - music_dark, above. After:
 * the idle clock starts over (last_input_at = now), no button is left
 * standing down, and time asleep stays out of Play Time (gametimectl).
 * False when light sleep found no suspend to escalate into: the caller
 * powers off, as PWR_waitForWake does. */
static bool sleep_cycle(app *a)
{
	unsigned t0, waited = 0;
	bool awake;

	checkpoint_game(a);
	if (musec_playing()) {
		dark_end end = music_dark(a, &waited);

		if (end != DARK_STOPPED) return end == DARK_WOKE;
	}
	t0 = plat_now_ms();
	awake = plat_light_sleep(waited);
	fprintf(stderr, "sleep: %s after %us%s\n",
	        awake ? "awake" : "no suspend, powering off",
	        (plat_now_ms() - t0) / 1000, a->game_on ? " (in game)" : "");
	stats_asleep(plat_now_ms() - t0);
	memset(&a->in, 0, sizeof a->in);
	a->idle.since_ms = plat_now_ms();
	return awake;
}
/* END PolyForm-Noncommercial-1.0.0 */

/* Every screen's power check, in one place: the button (a tap sleeps, a hold
 * powers off) and the idle clock. Auto Sleep's idle is the same sleep a tap
 * is; Auto Off's (TortOS's own, no NextUI counterpart) is the same power-off
 * a hold is, so each screen's own power-off branch - power_off() on the
 * shelf, STOP in a game, where Diatom writes the Auto slot on its way out -
 * serves both. Sleep happens in here; the caller only ever sees
 * PWR_POWEROFF, or PWR_NONE to carry on. */
static pwr_action power_check(app *a)
{
	pwr_action pa = plat_power_tap_or_hold(a->in.down[IN_POWER]);

	if (pa == PWR_NONE && idle_due(a)) {
		pa = a->auto_poweroff ? PWR_POWEROFF : PWR_SLEEP;
		fprintf(stderr, "power: idle -> %s\n",
		        pa == PWR_SLEEP ? "sleep" : "power off");
		/* Auto Off during music: the screen now, the power-off once the
		 * music has been stopped for the Suspend Timeout (music_dark). A
		 * hold is not idle, and still does what it says at once. */
		if (pa == PWR_POWEROFF && musec_playing() &&
		    music_dark(a, NULL) == DARK_WOKE)
			pa = PWR_NONE;
	}
	if (pa == PWR_SLEEP) pa = sleep_cycle(a) ? PWR_NONE : PWR_POWEROFF;
	return pa;
}

/* The in-game Shader list (plorpos-gkd.72.4), over the same paused frame and
 * under the same power rule as the menu it opens from. */
typedef struct {
	gm_ctx *gm;
	int     sys;       /* the owner's: a per-system choice, as Display is */
} sh_ctx;

static int sh_build(void *ctx, menu_row *rows, int max, const char **heading)
{
	sh_ctx *c = ctx;
	app *a = c->gm->a;
	int i, n = a->shaders.count < max ? a->shaders.count : max;

	for (i = 0; i < n; i++)
		rows[i] = (menu_row){ a->shaders.e[i].name,
		                      i == a->view[c->sys].shader ? "Current" : NULL, true };
	*heading = "Shader";
	return n;
}

/* A applies the entry to the paused game, keeps it for the system, and goes
 * back to the in-game menu (the user's call: the backdrop there is Diatom's
 * raw frame, so the look shows on Continue either way). Only once Diatom has
 * answered: a chain it refuses is not saved, and the old one stays on. */
static menu_result sh_key(app *a, void *ctx, in_button key, int sel)
{
	sh_ctx *c = ctx;
	char f[1024], dir[CFG_STR * 2];

	if (key != IN_ACCEPT) return MENU_STAY;
	if (sel == a->view[c->sys].shader) return MENU_DONE;
	snprintf(dir, sizeof dir, "%s/shaders", P_ROOT);
	if (!sl_fields(&a->shaders, sel, dir, f, sizeof f)) return MENU_DONE;
	plat_resident_line("SETDISPLAY\t%s", f);
	/* A two-pass compile measured 89 ms on the GKD (diatom ADR-0041); the
	 * bound is for a Diatom that never answers, not for the usual case. */
	if (!plat_resident_sync_rect(2000)) {
		fprintf(stderr, "shader: %s not applied\n", a->shaders.e[sel].name);
		return MENU_DONE;
	}
	a->view[c->sys].shader = sel;
	shader_save(a, c->sys);
	return MENU_DONE;
}

static void sh_backdrop(app *a, void *ctx) { gm_backdrop(a, ((sh_ctx *)ctx)->gm); }
static menu_result sh_power(app *a, void *ctx) { return gm_power(a, ((sh_ctx *)ctx)->gm); }

static void shader_screen(app *a, gm_ctx *gm)
{
	sh_ctx c = { gm, (int)(owner_view(a) - a->view) };
	menu_style st = gm->st;

	st.fixed_w  = 0;
	st.backdrop = sh_backdrop;
	st.on_power = sh_power;
	st.start    = a->view[c.sys].shader;
	menu_run(a, &st, sh_build, sh_key, &c);
}

/* The in-game Palette list (plorpos-gkd.76), a Game Boy game's own. Same
 * shape as the Shader list above, but kept per game. */
typedef struct {
	gm_ctx           *gm;
	const game_entry *g;
	int               cur;
} pal_ctx;

static int pal_build(void *ctx, menu_row *rows, int max, const char **heading)
{
	pal_ctx *c = ctx;
	int i, n = GBPAL_COUNT < max ? GBPAL_COUNT : max;

	for (i = 0; i < n; i++)
		rows[i] = (menu_row){ gbpal_label(i), i == c->cur ? "Current" : NULL, true };
	*heading = "Palette";
	return n;
}

/* A sends it to the paused game and keeps it for this game. mgba reads the
 * palette as the game runs, so a fixed one shows on Continue; Auto's per-game
 * colours wait for the next launch (gbpal.c). No answer is waited for: an
 * option diatom does not take is logged by it, and the choice is still the
 * player's for the next launch. */
static menu_result pal_key(app *a, void *ctx, in_button key, int sel)
{
	pal_ctx *c = ctx;
	char p[64], col[64];

	(void)a;
	if (key != IN_ACCEPT) return MENU_STAY;
	if (sel == c->cur) return MENU_DONE;
	if (gbpal_opts(sel, p, sizeof p, col, sizeof col)) {
		char *eq;
		eq = strchr(p, '=');   plat_resident_line("SETOPT\tkey=%.*s\tvalue=%s", (int)(eq - p), p, eq + 1);
		eq = strchr(col, '='); plat_resident_line("SETOPT\tkey=%.*s\tvalue=%s", (int)(eq - col), col, eq + 1);
	}
	palette_save(c->g, sel);
	return MENU_DONE;
}

static void pal_backdrop(app *a, void *ctx) { gm_backdrop(a, ((pal_ctx *)ctx)->gm); }
static menu_result pal_power(app *a, void *ctx) { return gm_power(a, ((pal_ctx *)ctx)->gm); }

static void palette_screen(app *a, gm_ctx *gm)
{
	sysview *sv = &a->view[a->sys_cursor];
	int o = shelf_owner(a, a->sys_cursor, sv->cursor);
	pal_ctx c = { gm, &sv->list.items[sv->cursor], 0 };
	menu_style st = gm->st;

	c.cur = palette_of(a, o, c.g);
	if (c.cur < 0) return;
	st.fixed_w  = 0;
	st.backdrop = pal_backdrop;
	st.on_power = pal_power;
	st.start    = c.cur;
	menu_run(a, &st, pal_build, pal_key, &c);
}

/* The in-game Disc list (plorpos-gkd.47), an .m3u game's own. Same shape as
 * the Palette list above; the disc is kept per game for the next launch. */
#define GM_DISC_MAX 8   /* the most discs a list shows; PlayStation sets stop at 5 */
typedef struct {
	gm_ctx           *gm;
	const game_entry *g;
	int               sys;
} disc_ctx;

static void disc_note(int index, int count)
{
	g_disc.index = index;
	g_disc.count = count;
	snprintf(g_disc.label, sizeof g_disc.label, "Disc %d", index + 1);
}

/* Asked once, as the menu opens, and only of an .m3u game: everything else
 * pays nothing and draws no row. */
static void disc_query(app *a)
{
	sysview *sv = &a->view[a->sys_cursor];
	unsigned t0;
	int i, n;

	g_disc.count = 0;
	if (sv->list.count == 0 || !is_m3u(sv->list.items[sv->cursor].file)) return;
	t0 = SDL_GetTicks();
	if (plat_resident_line("DISC") && plat_resident_disc(&i, &n, 150))
		disc_note(i, n);
	fprintf(stderr, "disc: %d of %d (%u ms)\n", g_disc.index + 1, g_disc.count,
	        SDL_GetTicks() - t0);
}

static int disc_build(void *ctx, menu_row *rows, int max, const char **heading)
{
	static char label[GM_DISC_MAX][16];
	int i, n = g_disc.count < max ? g_disc.count : max;

	(void)ctx;
	if (n > GM_DISC_MAX) n = GM_DISC_MAX;
	for (i = 0; i < n; i++) {
		snprintf(label[i], sizeof label[i], "Disc %d", i + 1);
		rows[i] = (menu_row){ label[i], i == g_disc.index ? "Current" : NULL, true };
	}
	*heading = "Disc";
	return n;
}

/* A swaps the disc in the paused game and keeps it for this game. diatom
 * opens the tray now and closes it once the game has run a second, so the
 * game sees the lid open; the swap is answered either way. */
static menu_result disc_key_cb(app *a, void *ctx, in_button key, int sel)
{
	disc_ctx *c = ctx;
	int i, n;

	if (key != IN_ACCEPT) return MENU_STAY;
	if (sel == g_disc.index) return MENU_DONE;
	if (plat_resident_line("SETDISC\tindex=%d", sel) &&
	    plat_resident_disc(&i, &n, 500)) {
		disc_note(i, n);
		if (i == sel) disc_save(a, c->sys, c->g, i);
	}
	fprintf(stderr, "disc: asked for %d, in the drive %d\n", sel + 1, g_disc.index + 1);
	return MENU_DONE;
}

static void disc_backdrop(app *a, void *ctx) { gm_backdrop(a, ((disc_ctx *)ctx)->gm); }
static menu_result disc_power(app *a, void *ctx) { return gm_power(a, ((disc_ctx *)ctx)->gm); }

static void disc_screen(app *a, gm_ctx *gm)
{
	sysview *sv = &a->view[a->sys_cursor];
	disc_ctx c = { gm, &sv->list.items[sv->cursor],
	               shelf_owner(a, a->sys_cursor, sv->cursor) };
	menu_style st = gm->st;

	if (g_disc.count < 2) return;
	st.fixed_w  = 0;
	st.backdrop = disc_backdrop;
	st.on_power = disc_power;
	st.start    = g_disc.index;
	menu_run(a, &st, disc_build, disc_key_cb, &c);
}

static menu_result gm_key(app *a, void *ctx, in_button key, int sel)
{
	gm_ctx *c = ctx;

	sel = c->ids[sel];   /* a drawn row, read back as the row it shows */
	/* Applied to the running game at once, not on resume: the whole point of
	 * this row being here rather than on the shelf is judging the mode against
	 * the game it is being applied to. Diatom takes SETDISPLAY while paused
	 * (its ADR-0020), answers with the new rect, and the backdrop behind this
	 * menu redraws into it. */
	if (sel == GM_DISPLAY && (key == IN_LEFT || key == IN_RIGHT)) {
		gm_cycle_display(a, key == IN_RIGHT ? 1 : -1);
		return MENU_STAY;
	}
	if (key != IN_ACCEPT) return MENU_STAY;

	switch ((gm_row)sel) {
	case GM_CONTINUE:
		c->resume = true;
		return MENU_DONE;
	case GM_SAVE:
	case GM_LOAD: {
		int slot = slot_strip(a, c->bg, sel == GM_SAVE);

		if (slot) {
			sysview *sv = &a->view[a->sys_cursor];
			int o = shelf_owner(a, a->sys_cursor, sv->cursor);   /* see slot_strip */
			char sp[LIB_PATH * 2], pp[LIB_PATH * 2];

			slot_state_path(a, o, &sv->list.items[sv->cursor],
			                slot, sp, sizeof sp);
			if (sel == GM_SAVE) {
				plat_resident_line("SAVE\tpath=%s", sp);
				/* The paused frame is what the save holds, and Diatom already
				 * wrote it as the pause preview: copy it beside the state so
				 * the strip can show what is inside the slot. */
				slot_preview_path(a, o, &sv->list.items[sv->cursor],
				                  slot, pp, sizeof pp);
				copy_file(plat_resident_last_preview(), pp);
			} else {
				plat_resident_line("LOAD\tpath=%s", sp);
			}
			c->resume = true;
			return MENU_DONE;
		}
		/* Backed out: fall through to the menu, still paused. */
		plat_input_flush();
		memset(&a->in, 0, sizeof a->in);
		break;
	}
	/* A cycles it forward as well as left and right. Every other row in this
	 * menu is something A does, so a row that only answered to left and right
	 * was a row that looked broken. */
	case GM_DISPLAY:
		gm_cycle_display(a, +1);
		break;
	case GM_SHADER:
		shader_screen(a, c);
		break;
	case GM_PALETTE:
		palette_screen(a, c);
		break;
	case GM_DISC:
		disc_screen(a, c);
		break;
	case GM_CHEEVOS:
		cheevos_screen(a, c->bg, false);
		break;
	case GM_HOTKEYS: {
		sysview *sv = &a->view[a->sys_cursor];
		int o = shelf_owner(a, a->sys_cursor, sv->cursor);

		hotkeys_screen(a, c->bg, a->sys.systems[o].tag);
		break;
	}
	case GM_RESET:
		plat_resident_line("RESET");
		c->resume = true;
		return MENU_DONE;
	case GM_QUIT:
		/* The wait loop carries on until EXIT arrives; quitting is asking,
		 * not tearing down. */
		plat_resident_line("STOP");
		return MENU_DONE;
	default: break;
	}

	/* A nested screen may have been the one that took the power press or ran
	 * the clock out - the slot strip and the achievements list both stop the
	 * game and close themselves, which would leave this menu drawn over a game
	 * that had already been told to stop. Only those two set the flag, and
	 * both are reached from right here, so this is where it is asked. */
	if (plat_run_power_pressed()) return MENU_DONE;
	return MENU_STAY;
}

static void game_menu(app *a)
{
	gm_ctx c = { 0 };
	const char *pv = plat_resident_last_preview();

	c.a = a;
	/* What was pressed in the game is not for this menu. The launcher reads
	 * no input while a game runs, so it all waits in the queues, and the
	 * first frame here replayed it: a stale MENU closed the menu before it
	 * was seen, an Up then A wrapped to Quit and ended the game (seen on the
	 * device 2026-09-28). NextUI's menu opens with PAD_reset for the same
	 * reason. */
	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);
	if (pv && *pv) {
		SDL_Surface *sf = IMG_Load(pv);

		if (sf) {
			c.bg = SDL_CreateTextureFromSurface(a->r, sf);
			SDL_FreeSurface(sf);
		}
	}
	disc_query(a);
	/* Measured across every mode label, so cycling Display does not resize
	 * the panel underneath the cursor. */
	c.st.fixed_w     = gm_width(a);
	c.st.follow_tint = true;
	c.st.backdrop    = gm_backdrop;
	c.st.on_power    = gm_power;

	/* B and MENU are the runner's, and both mean Continue here. The runner has
	 * to be the one to say so: it flushes the input on its way out, so asking
	 * a->in afterwards would always say no button was pressed. MENU from a
	 * screen this menu opened - the save slots, Achievements - comes back as
	 * MENU_LEFT_BACK too, so it is Continue from there as well. */
	g_menu_closing = false;
	if (menu_run(a, &c.st, gm_menu_build, gm_key, &c) == MENU_LEFT_BACK)
		c.resume = true;
	g_menu_closing = false;

#if !defined(PLATFORM_H700)
	if (c.bg) SDL_DestroyTexture(c.bg);
#endif

	/* Asked to quit with the menu open. Leaving here without saying anything
	 * strands the game: it is paused, waiting on this socket, and the wait
	 * loop below would go straight back to waiting on IT. Each on the other,
	 * and neither reachable by a signal. Seen on the device 2026-08-29.
	 *
	 * STOP rather than RESUME - the process is going away, and a game that
	 * ends writes its state on the way out. */
	if (want_quit && !c.resume) plat_resident_line("STOP");

	if (c.resume) {
		/* Hand the pages back the way Diatom expects to find them.
		 *
		 * Diatom does not repaint the area outside its picture every frame -
		 * its pages start opaque black and it writes only the rect - which is
		 * sound while it owns the framebuffer and false the moment this
		 * process has drawn a full-screen menu into the same pages. At any
		 * display mode that does not fill the panel, resuming showed the game
		 * correctly sized with this menu still surrounding it, and flickering:
		 * Diatom cycles three pages and only the two this process presents
		 * into had been dirtied, so the border alternated menu, menu, black at
		 * the refresh rate.
		 *
		 * Twice because this process alternates two pages and one present only
		 * clears the one it lands on. Before RESUME and never after:
		 * afterwards Diatom is drawing, and this would be a second presenter. */
#if defined(PLATFORM_H700)
		/* On the H700 the paused frame, undimmed, not black, and landed
		 * before RESUME (2026-10-06):
		 *  - the page flips are the Mali driver's and land after the swap
		 *    returns: with Diatom back in 33 ms, a late one put the menu (or
		 *    black) on glass for a frame about every other Continue. So the
		 *    GPU finishes and its last flip lands first (two refreshes, as
		 *    Diatom's gl_quiesce);
		 *  - both buffers must still get a frame unlike the menu: Mali's
		 *    transaction elimination skips tiles it would write unchanged,
		 *    so the next pause's identical menu left Diatom's game showing;
		 *  - the paused frame is what Diatom shows next, so closing the menu
		 *    reads as the game appearing, with no black between. Diatom
		 *    wipes the pages it reuses, as the black frames did. */
		{
			void (*finish)(void) = (void (*)(void))SDL_GL_GetProcAddress("glFinish");
			int i;

			for (i = 0; i < 2; i++) {
				SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_NONE);
				SDL_SetRenderDrawColor(a->r, 0, 0, 0, 255);
				SDL_RenderClear(a->r);
				draw_paused_frame(a, c.bg);
				plat_present(a->r);
				SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
			}
			if (finish) finish();
			usleep(34000);
		}
#else
		present_black(a);
		present_black(a);
#endif
		/* SELECT in this menu opens Muse, so the music may have started or
		 * stopped while the game was paused. Said before the game moves, not
		 * a tick after it. */
		plat_resident_quiet(musec_playing());
		plat_resident_line("RESUME");
	}
#if defined(PLATFORM_H700)
	if (c.bg) SDL_DestroyTexture(c.bg);   /* drawn above, before RESUME */
#endif
	/* Nothing presents from here: the next frame on screen is the game's. */
}

/* Bring the resident emulator back.
 *
 * With one process per game a core that segfaults takes down that game and
 * nothing else. With a resident emulator it takes down the emulator, and every
 * launch for the rest of the session would fall back to the slow path -- until
 * the launcher itself exits and launch.sh starts a new one. So the launcher
 * starts one too, once the fallback game has given the display back. Never
 * while a game is running: two GL contexts and a launcher is one more than
 * this device should be asked for. */
static void respawn_resident(app *a)
{
	char elf[CFG_STR * 2], save[CFG_STR * 2], bios[CFG_STR * 2];
	char cores[CFG_STR * 2];
	char *argv[10];
	(void)a;

	if (plat_resident_ready()) return;

	/* The resident is Diatom. It maps every core in the directory it is given
	 * and keeps them (its ADR-0006), so the directory is the only thing handed
	 * over and nothing here changes when a system is added. */
	snprintf(elf, sizeof elf, "%s/diatom", P_ROOT);
	if (access(elf, X_OK) != 0) return;
	snprintf(save, sizeof save, "%s/Saves", P_CARD);
	snprintf(bios, sizeof bios, "%s/Bios", P_CARD);
	/* --cores here as well as in launch.sh. There are two places that start
	 * the resident emulator - the boot script and this - and a flag added to
	 * only one of them means every restart after the first silently loses it.
	 * That is exactly what happened: the log showed a premapped boot, then a
	 * dlopen on every launch once this path had restarted it. */
	snprintf(cores, sizeof cores, "%s/cores", P_ROOT);
	argv[0] = elf;
	argv[1] = (char *)"--socket";
	argv[2] = (char *)plat_resident_socket();
	argv[3] = (char *)"--cores";  argv[4] = cores;
	argv[5] = (char *)"--save";   argv[6] = save;
	argv[7] = (char *)"--system"; argv[8] = bios;
	argv[9] = NULL;
	fprintf(stderr, "resident emulator is gone, starting diatom\n");
	plat_spawn_detached(argv, child_env, P_ROOT);
}

/* Hand the display to a child and take it back when it ends: the one-shot
 * diatom fallback and native PICO-8 both. The launcher tears its own down
 * first because the child has to own the screen. */
/* Native PICO-8's in-game menu (plorpos-gkd.50.16). plat_run calls it with
 * pico8_64 frozen - off the screen on the GKD, still on it under this menu on
 * the Brick (plat_video_init_over_child, plorpos-reo.8) - so the display is
 * the launcher's for as long as the menu is up: its video comes up here and
 * goes again before the child is thawed, as run_alone does around the run. */
typedef struct {
	run_choice choice;
	int        frames;     /* built so far: the second build follows the first present */
	bool       in_splore;
} nm_ctx;

/* Also where the press-to-menu time is said: the second build comes right
 * after the first frame went to the screen. 100 ms is the budget (user,
 * 2026-10-02). */
static int nm_build(void *ctx, menu_row *rows, int max, const char **heading)
{
	nm_ctx *c = ctx;

	(void)max;
	if (++c->frames == 2)
		fprintf(stderr, "native menu: up %u ms after the press\n",
		        plat_run_menu_age_ms());
	*heading = NULL;       /* no title, as the in-game menu */
	return gm_native_rows(c->in_splore, rows);
}

/* Black: no picture of the game behind it - grim takes 260 ms, and the menu
 * has 100. plorpos-gkd.50.20 is the picture. */
static void nm_backdrop(app *a, void *ctx)
{
	(void)ctx;
	SDL_SetRenderDrawColor(a->r, 0, 0, 0, 255);
	SDL_RenderClear(a->r);
}

/* Power held here ends the GAME, which run_alone's caller then reads as the
 * power press it was - as gm_power does for a resident game. */
static menu_result nm_power(app *a, void *ctx)
{
	nm_ctx *c = ctx;

	(void)a;
	plat_note_power_pressed();
	c->choice = RUN_QUIT;
	return MENU_DONE;
}

static menu_result nm_key(app *a, void *ctx, in_button key, int sel)
{
	nm_ctx *c = ctx;

	if (key != IN_ACCEPT) return MENU_STAY;
	/* Splore is a Quit that launch() follows with Splore: the child is
	 * ended the same way, so plat_run need not know. */
	a->to_splore = sel == GMN_SPLORE;
	c->choice = sel == GMN_RESET ? RUN_RESET
	          : sel == GMN_QUIT || sel == GMN_SPLORE ? RUN_QUIT : RUN_CONTINUE;
	return MENU_DONE;
}

static run_choice native_menu(void *ctx)
{
	app *a = ctx;
	nm_ctx c = { RUN_CONTINUE, 0, a->pico8_splore };
	menu_style st = { 0 };

	if (!plat_video_init_over_child() || !plat_input_init()) return RUN_QUIT;
	a->r = plat_renderer();
	ui_init(a->r, P_FONT);
	a->menu_w = 0;
	/* The Menu press that opened this is not for it - see game_menu. */
	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);
	st.follow_tint = true;
	st.backdrop    = nm_backdrop;
	st.on_power    = nm_power;
	/* B and MENU are Continue, as in the in-game menu; SELECT is Muse, which
	 * the runner opens itself. Asked to quit with the menu up, the game
	 * goes too. */
	g_menu_closing = false;   /* sticky: see game_menu */
	if (menu_run(a, &st, nm_build, nm_key, &c) == MENU_LEFT_GONE &&
	    (want_quit || !a->running))
		c.choice = RUN_QUIT;
	g_menu_closing = false;

	free_all_textures(a);
	ui_quit();
	plat_input_quit();
	plat_video_quit();
	return c.choice;
}

/* plat_run's tick for native PICO-8: what on_game_tick does for Muse in a
 * resident game. Its queue moves on only when the launcher sends the next
 * PLAY, so without this an album stopped after the track playing at launch
 * (plorpos-gkd.50.23). And the game is silent while Muse plays, Diatom's
 * ADR-0032, followed live - pausing Muse brings the game back within a
 * tenth of a second (plorpos-gkd.50.17). */
static bool native_tick(void *ctx)
{
	(void)ctx;
	muse_poll();
#if defined(PLATFORM_H700)
	/* Where PICO-8's sound should be now, which pico8sdl.so follows: nowhere
	 * while Muse plays, else where Diatom would be sent. plorpos-7ny.35. */
	{
		aout_state s = aout_now();

		plat_child_audio(musec_playing() ? NULL : aout_device(&s), g_bt_link);
	}
#endif
	return musec_playing();
}

static int run_alone(app *a, char *const argv[], run_menu_fn on_menu,
                     run_tick_fn on_tick)
{
	int rc;

	anim_launch(a, 190);
	free_all_textures(a);
	ui_quit();
	plat_input_quit();
	plat_video_quit();

	rc = plat_run(argv, child_env, P_ROOT, on_menu, on_tick, a);

	if (!plat_video_init() || !plat_input_init()) { a->running = false; return rc; }
	a->r = plat_renderer();
	ui_init(a->r, P_FONT);
	a->menu_w = 0;   /* fonts reopened: remeasure the panel */
	prime_sys_window(a);
	prime_window(a, a->sys_cursor);
	/* The display is ours again and no game is running: the safe moment
	 * to put a resident emulator back, so the NEXT launch is fast. */
	respawn_resident(a);
	return rc;
}

/* ---- PICO-8 -------------------------------------------------------------
 *
 * The fake08 shelf can also run the owner's own pico8_64 (Bios/pico8_64 and
 * Bios/pico8.dat beside it). Native is PICO-8 itself - Splore, the real
 * runtime - at the cost of most of what diatom gives a game: no states, no
 * rewind, and an in-game menu of only Continue, Reset and Quit (native_menu;
 * on the Brick since plorpos-reo.8). */
#define SPLORE "Splore"

static bool is_pico8(const system_cfg *s) { return !strcmp(s->core, "fake08"); }

/* Splore is PICO-8's own cart browser, so it is native whatever the shelf
 * runs. Its entry has no extension, so no file on the card can be it. */
static bool is_splore(const system_cfg *s, const char *file)
{
	return is_pico8(s) && !strcmp(file, SPLORE);
}

/* Carts played in Splore, onto the shelf (plorpos-gkd.50.18). Splore keeps
 * every cart it shows in bbs/carts, browsing included, but writes a
 * temp-<id>.nfo - the cart's lid (id-revision) and title - only for one that
 * was run (watched 2026-10-02), so the .nfo is what marks a cart to keep.
 * Each revision is copied once, as <title>.p8.png beside the other carts:
 * from then on an ordinary cart, with its art, states and engine the shelf's.
 * Recorded per card, so a cart deleted from the shelf stays gone until a
 * newer revision is played, and a cart of the owner's own by that name is
 * never written over. How many were copied. */
static int splore_keep(const char *folder)
{
	char dir[CFG_STR * 2], nfo[CFG_STR * 3], line[256];
	char from[CFG_STR * 3], to[CFG_STR * 3];
	char lid[64], title[128], name[128], key[96], had[96], hlid[64], rec[96];
	struct dirent *e;
	struct stat st;
	DIR *d;
	int n = 0;

	snprintf(dir, sizeof dir, "%s/Saves/pico-8/bbs/carts", P_CARD);
	if (!(d = opendir(dir))) return 0;
	while ((e = readdir(d))) {
		size_t len = strlen(e->d_name), k = 0;
		FILE *f;

		if (len <= 9 || strncmp(e->d_name, "temp-", 5) ||
		    strcmp(e->d_name + len - 4, ".nfo"))
			continue;
		snprintf(nfo, sizeof nfo, "%s/%s", dir, e->d_name);
		if (!(f = fopen(nfo, "r"))) continue;
		lid[0] = title[0] = '\0';
		while (fgets(line, sizeof line, f)) {
			line[strcspn(line, "\r\n")] = '\0';
			if (!strncmp(line, "lid:", 4))
				snprintf(lid, sizeof lid, "%.63s", line + 4);
			else if (!strncmp(line, "title:", 6))
				snprintf(title, sizeof title, "%.127s", line + 6);
		}
		fclose(f);
		if (!*lid || strchr(lid, '/')) continue;

		/* Keyed by the id without the revision: the .nfo's own name.
		 * Recorded as "<lid> <.nfo mtime>": Splore rewrites the .nfo each
		 * time the cart is played, so a newer one is a cart played again -
		 * one deleted from the shelf comes back for that, and only that
		 * (plorpos-gkd.50.25). A record of the lid alone is from before:
		 * taken as seen now, so carts deleted then stay gone. */
		long long hm = -1;
		bool again;

		if (stat(nfo, &st) != 0) continue;
		snprintf(key, sizeof key, "splore.%.*s", (int)(len - 9), e->d_name + 5);
		snprintf(rec, sizeof rec, "%s %lld", lid, (long long)st.st_mtime);
		db_get_str(db_lib(), key, had, sizeof had, "");
		hlid[0] = '\0';
		sscanf(had, "%63s %lld", hlid, &hm);
		again = !strcmp(hlid, lid);
		if (again && hm < 0) { db_set_str(db_lib(), key, rec); continue; }
		if (again && (long long)st.st_mtime <= hm) continue;
		snprintf(from, sizeof from, "%s/%s.p8.png", dir, lid);
		if (access(from, R_OK) != 0) continue;

		/* The title as a filename the card takes: no path or FAT-reserved
		 * characters, no trailing dot or space. The lid when nothing is left. */
		for (const char *c = title; *c && k < sizeof name - 1; c++)
			if ((unsigned char)*c >= ' ' && !strchr("/\\:*?\"<>|", *c))
				name[k++] = *c;
		while (k && (name[k - 1] == ' ' || name[k - 1] == '.')) k--;
		name[k] = '\0';
		if (!k) snprintf(name, sizeof name, "%s", lid);
		snprintf(to, sizeof to, "%s/%s/%s.p8.png", P_ROMS, folder, name);

		/* Played again with it still on the shelf - or still the card's own -
		 * there is nothing to bring back. */
		if ((!*had || again) && access(to, F_OK) == 0) {
			if (!*had)
				fprintf(stderr, "splore: %s is the card's own, left as it is\n", to);
		} else {
			copy_file(from, to);
			fprintf(stderr, "splore: %s%s -> %s\n", lid, again ? " (played again)" : "", to);
			n++;
		}
		db_set_str(db_lib(), key, rec);
	}
	closedir(d);
	return n;
}

static void pico8_bin(char *out, size_t n)
{
	snprintf(out, n, "%s/Bios/pico8_64", P_CARD);
}

/* First on the shelf, before any cart, whenever native PICO-8 is there to
 * run it. Without Bios/pico8_64 there is no Splore, only carts for fake08
 * (plorpos-gkd.32.7). */
static void splore_add(const system_cfg *s, game_list *l)
{
	char bin[CFG_STR * 2];
	game_entry *items;

	if (!is_pico8(s)) return;
	pico8_bin(bin, sizeof bin);
	if (access(bin, X_OK) != 0) return;
	items = realloc(l->items, (size_t)(l->count + 1) * sizeof *items);
	if (!items) return;
	memmove(items + 1, items, (size_t)l->count * sizeof *items);
	memset(items, 0, sizeof *items);
	snprintf(items[0].name, sizeof items[0].name, "%s", SPLORE);
	snprintf(items[0].title, sizeof items[0].title, "%s", SPLORE);
	snprintf(items[0].file, sizeof items[0].file, "%s", SPLORE);
	l->items = items;
	l->count++;
}

/* -home is where PICO-8 keeps everything it writes - config, log, cart data,
 * Splore's downloads - and -desktop its screenshots, which by default would
 * land beside the carts and show on the shelf as .png carts. Both under
 * Saves/pico-8, made here because PICO-8 makes neither.
 *
 * -root_path is the shelf's folder for a cart, where a multi-cart game
 * load()s its siblings from. Not for Splore: it writes a scratch Splore.png
 * into its root while it works, and one left by a quit mid-write was a
 * second "Splore" on the shelf (2026-10-02). PICO-8's own carts folder
 * under -home instead. */
/*
 * The shelf's Display Mode, as near as PICO-8's own flags come. Stretch is
 * a draw_rect over the whole output, measured off the renderer before
 * run_alone takes it down, so it is the panel's real size on either device.
 * Integer is pixel_perfect. Aspect is no flag at all, PICO-8's own fit.
 * Neither flag is saved in config.txt, so a run that
 * passes none is not left with the last one's. */
static int run_pico8(app *a, const char *bin, const char *folder,
                     const char *rom, bool splore, const char *dmode)
{
	char home[CFG_STR * 2], desk[CFG_STR * 2 + 16], root[CFG_STR * 2];
	char rect[48], preload[CFG_STR * 2 + 16], path[1024];
	char *argv[18];   /* 18 at most: env, preload, PATH, AUDIODEV, the binary, 12 flags, NULL */
#if defined(PLATFORM_H700)
	char audiodev[160];
	aout_state as = aout_now();
	const char *dev = aout_device(&as);
#endif
	int n = 0, ow = 0, oh = 0, flags;

	snprintf(home, sizeof home, "%s/Saves/pico-8", P_CARD);
	snprintf(desk, sizeof desk, "%s/desktop", home);
	mkdir(home, 0755);
	mkdir(desk, 0755);
	if (splore) {
		snprintf(root, sizeof root, "%s/carts", home);
		mkdir(root, 0755);
	} else {
		snprintf(root, sizeof root, "%s/%s", P_ROMS, folder);
	}

	/* Through env(1) when the device needs a library preloaded into PICO-8
	 * alone, or a PATH of its own; env execs it, so the pid plat_run signals
	 * is pico8_64's. */
	if (plat_pico8_preload || plat_pico8_path)
		argv[n++] = (char *)"/usr/bin/env";
	if (plat_pico8_preload) {
		snprintf(preload, sizeof preload, "LD_PRELOAD=%s", plat_pico8_preload);
		argv[n++] = preload;
	}
	if (plat_pico8_path) {
		const char *was = getenv("PATH");
		snprintf(path, sizeof path, "PATH=%s:%s", plat_pico8_path,
		         was ? was : "/usr/bin:/bin");
		argv[n++] = path;
	}
#if defined(PLATFORM_H700)
	/* To the headset when that is where the sound goes, by its PCM name in
	 * .asoundrc (bt-alsa.sh) - SDL opens AUDIODEV as its default device.
	 * Chosen once: pico8_64 opens its device at start and keeps it, so a
	 * headset that comes or goes mid-session is not followed. Not while Muse
	 * plays: a bluealsa PCM takes one opener, Muse holds it, and PICO-8 is
	 * quiet then anyway (child_quiet). plorpos-7ny.35. */
	if (dev[0] && !musec_playing() && (plat_pico8_preload || plat_pico8_path)) {
		snprintf(audiodev, sizeof audiodev, "AUDIODEV=%s", dev);
		argv[n++] = audiodev;
		fprintf(stderr, "pico8_64: audio %s\n", dev);
	}
#endif
	argv[n++] = (char *)bin;
	argv[n++] = (char *)"-home";      argv[n++] = home;
	argv[n++] = (char *)"-root_path"; argv[n++] = root;
	argv[n++] = (char *)"-desktop";   argv[n++] = desk;
	argv[n++] = (char *)"-joystick";  argv[n++] = (char *)"0";
	flags = n;
	if (!strcmp(dmode, "stretch") &&
	    SDL_GetRendererOutputSize(a->r, &ow, &oh) == 0 && ow > 0 && oh > 0) {
		snprintf(rect, sizeof rect, "0,0,%d,%d", ow, oh);
		argv[n++] = (char *)"-draw_rect"; argv[n++] = rect;
	} else if (!strcmp(dmode, "integer")) {
		argv[n++] = (char *)"-pixel_perfect"; argv[n++] = (char *)"1";
	}
	fprintf(stderr, "pico8_64: display %s %s %s\n", dmode,
	        n > flags ? argv[flags] : "-", n > flags ? argv[flags + 1] : "");
	a->pico8_splore = splore;
	a->to_splore = false;
#if defined(PLATFORM_H700)
	plat_child_audio_reset();
#endif
	if (splore) argv[n++] = (char *)"-splore";
	else { argv[n++] = (char *)"-run"; argv[n++] = (char *)rom; }
	argv[n] = NULL;
	return run_alone(a, argv, native_menu, native_tick);
}

/* The firmware file a disc on this shelf boots with. One per shelf, except a
 * Sega CD on the Genesis shelf: genesis_plus_gx loads bios_CD_E, _J or _U by
 * the disc's region, which it reads from the security code at 0x20B of the
 * first sector (core/loadrom.c get_region). Same rule here, so the panel names
 * the file the core would have asked for. An unreadable header falls back to
 * systems.cfg's name, and the core says what it says. (plorpos-gkd.71) */
static void disc_bios_for(const system_cfg *s, const char *rom, char *out, size_t n)
{
	unsigned char head[0x20C];

	snprintf(out, n, "%s", s->disc_bios);
	if (strcmp(s->tag, "MD") || !cd_read_head(rom, head, sizeof head)) return;
	snprintf(out, n, "bios_CD_%c.bin",
	         head[0x20B] == 0x64 ? 'E' : head[0x20B] == 0xA1 ? 'J' : 'U');
}

static void launch(app *a)
{
	sysview *v = &a->view[a->sys_cursor];
	/* The game's system, not the shelf's. Launching a favorite off the
	 * Favorites shelf otherwise loads whatever core the shelf claims, which
	 * is none, and looks for the ROM under a folder that does not exist. */
	int o = shelf_owner(a, a->sys_cursor, v->cursor);
	const system_cfg *s = &a->sys.systems[o];
	char core[CFG_STR * 2], elf[CFG_STR * 2], rom[LIB_PATH * 2];
	char st[LIB_PATH * 2], pv[LIB_PATH * 2], apv[LIB_PATH * 2];
	char save[CFG_STR * 2], bios[CFG_STR * 2];
	char active[LIB_PATH * 2] = "";
	char set[LIB_PATH * 2];
	int  console = 0;
	bool first_play = false;
	/* 18 fixed entries plus NULL, then two per core option. Sized off the
	 * loader's own cap so the two cannot drift apart: the previous 20 was
	 * already 18 full, and a silent bound check would have dropped every
	 * option rather than failing loudly. */
	char *argv[20 + 2 * 32 + 6];   /* and the palette's two (plorpos-gkd.76),
	                                * and --disc (plorpos-gkd.47) */
	char palp[64] = "", palc[64] = "";
	char discs[16] = "";
	int  disc;   /* 0-based; -1 = the core's choice (plorpos-gkd.47) */
	bool resident = false, want_menu, native;
	char pico8[CFG_STR * 2];
	int n = 0;

	if (v->list.count == 0) return;

	snprintf(core, sizeof core, "%s/cores/%s_libretro.so", P_ROOT, s->core);
	snprintf(elf, sizeof elf, "%s/diatom", P_ROOT);
	/* Captured once, beside the path it goes into: play time is keyed on the
	 * launch path the same way favorites are, and reading the cursor again
	 * further down would be reading it at a different moment. */
	const char *romfile = v->list.items[v->cursor].file;

	snprintf(rom, sizeof rom, "%s/%s/%s", P_ROMS, s->folder, romfile);
	/* A Game Boy game always says its palette, Auto included, so a value an
	 * older card stored for the whole system cannot decide it. */
	{
		int pal = palette_of(a, o, &v->list.items[v->cursor]);
		if (pal >= 0) gbpal_opts(pal, palp, sizeof palp, palc, sizeof palc);
	}
	disc = disc_of(a, o, &v->list.items[v->cursor]);
	/* Each shelf's saves in a folder named as its Roms folder, so the same
	 * title on two shelves cannot share one .srm, and a core's own files
	 * (memory cards, fbneo/) sit with their system (plorpos-aev; the Brick
	 * too since plorpos-reo.3). PICO-8's is Saves/pico-8 on exFAT, which
	 * ignores case: fake08 and native PICO-8 share cdata/, in one format. */
	snprintf(save, sizeof save, "%s/Saves/%s", P_CARD, s->folder);
	mkdir(save, 0755);
	snprintf(bios, sizeof bios, "%s/Bios", P_CARD);

	/* A disc that needs firmware, before anything tries to run it.
	 *
	 * Diatom checks this too (its ADR-0017) and answers bios_missing, but that
	 * answer has never been reachable: it acts on a `firmware=` key the
	 * launcher has never sent. So the core was asked to load a CD with no
	 * System Card, refused it the way it refuses a corrupt ROM, and the shelf
	 * came back with nothing said - which is a long way from "needs
	 * Bios/syscard3.pce".
	 *
	 * Disc images only. A HuCard needs no System Card, and refusing every
	 * cartridge on the shelf because a CD would have needed one is a worse
	 * failure than the one being fixed. */
	if (s->disc_bios[0] && lib_is_disc(v->list.items[v->cursor].file)) {
		char fw[CFG_STR * 3], name[CFG_STR];

		disc_bios_for(s, rom, name, sizeof name);
		snprintf(fw, sizeof fw, "%s/%s", bios, name);
		if (access(fw, R_OK) != 0) {
			char msg[CFG_STR + 32];

			snprintf(msg, sizeof msg, "needs Bios/%s", name);
			wait_panel(a, v->list.items[v->cursor].title, msg);
			SDL_Delay(2200);
			plat_input_flush();
			memset(&a->in, 0, sizeof a->in);
			return;
		}
	}

	/* Native PICO-8 with no pico8_64 is said, the same way, rather than
	 * quietly run on fake08: the player asked for the real one. */
	/* The owner's choice, not the shelf's: a favorite runs the way it
	 * would from PICO-8's own shelf. */
	native = is_splore(s, romfile) || (is_pico8(s) && a->view[o].native);
	pico8_bin(pico8, sizeof pico8);
	if (native && access(pico8, X_OK) != 0) {
		wait_panel(a, v->list.items[v->cursor].title, "needs Bios/pico8_64");
		SDL_Delay(2200);
		plat_input_flush();
		memset(&a->in, 0, sizeof a->in);
		return;
	}

	/* The autosave story, by path rather than by convention: the state and the
	 * preview live beside each other, the launch hands both over, and a game
	 * always comes up where it was left. */
	state_path(a, o, &v->list.items[v->cursor], st, sizeof st);
	/* The scratch, not the Auto card - see pause_preview_path. The card is
	 * written from this only when the game actually ends, below. */
	pause_preview_path(pv, sizeof pv);
	persist_dir_ensure(a, o);

	/* Achievements, if this game has any. Most of a library does not, and that
	 * is not a failure: chv_load says so by returning false and everything
	 * below carries on with console 0 and no set, which is what Diatom reads
	 * as "this game has none". */
	{
		chv_path(P_ROMS, s->folder, v->list.items[v->cursor].name,
		         set, sizeof set);
		/* Every launch, not just a first play: this is what a late fetch is
		 * checked against, and it has to name the game on screen now rather
		 * than the last one that went looking for a set. */
		snprintf(g_game_set, sizeof g_game_set, "%s", set);

		/* The async slot, cleared of an account answer from the last game that
		 * never landed, before this launch asks anything of its own. And no
		 * watch list to trim until this game has handed one over. */
		sync_abandon();
		g_watch_ms = 0;

		/* Fetch it if this game has never been played here. Only then: a
		 * cached set costs nothing and this is the launch path, so the delay
		 * is paid once per game rather than every time. Failing is ordinary -
		 * offline, not signed in, or a game RetroAchievements has never seen -
		 * and the launch carries on without. */
		/* Never played here, so there is no set to hand over. The game starts
		 * anyway and the set is found behind it - see ra_fetch_begin. Nothing
		 * in front of the launch. */
		first_play = ra_signed_in() && access(set, R_OK) != 0 && net_online();

		if (chv_load(set)) {
			/* Reconcile with the account BEFORE deciding what to watch.
			 * Without this the launcher filters against what this device
			 * happens to have seen, which is not the same question and does
			 * not look different: measured 2026-08-29 as 3 of 40 for Contra
			 * against the site's own 13. */
			/* The account's answer, without the launch waiting for it - read
			 * while the game runs, see sync_poll. On a first play there is
			 * nothing to ask about yet; that question is asked once the set
			 * arrives. */
			if (!first_play) sync_begin(chv_game(), set);


			/* No "startsession" request, deliberately. It drives the "currently
			 * playing" indicator on the website and nothing on the device, and
			 * it is another 320ms request - which is the exact trade this whole
			 * path exists to refuse. */

			chv_active_path(active, sizeof active);
			if (chv_write_active(active)) {
				console = chv_console();
			} else {
				/* Everything in the set is already earned. Nothing to watch,
				 * and sending an empty file would have Diatom log a set with
				 * no achievements in it every launch. */
				active[0] = '\0';
			}
		}
	}

	remember_place(a);
	/* Before the launch, not after: a device that loses power during the load
	 * was still playing this game. */
	playing_set(a);

	/* Read and cleared together. Leaving it set would have the NEXT game
	 * launched in this session open with its menu up too, which is the sort
	 * of thing that looks like a haunting rather than a bug. */
	want_menu = a->resume_menu;
	a->resume_menu = false;

	/* Whether this game should be heard - quiet while Muse plays, Diatom's
	 * ADR-0032. Stated before the RUN goes out, which sends it. */
	plat_resident_quiet(musec_playing());

	if (!native && plat_resident_ready_wait()) {
		/* The emulator is already up, holding its context and every core,
		 * so this is ~200ms rather than ~1100. Nothing here is torn
		 * down -- this process keeps its own context through the whole game,
		 * which is why coming back is a frame rather than a second and a
		 * half. The two do overlap for the length of this animation, while
		 * the game loads behind it; after that only the emulator draws,
		 * because this process is blocked - except when the player opens
		 * the in-game menu, which is drawn HERE now, over the frame the
		 * emulator hands us on the way into its pause. */
		aout_before_launch();
		char shf[1024];
		plat_game g = {
			.tag = s->tag, .core = core, .rom = rom,
			.resume = st, .exit_state = st, .preview = pv, .save = save,
			.console = console, .cheevos = active[0] ? active : NULL,
			.opts = { palp[0] ? palp : NULL, palc[0] ? palc : NULL },
			.disc = disc + 1,
			.shader = shader_fields(a, o, shf, sizeof shf),
		};
		if (plat_resident_send(&g)) {
			int r;

			/* The list Diatom was just handed, for an early answer to trim. */
			if (active[0]) {
				g_watch_ms = plat_now_ms();
				snprintf(g_watch_active, sizeof g_watch_active, "%s", active);
			}

			/* Play time starts here, and costs a clock read. Nothing is
			 * opened or written - the launch path does no I/O for this, on
			 * purpose, because a millisecond bought here is the first of
			 * several. See stats.h. */
			stats_begin(s->tag, romfile, plat_now_ms());

			/* Straight after RUN and the levels, and for the same reason: the
			 * mode is Diatom's own global and survives from the last game, so
			 * a system that has never been set would otherwise inherit
			 * whatever the previous one chose. Ordered on the same socket, so
			 * it lands before the first frame. */
			plat_resident_line("SETDISPLAY\tmode=%s",
			                   DMODES[a->view[o].dmode].name);

			/* No idle anything during play: NextUI disables autosleep for
			 * the whole of a running game (minarch.c's PWR_disableAutosleep)
			 * and turns it back on only in the in-game menu, which is the
			 * launcher's own idle_due. Said rather than assumed, because
			 * Diatom's clock is its own global and outlives any one game. */
			plat_resident_line("SETIDLE\tms=0");

			/* The first play of this game: nothing was cached, so there is a
			 * set to go and find. Out here and not inside the chv_load branch
			 * above, which is the mistake the first version made - that
			 * branch is precisely the one a first play does not take.
			 *
			 * Hashing is local work and happens after RUN, so it overlaps the
			 * game's own startup rather than delaying it. The two requests
			 * then run behind the game and on_game_tick hands the set over
			 * when they land. */
			if (first_play) {
				char h[33];

				if (ra_hash_rom(rom, s->tag, h)) {
					chv_active_path(g_pending_active, sizeof g_pending_active);
					snprintf(g_pending_set, sizeof g_pending_set, "%s", set);
					ra_fetch_begin(h, set);
				}
			}
			/* Coming back from a shutdown: the game loads and the menu is
			 * already up, so nothing is handed control of a game the player
			 * may not have meant to resume. Ordered on the same socket, so it
			 * lands before the first frame the player could act on. */
			if (want_menu) plat_resident_line("PAUSE");
			/* No launch animation, and it is a display-safety rule, not a
			 * taste call: Diatom presents through fbdev, this process
			 * through GL, and the handoff spike's one invariant is that
			 * they never present concurrently - a 190ms overlap that is
			 * harmless GL-on-GL is the exact case that wedges the display
			 * engine when one side is fbdev. A warm launch is 26 to 44 ms from
			 * RUN to RUNNING, so there is nothing to animate over anyway; the
			 * shelf simply holds until the game's first frame replaces it. */
			a->game_on = true;
			for (;;) {
				r = plat_resident_wait();
				/* A tap mid-game: Diatom paused for it (platform.c), and
				 * nothing was drawn over its pages, so it can simply be
				 * resumed - or, if there was no suspend to escalate into,
				 * stopped the way a held button stops it. */
				if (r == RES_PAUSED && plat_resident_sleep_asked()) {
					if (sleep_cycle(a)) plat_resident_line("RESUME");
					else {
						plat_note_power_pressed();
						plat_resident_line("STOP");
					}
					continue;
				}
				if (r == RES_PAUSED) { game_menu(a); continue; }
				break;
			}
			a->game_on = false;
			resident = (r == RES_EXIT);

			/* The Auto card's picture, written ONLY here - at the same moment
			 * Diatom wrote the state it belongs to, so the two cannot drift.
			 * Between exits the card keeps the previous session's frame, which
			 * is exactly what resuming would give you. */
			if (resident) {
				char ap[LIB_PATH * 2];

				preview_path(a, o, &v->list.items[v->cursor],
				             ap, sizeof ap);
				copy_file(plat_resident_last_preview(), ap);
			}

			/* The game is over, so this is the one write that is synced -
			 * on the path where the player is waiting for the shelf rather
			 * than for a game to start. */
			stats_end(NULL, plat_now_ms());

			/* Whatever was earned is sent now, behind the shelf rather than in
			 * front of it - see ra_flush_start. It used to be sent right here,
			 * blocking, on the reasoning that the wait loop above is the power
			 * button's watchdog and so the first safe moment was after it. True
			 * of that loop, and not the only place the watchdog matters: this
			 * spot is between that loop and the shelf's, so the button was
			 * read by neither while it waited. Whatever will not send stays
			 * queued. */
			/* After the account's answer, so anything it already held is
			 * settled before deciding what is owed - otherwise the flush would
			 * cheerfully submit a dozen duplicates. The answer has usually been
			 * in since half a second into the game. If it is still on its way,
			 * the sending waits for it rather than the shelf - see sync_poll.
			 *
			 * The game is passed rather than read later: by the time a waiting
			 * send starts, the info screen may have loaded another set. */
			g_watch_ms = 0;
			sync_poll();
			if (g_sync_game) {
				g_sync_flush = true;
				snprintf(g_sync_rom, sizeof g_sync_rom, "%s", rom);
				snprintf(g_sync_tag, sizeof g_sync_tag, "%s", s->tag);
				g_sync_cur = chv_game();
				fprintf(stderr, "ra: the account's answer is still on its way; "
				                "sending waits for it\n");
			} else {
				ra_flush_start(rom, s->tag, chv_game());
			}

			/* RES_DEAD means it stopped answering -- it died, or the game
			 * never started. Say so by falling through to the path that
			 * runs it the slow way, rather than fading the shelf back up
			 * as though it had run. */
			if (!resident)
				fprintf(stderr, "resident emulator stopped answering, falling back\n");
		} else {
			fprintf(stderr, "resident emulator did not answer, falling back\n");
		}
	}

	if (native) {
		/* Played time counts here too, begun and ended around plat_run,
		 * less any sleep. No stats_tick checkpoints while it runs, so a
		 * power cut mid-session loses that session's time. */
		/* romfile points into the shelf, which the rescan below frees. */
		char from[LIB_PATH];
		bool splore = is_splore(s, romfile);

		snprintf(from, sizeof from, "%s", romfile);
		for (;;) {
			stats_begin(s->tag, splore ? SPLORE : from, plat_now_ms());
			fprintf(stderr, "pico8_64 exited %d\n",
			        run_pico8(a, pico8, s->folder, rom, splore,
			                  DMODES[a->view[o].dmode].name));
			stats_asleep(plat_run_asleep_ms());
			stats_end(NULL, plat_now_ms());
			if (!a->running) return;
			/* The cart's menu asked for Splore (plorpos-gkd.50.21). */
			if (!a->to_splore) break;
			fprintf(stderr, "pico8_64: %s -> Splore\n", from);
			splore = true;
		}
		/* Splore is left on the shelf it was opened from, now with the
		 * carts played in it. The rescan rebuilds the views, so v again,
		 * and puts every cursor first - on Splore, where a Splore opened
		 * from the shelf comes back to. One opened from a cart's menu
		 * comes back to that cart. */
		if (splore && splore_keep(s->folder) > 0) {
			rescan_all(a);
			v = &a->view[a->sys_cursor];
			for (int i = 0; i < v->list.count; i++)
				if (!strcmp(v->list.items[i].file, from)) { v->cursor = i; break; }
			cf_reset(&v->cf, v->cursor);
		}
	} else if (!resident && !want_quit) {
		/* One game per process, the old way: the fallback for a resident that
		 * is missing or has died. Diatom standalone IS the one-shot mode -
		 * same binary, no socket - so the fallback stopped being a different
		 * emulator and became the same one held differently. It has to take
		 * the display, so run_alone tears this side's down first.
		 *
		 * Not when quitting: the resident died because the launcher is ending
		 * the game, and starting it again would swallow the quit
		 * (plorpos-gkd.58). */
		argv[n++] = elf;
		argv[n++] = (char *)"--core";            argv[n++] = core;
		argv[n++] = (char *)"--rom";             argv[n++] = rom;
		argv[n++] = (char *)"--save";            argv[n++] = save;
		argv[n++] = (char *)"--system";          argv[n++] = bios;
		argv[n++] = (char *)"--display";
		argv[n++] = (char *)DMODES[a->view[o].dmode].name;
		argv[n++] = (char *)"--load-state";      argv[n++] = st;
		argv[n++] = (char *)"--state-on-exit";   argv[n++] = st;
		if (disc >= 0) {
			snprintf(discs, sizeof discs, "%d", disc);
			argv[n++] = (char *)"--disc";        argv[n++] = discs;
		}
		/* The Auto card's own path here, not the scratch the resident mode
		 * uses. Standalone has no in-game menu - the launcher is torn down -
		 * so nothing can overwrite it mid-session, and it is written once at
		 * exit beside the state. The resident path needs the indirection
		 * because a pause writes the preview too; this one does not. */
		preview_path(a, o, &v->list.items[v->cursor], apv, sizeof apv);
		argv[n++] = (char *)"--preview-on-exit"; argv[n++] = apv;
		/* Same opinions as the resident path gets over SETOPT, so a game plays
		 * the same whether the resident was up or the fallback ran it. */
		{
			int ci;
			for (ci = 0; ci < plat_coreopt_count(s->tag) && n < (int)(sizeof argv / sizeof argv[0]) - 3; ci++) {
				argv[n++] = (char *)"--core-option";
				argv[n++] = (char *)plat_coreopt(s->tag, ci);
			}
		}
		if (palp[0]) {
			argv[n++] = (char *)"--core-option"; argv[n++] = palp;
			argv[n++] = (char *)"--core-option"; argv[n++] = palc;
		}
		argv[n] = NULL;
		fprintf(stderr, "diatom exited %d\n", run_alone(a, argv, NULL, NULL));
		if (!a->running) return;
	}

	t_back0 = plat_now_ms();
	present_black(a);

	/* The game has just written a fresh autosave preview. Throw the card's
	 * texture away so it picks that up - but only if the preview is what the
	 * card is drawn FROM.
	 *
	 * Box art comes first in the order a card finds its picture, so for a game
	 * that has it this used to rebuild the card from a file that had not
	 * changed, into a pixel-identical texture. That was always wasted work, and
	 * it was invisible while decoding blocked the frame: the card was back
	 * before anything drew. Since decoding moved to a worker, the card is
	 * genuinely absent for a few frames, and the one you just quit popped in
	 * while every card beside it was already there. The question was never
	 * "did the preview change", it is "is this card showing the preview". */
	{
		game_entry *g = &v->list.items[v->cursor];
		int o = shelf_owner(a, a->sys_cursor, v->cursor);

		char art[LIB_PATH * 3];

		if (v->tex[v->cursor] &&
		    !card_art_path(&a->sys.systems[o], g, art, sizeof art)) {
			SDL_DestroyTexture(v->tex[v->cursor]);
			v->tex[v->cursor] = NULL;
		}
	}

	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);
	/* The idle clock is not restarted here, though it has been stopped for
	 * the whole session. idle_due notices the gap on its own, which is what
	 * stops this from being a list of places to remember. */

	/* This one exits without power_off(), so it darkens the lights itself. */
	if (access(TORTOS_POWEROFF_FLAG, F_OK) == 0) {
		plat_leds_off();
		a->running = false;
		return;
	}
	/* Power was pressed during the game. The emulator no longer handles that
	 * key itself, so the press arrived here. */
	if (plat_run_power_pressed()) { power_off(a); return; }

	/* Past both power checks, so this is a real return to the shelf rather
	 * than a shutdown. Everything above leaves the marker standing, which is
	 * what makes the next boot able to tell them apart. */
	playing_clear();

	/* Straight to the shelf, no fade. The card decode inside this render is
	 * the only real cost left on the way back, and a fade laid over the top of
	 * it is time spent easing in a picture the player has already been looking
	 * at all the way up to the moment they quit. render() is right here: it
	 * draws and presents exactly once. */
	render(a);
	fprintf(stderr, "exit: back in %u ms\n", plat_now_ms() - t_back0);
	/* Seventy-nine open/write/close round trips through sysfs, measured at
	 * 35-52ms. Nothing about them is urgent and the panel is what the player
	 * is waiting on, so they happen once the shelf is up rather than while it
	 * is still black. The two paths above that leave without reaching here do
	 * it themselves. */
	plat_leds_off();
}

/* ---------- input --------------------------------------------------------- */

static void enter_system(app *a)
{
	sysview *v = &a->view[a->sys_cursor];
	/* Muse too: its cards are albums, and opening one is the games shelf's
	 * own A - see update_games. */
	if (v->list.count == 0) return;
	a->screen = SCREEN_GAMES;
	cf_reset(&v->cf, v->cursor);
	prime_window(a, a->sys_cursor);
}

/* Start on a system's cards when the systems row moves onto it, not when A is
 * pressed.
 *
 * Entering used to be the first time anything asked for a system's covers, so
 * the games shelf drew with empty slots until they arrived. Measured
 * 2026-09-14, cold after a reboot, across six systems: the focused card 70-160ms
 * after A, the whole row 95-177ms. Short, and visible. Leaving a system keeps
 * the cards around its cursor, which is why only the first entry showed it.
 *
 * Starting here gives the decode however long it takes to reach the system and
 * press A, and that is enough: re-measured the same way, four systems entered
 * after a reboot all had every card already loaded on arrival.
 *
 * Whatever was queued for the system just passed is dropped first, so a
 * held direction does not decode a row for every system it crosses, and the one
 * the row stops on is not left waiting behind them.
 *
 * The visible row only; entering asks for two more either side. They stay after
 * the row moves on, which is less than already stays: a system entered and left
 * keeps nine. */
static void load_ahead(app *a)
{
	texload_forget();
	prime_toward(a, a->sys_cursor, a->view[a->sys_cursor].cursor);
}

static void update_systems(app *a)
{
	int n = a->sys.count;

	/* No systems means the modulo below divides by zero. Reachable the moment
	 * empty systems started being hidden: a card with no ROMs on it, or ROMs
	 * one directory too deep, now leaves nothing on the shelf and the first
	 * press of left or right took the launcher down with SIGFPE. */
	if (n <= 0) return;

	/* The direction is carried through, not inferred from the cursor: on a
	 * shelf of two, moving from either card to the other is one step in BOTH
	 * directions, and only the press says which. */
	/* The d-pad axis follows the shelf. Vertically the list runs down the
	 * screen, first at the top, so down advances - see cards.h. */
	in_button back = CARD_DIRS[g_dir].vertical ? IN_UP : IN_LEFT;
	in_button fwd  = CARD_DIRS[g_dir].vertical ? IN_DOWN : IN_RIGHT;
	int dir = 0;

	if (in_repeat(&a->in, back)) { a->sys_cursor = (a->sys_cursor - 1 + n) % n; dir = -1; }
	if (in_repeat(&a->in, fwd))  { a->sys_cursor = (a->sys_cursor + 1) % n; dir = +1; }
	if (dir) load_ahead(a);
	/* Muse's card opens Muse, the one SELECT opens - see update_games. */
	if (a->in.pressed[IN_ACCEPT]) {
		if (is_muse(&a->sys.systems[a->sys_cursor])) muse_open(a, NULL, NULL);
		else enter_system(a);
	}
	cf_set_cursor_dir(&a->cf_sys, a->sys_cursor, n, dir);
}

/* The first alphanumeric character of a name, upper-cased, or '\0' for a name
 * with none at all - which groups those few together rather than giving each
 * one a group of its own. Leading articles and punctuation are deliberately
 * NOT skipped: lib_scan sorts the shelf with strcasecmp on this same string,
 * so grouping that disagreed with the order the cards are drawn in would make
 * the jump land somewhere that looks arbitrary. "The Legend of Zelda" files
 * under T here because it sits under T on the shelf. */
static char shelf_initial(const char *s)
{
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c >= 'a' && c <= 'z') return (char)(c - 32);
		if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return (char)c;
	}
	return '\0';
}

/* The first game sharing idx's initial, walking backwards from it. */
/* What the letter jump reads: the title, or on Muse's shelf the artist. */
static const char *shelf_key(const sysview *v, int i)
{
	return v->jump_by_name ? v->list.items[i].name : v->list.items[i].title;
}

static int shelf_group_start(sysview *v, int idx)
{
	int n = v->list.count, i = idx;
	char c = shelf_initial(shelf_key(v, idx));

	for (;;) {
		int p = (i - 1 + n) % n;
		if (p == idx) return idx;             /* one initial, the whole shelf */
		if (shelf_initial(shelf_key(v, p)) != c) break;
		i = p;
	}
	return i;
}

/* Up and down cross the shelf one initial at a time, which on a long library
 * is the difference between forty presses and two. Down lands on the first
 * game of the next initial. Up lands on the first game of THIS one, and moves
 * to the previous initial only when the cursor is already there - so from any
 * group's first game, up and down are exact inverses. */
static int shelf_letter_jump(sysview *v, int dir)
{
	int n = v->list.count, i, cur = v->cursor;
	char c0 = shelf_initial(shelf_key(v, cur));

	if (n <= 1) return cur;
	if (dir > 0) {
		for (i = 1; i < n; i++) {
			int k = (cur + i) % n;
			if (shelf_initial(shelf_key(v, k)) != c0) return k;
		}
		return cur;                           /* every name starts alike */
	}
	i = shelf_group_start(v, cur);
	if (i != cur) return i;
	/* Already at the top of the group. If what precedes it shares the initial
	 * then the shelf is one group and there is nowhere to go - the same answer
	 * down gives, rather than shuffling back by one. */
	i = (cur - 1 + n) % n;
	if (shelf_initial(shelf_key(v, i)) == c0) return cur;
	return shelf_group_start(v, i);
}

static void update_games(app *a)
{
	sysview *v = &a->view[a->sys_cursor];
	int n = v->list.count;
	if (n <= 0) { a->screen = SCREEN_SYSTEMS; return; }
	/* MUSE HAS ONE DOOR. Its card used to open its shelf HERE, in the main
	 * loop, as if it were a console: a second Muse beside the one SELECT
	 * opens, so SELECT on it opened that one on top, and the two looked
	 * alike. Eric's call, 2026-09-23: the card opens the same Muse SELECT
	 * does. Whatever lands the main loop on Muse's shelf - A on its card, a
	 * jump to it, a screen put back at boot - hands over to muse_open, which
	 * comes back to the card on the systems row. */
	if (is_muse(&a->sys.systems[a->sys_cursor])) {
		a->screen = SCREEN_SYSTEMS;
		muse_open(a, NULL, NULL);
		return;
	}
	/* The two axes trade roles with the shelf rather than one of them being
	 * reassigned onto the other: stepping always runs along the row, and the
	 * letter jump always runs across it. Turning the shelf turns the d-pad
	 * with it and nothing is left doing two jobs. */
	bool vert = CARD_DIRS[g_dir].vertical;
	in_button back = vert ? IN_UP : IN_LEFT, fwd = vert ? IN_DOWN : IN_RIGHT;
	in_button jup = vert ? IN_LEFT : IN_UP, jdn = vert ? IN_RIGHT : IN_DOWN;
	int dir = 0;

	if (in_repeat(&a->in, back)) { v->cursor = (v->cursor - 1 + n) % n; dir = -1; }
	if (in_repeat(&a->in, fwd))  { v->cursor = (v->cursor + 1) % n; dir = +1; }
	if (in_repeat(&a->in, jdn))  v->cursor = shelf_letter_jump(v, +1);
	if (in_repeat(&a->in, jup))  v->cursor = shelf_letter_jump(v, -1);
	/* L1/R1 jump a screenful, so a long shelf is crossable. Wrapped the long
	 * way round on purpose: C's % truncates toward zero, so on a shelf of
	 * three games the obvious `(cursor - CF_WINDOW + n*2) % n` lands on -1 and
	 * the draw walks off the front of the list. */
	if (in_repeat(&a->in, IN_L1))    v->cursor = ((v->cursor - CF_WINDOW) % n + n) % n;
	if (in_repeat(&a->in, IN_R1))    v->cursor = (v->cursor + CF_WINDOW) % n;
	/* HERE, AND NOT AT THE END OF THE PASS, because four of the handlers below
	 * return without reaching the end.
	 *
	 * A repeat fires from the clock every REPEAT_RATE_MS while a direction is
	 * held, and the loop passes at about 60Hz while any button is down, so
	 * roughly one pass in five moves the cursor. Press A on one of those passes
	 * and the shelf was never told: `target` stays a card behind `cursor`, the
	 * game runs, the render on the way back snaps the cards to that stale
	 * target, and the NEXT pass through here finally notices and animates the
	 * difference. Which is exactly the report - the shelf comes back showing a
	 * game off to the side and then slides across to the right one - and the
	 * one-in-five is why it only happened sometimes.
	 *
	 * X and Y are the same shape: the info screen and a favorite toggle both
	 * return from here too. B is the only one that got away with it, because
	 * enter_system resets this coverflow on the way back in.
	 *
	 * The call is unconditional and `dir` may be 0: a letter jump and L1/R1
	 * move the cursor without a direction, and cf_set_cursor_dir works out the
	 * rest itself. It returns immediately when the cursor has not moved. */
	cf_set_cursor_dir(&v->cf, v->cursor, n, dir);
	if (a->in.pressed[IN_BACK]) {
		a->screen = SCREEN_SYSTEMS;
		evict_far(v, TEX_KEEP_FAR);
		return;
	}
	/* X opens the game. Y marks it. The pair sits together because they are
	 * the two things you do to a card without launching it. */
	if (a->in.pressed[IN_X] && n > 0) { game_info_screen(a); return; }
	/* Y favorites what is under the cursor. Written through immediately:
	 * there is no confirm step to hang the save off, and the alternative is
	 * losing the choice to a flat battery. */
	if (a->in.pressed[IN_Y] && n > 0) {
		fav_toggle(a->sys.systems[shelf_owner(a, a->sys_cursor, v->cursor)].tag,
		           v->list.items[v->cursor].file);
		fav_save();
		/* The shelf follows immediately. Everything below this line may have
		 * moved - the system indices, sys_cursor, and v itself - so nothing
		 * from before it can be reused. */
		refresh_favorites_shelf(a);
		return;
	}
	if (a->in.pressed[IN_ACCEPT]) { launch(a); return; }
}

/* ---------- main ---------------------------------------------------------- */

/* Drop systems with nothing in them.
 *
 * A card is a promise that opening it leads somewhere, and nine cards where
 * three have games is eight swipes to find the one you wanted. systems.cfg
 * stays the full list of what TortOS knows how to run; this is only what is
 * worth showing today.
 *
 * Compacted in place rather than filtered at draw time because sys_cursor
 * indexes sys.systems[] directly in a dozen places - the tint, the card art,
 * the launch, the resume - and an index layer over all of them would be a lot
 * of surface for a cosmetic rule. Both arrays move together or the shelf
 * shows one system's card over another's games.
 *
 * Textures are not freed here: this runs before prime_sys_window, so there
 * are none yet. */
static void hide_empty_systems(app *a)
{
	int i, n = 0;

	for (i = 0; i < a->sys.count; i++) {
		if (a->view[i].list.count <= 0) continue;
		if (n != i) {
			a->sys.systems[n] = a->sys.systems[i];
			a->view[n] = a->view[i];
			memset(&a->view[i], 0, sizeof a->view[i]);
		}
		n++;
	}
	if (n != a->sys.count)
		fprintf(stderr, "scan: %d of %d systems have games\n", n, a->sys.count);
	a->sys.count = n;
	if (a->sys_cursor >= n) a->sys_cursor = n ? n - 1 : 0;
}

/* Build the Favorites shelf: one shelf whose games come from every other.
 *
 * Resolved against the shelves that actually scanned, so a favorite whose ROM
 * is off the card simply does not appear - which is the same rule as hiding
 * an empty system, and better than a card that opens onto a game that is not
 * there. Sorted by name like every other shelf rather than by the order
 * someone pressed Y, so it reads as a shelf and not as a history.
 *
 * Inserted at the front, which shifts every system index up by one, so the
 * owners recorded during resolution are corrected afterwards. Runs before
 * prime_sys_window, so there are no textures to move with them.
 *
 * It is NOT a system: no core, no folder, no extensions. Nothing may read
 * those from a->sys.systems[] for a game on this shelf - shelf_owner() is how
 * every one of them is found instead. */
static void build_favorites_shelf(app *a)
{
	game_entry *items;
	int *owner;
	int n = 0, i, k, si, real;

	if (fav_count() <= 0 || a->sys.count <= 0) return;
	if (a->sys.count >= CFG_MAX_SYSTEMS) {
		fprintf(stderr, "scan: no room for a Favorites shelf\n");
		return;
	}

	items = calloc(FAV_MAX, sizeof *items);
	owner = calloc(FAV_MAX, sizeof *owner);
	if (!items || !owner) { free(items); free(owner); return; }

	real = a->sys.count;
	for (i = 0; i < fav_count(); i++) {
		const char *tag, *file;
		bool found = false;
		if (!fav_at(i, &tag, &file)) continue;
		for (si = 0; si < real; si++) {
			if (strcmp(a->sys.systems[si].tag, tag)) continue;
			for (k = 0; k < a->view[si].list.count; k++) {
				if (strcmp(a->view[si].list.items[k].file, file)) continue;
				items[n] = a->view[si].list.items[k];
				owner[n] = si;
				n++;
				found = true;
				break;
			}
			break;
		}
		/* Said out loud. A favorite that does not resolve is either a ROM
		 * that has left the card or a key that never matched, and silently
		 * showing one fewer game than the file lists is the kind of thing
		 * that gets noticed months later. */
		if (!found) fprintf(stderr, "fav: unresolved %s\t%s\n", tag, file);
		if (n >= FAV_MAX) break;
	}
	if (n == 0) { free(items); free(owner); return; }

	/* Insertion sort on the shown title, carrying the owner with it. n is at
	 * most FAV_MAX and realistically a dozen. */
	for (i = 1; i < n; i++) {
		game_entry t = items[i];
		int to = owner[i], j = i - 1;
		while (j >= 0 && lib_order(&items[j], &t) > 0) {
			items[j + 1] = items[j];
			owner[j + 1] = owner[j];
			j--;
		}
		items[j + 1] = t;
		owner[j + 1] = to;
	}

	/* sys_tex/sys_w/sys_h are parallel to systems[] and have to move with it.
	 * At startup they are all NULL and skipping them is harmless, which is
	 * exactly why it would have gone unnoticed until the shelf was rebuilt
	 * live with the cards already loaded - and then one system would have
	 * been wearing the next one's art. */
	for (i = a->sys.count; i > 0; i--) {
		a->sys.systems[i] = a->sys.systems[i - 1];
		a->view[i] = a->view[i - 1];
		a->sys_tex[i] = a->sys_tex[i - 1];
		a->sys_w[i] = a->sys_w[i - 1];
		a->sys_cb[i] = a->sys_cb[i - 1];
		a->sys_h[i] = a->sys_h[i - 1];
	}
	a->sys_tex[0] = NULL;
	for (i = 0; i < n; i++) owner[i]++;          /* everything moved up one */

	memset(&a->sys.systems[0], 0, sizeof a->sys.systems[0]);
	snprintf(a->sys.systems[0].name, CFG_STR, "%s", "Favorites");
	snprintf(a->sys.systems[0].tag, sizeof a->sys.systems[0].tag, "%s", "FAV");
	snprintf(a->sys.systems[0].card, CFG_STR, "%s", "FAVORITES.png");
	a->sys.systems[0].accent = MENU_ACCENT;      /* TortOS's, not a console's */

	memset(&a->view[0], 0, sizeof a->view[0]);
	a->view[0].list.items = items;
	a->view[0].list.count = n;
	a->view[0].list.scanned = true;
	a->view[0].owner = owner;
	a->view[0].tex = calloc((size_t)n, sizeof *a->view[0].tex);
	a->view[0].tw  = calloc((size_t)n, sizeof *a->view[0].tw);
	a->view[0].th  = calloc((size_t)n, sizeof *a->view[0].th);
	/* cb with the rest. The scan allocates these four together and this path
	 * allocated three, so Favorites had a NULL where every other shelf had an
	 * array - and game_get_tex writes to it on the first card it draws. */
	a->view[0].cb  = calloc((size_t)n, sizeof *a->view[0].cb);
	if (!a->view[0].tex || !a->view[0].tw || !a->view[0].th || !a->view[0].cb) {
		free(a->view[0].tex); free(a->view[0].tw); free(a->view[0].th);
		free(a->view[0].cb);
		free(items); free(owner);
		memset(&a->view[0], 0, sizeof a->view[0]);
		for (i = 0; i < a->sys.count; i++) {
			a->sys.systems[i] = a->sys.systems[i + 1];
			a->view[i] = a->view[i + 1];
			a->sys_tex[i] = a->sys_tex[i + 1];
			a->sys_w[i] = a->sys_w[i + 1];
			a->sys_cb[i] = a->sys_cb[i + 1];
			a->sys_h[i] = a->sys_h[i + 1];
		}
		return;
	}
	a->sys.count++;
	fprintf(stderr, "scan: %-16s %d game%s\n", "Favorites", n, n == 1 ? "" : "s");
}

static bool fav_shelf_present(app *a)
{
	return a->sys.count > 0 && strcmp(a->sys.systems[0].tag, "FAV") == 0;
}

/* Take the Favorites shelf back off. Its game list and owner map are this
 * file's own allocations rather than lib_scan's, and the game_entry values in
 * it are copies - the originals belong to the shelves they came from and must
 * not be touched. */
static void drop_favorites_shelf(app *a)
{
	int i;

	if (!fav_shelf_present(a)) return;

	for (i = 0; i < a->view[0].list.count; i++)
		if (a->view[0].tex[i]) SDL_DestroyTexture(a->view[0].tex[i]);
	free(a->view[0].tex);
	free(a->view[0].tw);
	free(a->view[0].th);
	free(a->view[0].list.items);
	free(a->view[0].owner);
	if (a->sys_tex[0]) SDL_DestroyTexture(a->sys_tex[0]);

	for (i = 0; i + 1 < a->sys.count; i++) {
		a->sys.systems[i] = a->sys.systems[i + 1];
		a->view[i] = a->view[i + 1];
		a->sys_tex[i] = a->sys_tex[i + 1];
		a->sys_w[i] = a->sys_w[i + 1];
		a->sys_cb[i] = a->sys_cb[i + 1];
		a->sys_h[i] = a->sys_h[i + 1];
	}
	a->sys.count--;
	memset(&a->view[a->sys.count], 0, sizeof a->view[0]);
	a->sys_tex[a->sys.count] = NULL;
}

/* Rebuild the Favorites shelf in place, for use the moment a favorite
 * changes. Restarting the launcher to see a star take effect is not an
 * answer.
 *
 * Drop and rebuild rather than patch: the shelf is a sorted projection of a
 * set over every other shelf, and the four cases - it appears, it grows, it
 * shrinks, it goes away - are one line each this way and four separate
 * index-juggling routines the other.
 *
 * Everything is re-found by TAG afterwards, never by index. Inserting or
 * removing the shelf moves every system up or down by one, so the index the
 * caller was standing on means something different by the time this returns.
 * The tag does not move. */
static void refresh_favorites_shelf(app *a)
{
	/* Favorites is rebuilt in place, so its indices now name other games. */
	texload_bump();
	char tag[sizeof a->sys.systems[0].tag];
	int cur, i, order = 0;

	if (a->sys.count <= 0) return;
	snprintf(tag, sizeof tag, "%s", a->sys.systems[a->sys_cursor].tag);
	cur = a->view[a->sys_cursor].cursor;
	for (i = 0; i < a->sys.count; i++)
		if (a->view[i].owner) { order = a->view[i].sort; break; }

	drop_favorites_shelf(a);
	build_favorites_shelf(a);
	/* Built in name order with its setting cleared, so the order chosen for
	 * it is put back: without this, favoriting a game turned the shelf back
	 * to Name, and the menu said Name, until the next boot read the setting
	 * again. */
	for (i = 0; i < a->sys.count; i++)
		if (a->view[i].owner) {
			a->view[i].sort = order;
			sort_shelf(a, i);
			break;
		}

	for (i = 0; i < a->sys.count; i++)
		if (strcmp(a->sys.systems[i].tag, tag) == 0) break;

	if (i < a->sys.count) {
		a->sys_cursor = i;
		/* The Favorites list can shrink under the cursor - un-favoriting the
		 * game you are looking at is the ordinary way to use this. */
		if (cur >= a->view[i].list.count)
			cur = a->view[i].list.count ? a->view[i].list.count - 1 : 0;
		a->view[i].cursor = cur;
		cf_reset(&a->view[i].cf, cur);
	} else {
		/* The shelf being stood on no longer exists, which happens exactly
		 * once: un-favoriting the last favorite while inside Favorites. There
		 * is no list to stay in, so go back out to the shelves. */
		a->sys_cursor = 0;
		a->screen = SCREEN_SYSTEMS;
	}
	cf_reset(&a->cf_sys, a->sys_cursor);
}

/* Muse's shelf: a card per album, and which album each card is. Built in the
 * folder's own order, by artist; the order chosen for it is put on it by
 * sort_all once the setting has been read, the way every shelf's is. */
static void muse_fill_view(sysview *v)
{
	int n = g_muse.nalbums;

	if (n <= 0) return;
	v->list.items = calloc((size_t)n, sizeof *v->list.items);
	v->tex = calloc((size_t)n, sizeof *v->tex);
	v->tw = calloc((size_t)n, sizeof *v->tw);
	v->th = calloc((size_t)n, sizeof *v->th);
	v->cb = calloc((size_t)n, sizeof *v->cb);
	v->album = calloc((size_t)n, sizeof *v->album);
	if (!v->list.items || !v->tex || !v->tw || !v->th || !v->cb || !v->album) {
		free(v->list.items); free(v->tex); free(v->tw); free(v->th); free(v->cb);
		free(v->album);
		memset(v, 0, sizeof *v);
		return;
	}
	v->list.count = n;
	v->list.scanned = true;
	muse_order_view(v);
}

/* Muse's shelf in its order, v->sort: which album each card is, and each
 * card's entry from its album - the artist in `name`, the album in `title`,
 * which the card is called, and its folder in `file`, which is how a change
 * of order finds the album that was selected. Filled afresh rather than
 * sorted in place: an entry is only ever a copy of the library's names. The
 * letter jump goes by whatever the shelf is in order of. */
static void muse_order_view(sysview *v)
{
	int k;

	/* The arrays hold every album; the shelf is the kind it shows. */
	if (!v->album) return;
	v->list.count = ml_shelf_order(&g_muse, (ml_order)v->sort, muse_books_shown(),
	                               v->album);
	for (k = 0; k < v->list.count; k++) {
		game_entry *e = &v->list.items[k];
		int al = v->album[k];

		snprintf(e->name, sizeof e->name, "%s", g_muse.artists[muse_artist_of(al)].name);
		snprintf(e->title, sizeof e->title, "%s", g_muse.albums[al].name);
		muse_album_dir(al, e->file, sizeof e->file);
	}
	v->jump_by_name = v->sort == ML_BY_ARTIST;
}

/* Muse's card, at the end of the shelf, when there is music to play.
 *
 * After Favorites and after the empty systems are hidden, so it is neither
 * compacted away for having no games nor shifted by Favorites being inserted
 * in front: it is appended, and every later insertion moves it along with
 * everything else. Its view holds the albums - see muse_fill_view - so A on
 * the card opens a shelf of their covers, the way a console's opens its
 * games.
 *
 * Hidden when the folder is empty or missing, the same rule an empty system
 * and an empty Favorites shelf follow: a card should lead somewhere. */
static void build_muse_shelf(app *a)
{
	system_cfg *s;
	int i;

	/* Everything indexed by album goes with the albums: a rescan can put a
	 * different album at every index. */
	g_muse_gen++;
	ml_free(&g_muse);
	free(g_cov);
	g_cov = NULL;
	free(g_book_done);
	g_book_done = NULL;
	np_forget();
	snprintf(g_muse_root, sizeof g_muse_root, "%s", P_CARD);
	if (!ml_scan_card(g_muse_root, &g_muse) || g_muse.ntracks == 0) {
		fprintf(stderr, "scan: %-16s no music or books in %s\n", "Muse", g_muse_root);
		return;
	}
	g_cov = calloc((size_t)g_muse.nalbums, sizeof *g_cov);
	g_book_done = calloc((size_t)g_muse.nalbums, sizeof *g_book_done);
	if (!g_book_done) { ml_free(&g_muse); return; }
	for (i = 0; i < g_muse.nalbums; i++) {
		char key[LIB_PATH + 8], val[16];

		if (!g_muse.albums[i].book) continue;
		book_key(i, key, sizeof key);
		db_get_str(db_dev(), key, val, sizeof val, "");
		g_book_done[i] = !strcmp(val, "finished");
	}
	{
		char show[16];

		db_get_str(db_dev(), "muse.show", show, sizeof show, "music");
		g_muse_books = !strcmp(show, "books");
	}
	if (a->sys.count >= CFG_MAX_SYSTEMS) return;
	i = a->sys.count;
	s = &a->sys.systems[i];
	memset(s, 0, sizeof *s);
	snprintf(s->name, CFG_STR, "%s", "Muse");
	snprintf(s->tag, sizeof s->tag, "%s", "MUSE");
	snprintf(s->card, CFG_STR, "%s", "MUSE.png");
	s->accent = MUSE_ACCENT;
	memset(&a->view[i], 0, sizeof a->view[i]);
	muse_fill_view(&a->view[i]);
	a->sys_tex[i] = NULL;
	a->sys_w[i] = a->sys_h[i] = 0;
	a->sys_cb[i] = 0;
	a->sys.count++;
	fprintf(stderr, "scan: %-16s %d artists, %d albums, %d books, %d tracks\n", "Muse",
	        g_muse.nartists, ml_count(&g_muse, false), ml_count(&g_muse, true),
	        g_muse.ntracks);
}

static void scan_all(app *a)
{
	for (int i = 0; i < a->sys.count; i++) {
		sysview *v = &a->view[i];
		lib_scan(P_ROMS, a->sys.systems[i].folder, a->sys.systems[i].exts, &v->list);
		/* Both shelves on fbneo are named by set, Arcade and Neo Geo alike. */
		titles_apply(&v->list, db_lib(), a->sys.systems[i].folder,
		             strcmp(a->sys.systems[i].core, "fbneo") ? NULL : fbneo_dat());
		splore_add(&a->sys.systems[i], &v->list);
		if (v->list.count > 0) {
			v->tex = calloc((size_t)v->list.count, sizeof *v->tex);
			v->tw = calloc((size_t)v->list.count, sizeof *v->tw);
			v->th = calloc((size_t)v->list.count, sizeof *v->th);
			v->cb = calloc((size_t)v->list.count, sizeof *v->cb);
			/* A count with no array behind it would be dereferenced on the
			 * next frame. An empty shelf is the honest answer. */
			if (!v->tex || !v->tw || !v->th || !v->cb) {
				free(v->tex); free(v->tw); free(v->th); free(v->cb);
				v->tex = NULL; v->tw = NULL; v->th = NULL; v->cb = NULL;
				lib_free(&v->list);
			}
		}
		fprintf(stderr, "scan: %-16s %d game%s\n", a->sys.systems[i].folder,
		        v->list.count, v->list.count == 1 ? "" : "s");
		/* What was left out, and what the folder takes, so a game that is
		 * on the card and not on the shelf has its reason in the log. */
		if (v->list.skipped) {
			char names[LIB_SKIPS_SHOWN * (LIB_NAME + 4)] = "";
			int k, shown = v->list.skipped < LIB_SKIPS_SHOWN ? v->list.skipped
			                                                 : LIB_SKIPS_SHOWN;

			for (k = 0; k < shown; k++) {
				size_t at = strlen(names);

				snprintf(names + at, sizeof names - at, "%s\"%s\"", k ? ", " : "",
				         v->list.skipped_eg[k]);
			}
			fprintf(stderr, "scan: %-16s left out %d (it takes %s): %s%s\n",
			        a->sys.systems[i].folder, v->list.skipped,
			        a->sys.systems[i].exts[0] ? a->sys.systems[i].exts : "anything",
			        names, v->list.skipped > shown ? ", and more" : "");
		}
	}
	hide_empty_systems(a);
	build_favorites_shelf(a);
	build_muse_shelf(a);
}

/* Read the card again and rebuild every shelf, for when something outside the
 * launcher has changed what is on it - which today means Over The Hare.
 *
 * Drop and rebuild rather than patch, on the same reasoning drop_favorites
 * gives: the cases are "a game appeared", "a game went", "a system that was
 * empty now has games", "a system emptied", and "the shelf you are standing on
 * is one of those". Patching five cases correctly is harder than doing the
 * whole thing again, and the whole thing is three directory reads.
 *
 * The config is re-read because hide_empty_systems does not hide, it COMPACTS:
 * a system with no games is removed from systems[] and its entry overwritten.
 * Uploading the first ROM for a system is precisely what this feature is for,
 * so without the reload that system could never come back. */
static void rescan_all(app *a)
{
	char tag[sizeof a->sys.systems[0].tag];
	char path[CFG_STR * 2];
	int i;

	/* Indices move when a system appears or empties, so the tag is the only
	 * handle on "where the player was" that survives the rebuild. */
	snprintf(tag, sizeof tag, "%s", a->sys.systems[a->sys_cursor].tag);

	/* First, and on its own: its list and owner map are this file's
	 * allocations rather than lib_scan's, and its textures are its own. */
	drop_favorites_shelf(a);

	free_all_textures(a);
	for (i = 0; i < a->sys.count; i++) {
		free(a->view[i].tex); free(a->view[i].tw); free(a->view[i].th);
		free(a->view[i].cb);
		free(a->view[i].album);                         /* Muse's, else NULL */
		lib_free(&a->view[i].list);
		memset(&a->view[i], 0, sizeof a->view[i]);
	}
	/* Every slot, not the first sys.count of them.
	 *
	 * hide_empty_systems moves systems[] and view[] and leaves sys_tex where
	 * it is - harmless at startup because they are all NULL then, and the
	 * comment in build_favorites_shelf already says so. Running the scan a
	 * second time is the moment that stops being true, and the failure is a
	 * system wearing the next one's card. Clearing the whole array puts it
	 * back to the state the omission is safe in. */
	memset(a->sys_tex, 0, sizeof a->sys_tex);
	memset(a->sys_w, 0, sizeof a->sys_w);
	memset(a->sys_h, 0, sizeof a->sys_h);

	/* menu_shelf_width measures every row of every system's menu once and
	 * caches it. The set of systems is exactly what it measured across, so a
	 * rescan invalidates it - and the case that shows is a card that booted
	 * empty: with no systems there are no game-menu rows to measure, the
	 * panel is sized for the TortOS menu alone, and the first system to
	 * arrive gets "Pico-8 Native" cut off inside a frame measured before it
	 * existed. */
	a->menu_w = 0;

	snprintf(path, sizeof path, "%s/systems.cfg", P_ROOT);
	if (!cfg_load_systems(path, &a->sys)) {
		/* The card was readable a moment ago, so this is a card that has gone
		 * away underneath us. An empty shelf is the honest picture. */
		fprintf(stderr, "rescan: no usable %s\n", path);
		a->sys.count = 0;
		a->sys_cursor = 0;
		a->screen = SCREEN_SYSTEMS;
		return;
	}

	scan_all(a);
	display_load(a);     /* indexes by tag, so it is safe to run again */
	shader_load(a);
	engine_load(a);
	sort_load(a);
	sort_all(a);

	a->sys_cursor = 0;
	for (i = 0; i < a->sys.count; i++)
		if (strcmp(a->sys.systems[i].tag, tag) == 0) { a->sys_cursor = i; break; }

	/* Whether the caller can stay where it was.
	 *
	 * Rescanning from inside a system's game list should leave you in that
	 * list looking at the new games, so the screen is not forced here. But the
	 * shelf being stood on can have emptied and gone - deleting the last ROM
	 * for a system over the network is the ordinary way - and then there is no
	 * list to stay in. Same rule refresh_favorites_shelf follows when the last
	 * favorite goes. */
	if (i >= a->sys.count || !a->sys.count) a->screen = SCREEN_SYSTEMS;

	if (a->sys.count) {
		a->tint = a->sys.systems[a->sys_cursor].accent;
		/* The rebuilt view starts at zero and its coverflow has to agree, or
		 * the shelf animates from wherever the old one happened to be. */
		a->view[a->sys_cursor].cursor = 0;
		cf_reset(&a->view[a->sys_cursor].cf, 0);
	}
	cf_reset(&a->cf_sys, a->sys_cursor);
}

/* --shot <file.png> [--screen games|systems] draws one frame, writes it out
 * and exits. This is how the shelf gets looked at without a device in hand:
 * the host build renders exactly what the handheld renders. */
static const char *shot_path;
static int shot_screen = -1;
static int shot_menu, shot_menu_sel;
static int shot_kb, shot_kb_layer;
static const char *shot_notice;
static const char *shot_notice_head = "Unlocked  -  5 points";
static int shot_cheevos;
static int shot_cheevos_sel;
/* How far between two systems the shelf is caught, 0 at rest. The shot is one
 * settled frame, so anything that only happens DURING a move - the label
 * crossfade, a card mid-slide - cannot be looked at without this. */
static float shot_sysmove;
/* The same for the games shelf, which is the one with a long list under it.
 * Eleven systems make a rail whose segments tile the track; three hundred games
 * make an 18px segment moving two and a half pixels a card, and the two do not
 * look alike at any fraction of a move. */
static float shot_gamemove;
static const char *shot_cheevos_game = "Hagane: The Final Conflict";
static int shot_syn;       /* --synopsis: the card a scraped game opens */
static int shot_info;
/* --controls [page] draws MENU > Controls, page 0-3, over whichever screen
 * --screen asked for: the page follows UI Direction, so this is the only way
 * to look at all three without setting the device three times. */
static int shot_controls = -1;
static int shot_art;
static const char *shot_art_now = "Legend of Zelda, The - A Link to the Past (USA)";
static int shot_hare;
static const char *shot_hare_addr = "192.168.1.42";
static const char *shot_hare_who  = "1 connected";
static const char *shot_hare_head = "receiving Contra (USA).zip";
static const char *shot_wait, *shot_wait_msg = "Scanning...";
static const char *shot_kb_text = "correct horse";
/* The title was hardcoded to "Wi-Fi password", so the one other thing that
 * uses this keyboard - signing in to RetroAchievements - could not be
 * rendered at all. A harness narrower than the thing it checks. */
static const char *shot_kb_title = "Wi-Fi password";
static int shot_slots, shot_slot_sel;
static int shot_jump;               /* letter-jumps to apply before drawing */
/* --nowplaying ALBUM [TRACK [SECONDS]], and --paused with it. */
static int   shot_np = -1, shot_np_track, shot_np_paused;
static float shot_np_at = 83.0f;
/* --mode NAME plays whatever the shot shows in that play mode: the mark beside
 * the track count is drawn from it. */
static const char *shot_mode;
static float shot_slot_aspect = 4.0f / 3.0f;

/* A stand-in for a paused game frame, at whatever shape was asked for: the
 * slot carousel frames the picture to its own aspect, so looking at that
 * without a device means being able to hand it one. */
static SDL_Texture *fake_frame(SDL_Renderer *r, float aspect)
{
	int h = 224, w = (int)(224 * aspect + 0.5f);
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32,
	                                                SDL_PIXELFORMAT_ARGB8888);
	SDL_Texture *t;
	int x, y;

	if (!s) return NULL;
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			Uint32 *px = (Uint32 *)((Uint8 *)s->pixels + (size_t)y * s->pitch);
			int checker = ((x / 16) + (y / 16)) & 1;
			px[x] = SDL_MapRGBA(s->format,
			                    (Uint8)(30 + 180 * x / w),
			                    (Uint8)(40 + 150 * y / h),
			                    (Uint8)(checker ? 150 : 90), 255);
		}
	t = SDL_CreateTextureFromSurface(r, s);
	SDL_FreeSurface(s);
	return t;
}

static void shot_draw_slots(app *a)
{
	slot_view sv = { .aspect = shot_slot_aspect, .saving = 0 };
	int i;

	for (i = 0; i <= GM_SLOTS; i++) {
		/* Slot 5 left empty, so the empty state is in the picture too. */
		if (i == 5) continue;
		sv.have[i] = 1;
		snprintf(sv.when[i], sizeof sv.when[i], "Aug %d %d:%02d:%02d PM",
		         20 + i, 1 + i, i * 7, i * 9);
		sv.thumb[i] = fake_frame(a->r, shot_slot_aspect);
	}
	SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(a->r, 0, 0, 0, 185);
	SDL_RenderFillRect(a->r, NULL);
	slot_draw(a, &sv, shot_slot_sel);
	for (i = 0; i <= GM_SLOTS; i++)
		if (sv.thumb[i]) SDL_DestroyTexture(sv.thumb[i]);
}

/* Now Playing without a daemon, for --shot. A cover not yet on the card is
 * drawn as missing rather than asked for: there is nobody to ask. */
static void np_shot(app *a)
{
	mu_now mn = { 0 };
	const ml_album *al;
	char next[128] = "";
	int ar, t;

	if (shot_np < 0 || shot_np >= g_muse.nalbums) return;
	al = &g_muse.albums[shot_np];
	t = shot_np_track < al->n ? shot_np_track : al->n - 1;
	for (ar = 0; ar < g_muse.nartists - 1; ar++)
		if (shot_np < g_muse.artists[ar].first + g_muse.artists[ar].n) break;
	mn.state = shot_np_paused ? MU_PAUSED : MU_PLAYING;
	snprintf(mn.title, sizeof mn.title, "%s", g_muse.tracks[al->first + t].name);
	snprintf(mn.artist, sizeof mn.artist, "%s", g_muse.artists[ar].name);
	snprintf(mn.album, sizeof mn.album, "%s", al->name);
	mn.at = shot_np_at;
	mn.len = 236.0;
	mn.index = t;
	mn.count = al->n;
	if (t + 1 < al->n)
		snprintf(next, sizeof next, "%s", g_muse.tracks[al->first + t + 1].name);

	{
		char base[LIB_PATH * 2], path[LIB_PATH * 2 + 8];

		ml_cover_base(g_muse_root, &g_muse, shot_np, base, sizeof base);
		snprintf(path, sizeof path, "%s.jpg", base);
		if (!file_nonempty(path)) {
			snprintf(path, sizeof path, "%s.png", base);
			if (!file_nonempty(path) && g_cov)
				g_cov[shot_np].st = ml_folder_image(g_muse_root, &g_muse, shot_np,
				                                    path, sizeof path)
				                  ? COV_FOLDER : COV_NONE;
		}
	}
	np_cover(a, shot_np);
	np_draw(a, &mn, next, plat_hold_switch());
}

/* For a shot: every cover not on the card is a cover the music does not
 * carry, since there is no daemon to ask - so those albums draw their
 * generated card rather than an empty slot. */
static void muse_covers_settle(void)
{
	char base[LIB_PATH * 2], p[LIB_PATH * 2 + 8];
	int i;

	for (i = 0; g_cov && i < g_muse.nalbums; i++) {
		ml_cover_base(g_muse_root, &g_muse, i, base, sizeof base);
		snprintf(p, sizeof p, "%s.jpg", base);
		if (file_nonempty(p)) continue;
		snprintf(p, sizeof p, "%s.png", base);
		if (!file_nonempty(p))
			g_cov[i].st = ml_folder_image(g_muse_root, &g_muse, i, p, sizeof p)
			            ? COV_FOLDER : COV_NONE;
	}
}

static void take_shot(app *a)
{
	/* The panel's pixels, not the layout's units: the two differ wherever
	 * plat_scale() is not 1, and the read-back is of the panel. */
	int ow = TORTOS_SCREEN_W, oh = TORTOS_SCREEN_H;
	SDL_Surface *out;

	SDL_GetRendererOutputSize(a->r, &ow, &oh);
	out = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_RGBA32);
	muse_covers_settle();
	draw_shelf(a);
	if (shot_menu) tortos_menu_draw(a, shot_menu_sel);
	if (shot_slots) shot_draw_slots(a);
	if (shot_kb) kb_preview(a->r, shot_kb_title, shot_kb_text,
	                        shot_kb_layer, 1, 0, MENU_ACCENT);
	/* The in-game notice is not drawn by this process - Diatom composites it
	 * over the game - so it cannot be screenshotted like the rest. Rendered
	 * to its wire format and read straight back, which is the same pixels
	 * Diatom will show and therefore the thing worth looking at. */
	/* The achievements list, with sample data. It is only reachable from the
	 * in-game menu, so without this it can only be judged on a device with a
	 * game running - and it took two rounds of that to notice its heading was
	 * clipped at both ends. */
	/* The one-line panel behind "Scanning...", "Connecting...", "Signing
	 * in..." and "Looking up this game...". Five flows use it and none of them
	 * could be rendered. */
	if (shot_wait) {
		menu_row row = { shot_wait_msg, NULL, false };

		menu_draw(a, shot_wait, &row, 1, -1, 0, MENU_ACCENT);
	}
	if (shot_info) info_preview(a, true);
	if (shot_controls >= 0) {
		menu_row rows[CTL_MAX_ROWS];
		char head[80];
		ctl_page p = (ctl_page)(shot_controls % CTL_PAGES);
		int n = ctl_rows(p, controls_dir(), rows);

		snprintf(head, sizeof head, "Controls: %s", ctl_page_name(p));
		SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(a->r, 0, 0, 0, 120);
		SDL_RenderFillRect(a->r, NULL);
		menu_draw(a, head, rows, n, -1, menu_std_width(a), MENU_ACCENT);
	}
	if (shot_np >= 0) np_shot(a);
	if (shot_art)
		art_preview(a, shot_art_now, "4 of 10 systems",
		            "37 found, 2 missing, 61 already", true);
	if (shot_hare)
		hare_preview(a, shot_hare_addr, "4071", shot_hare_who,
		             "12 KB / 41 MB", shot_hare_head);
	/* The synopsis card, from the card's own games table - so a shot of it is a
	 * picture of real scraped text rather than a fixture. */
	if (shot_syn) {
		sysview *v = &a->view[a->sys_cursor];
		const system_cfg *s = &a->sys.systems[shelf_owner(a, a->sys_cursor, v->cursor)];
		game_meta m;

		if (v->list.count > 0 &&
		    db_game_get(db_lib(), s->folder, v->list.items[v->cursor].file, &m) &&
		    m.synopsis[0]) {
			char lines[SYN_WRAP_LINES][CHV_WRAP_COLS];
			menu_row rows[SYN_ROWS];
			int fixed = menu_std_width(a);
			int loop_at = 0;
			int nl = syn_layout(m.synopsis, fixed, lines, rows, &loop_at);

			SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
			SDL_SetRenderDrawColor(a->r, 0, 0, 0, 120);
			SDL_RenderFillRect(a->r, NULL);
			menu_draw_ex(a, v->list.items[v->cursor].title, rows, nl, -1, fixed,
			             s->accent, NULL, false, loop_at);
		}
	}
	if (shot_cheevos) {
		/* Real titles, and the long ones are the point: a set's names run past
		 * the panel often, and this shot drew eight short ones - so the screen
		 * could be looked at without a device and still not show the thing
		 * worth looking at. Two of the three below are real RetroAchievements
		 * titles from sets on this card. */
		static const struct { const char *t; int p; bool got; } sample[] = {
			{ "The Path to Disaster", 5, true },
			{ "Hold On To Your Potatoes, We're In For A Bumpy Ride", 10, true },
			{ "Violated Heavens", 10, true },
			{ "I Am Not Left Handed Either, But I Can Still Beat You", 25, false },
			{ "Koma Faction's Fall", 25, false },
			{ "Not So Disaster", 10, false },
			{ "Storming The Fortress", 10, false },
			{ "Blazing through the Skies", 10, false },
		};
		int cn = (int)(sizeof sample / sizeof sample[0]), ci;
		menu_row rows[8];
		unsigned vcols[8] = { 0 };
		char vals[8][16], head[192];

		for (ci = 0; ci < cn; ci++) {
			snprintf(vals[ci], sizeof vals[ci], "%d", sample[ci].p);
			rows[ci] = (menu_row){ sample[ci].t, vals[ci], sample[ci].got };
			vcols[ci] = sample[ci].got ? UI_EARNED_RGB : 0u;
		}
		snprintf(head, sizeof head, "%s\n%d/36 cheevos   %d/415 points",
		         shot_cheevos_game, 3, 25);
		/* Drawn the way cheevos_screen draws it - menu_draw_ex, the earned
		 * colors, the shared width - because a shot that composes the panel
		 * differently from the screen is a picture of something that does not
		 * exist. It used to call menu_draw with a width of 0. */
		menu_draw_ex(a, head, rows, cn, shot_cheevos_sel, menu_std_width(a),
		             a->tint, vcols, true, 0);
	}
	if (shot_notice) {
		char dt[512];
		FILE *nf;

		snprintf(dt, sizeof dt, "%s.dtov", shot_path);
		if (notice_render(shot_notice_head, shot_notice, dt) &&
		    (nf = fopen(dt, "rb"))) {
			unsigned char hd[8];
			if (fread(hd, 1, 8, nf) == 8 && !memcmp(hd, "DTOV", 4)) {
				int nw = hd[4] | (hd[5] << 8), nh = hd[6] | (hd[7] << 8);
				SDL_Surface *ns = SDL_CreateRGBSurfaceWithFormat(
					0, nw, nh, 32, SDL_PIXELFORMAT_ARGB8888);
				if (ns && fread(ns->pixels, 4, (size_t)nw * nh, nf)
				          == (size_t)nw * nh) {
					/* The buffer is panel pixels, so it is drawn at 1/s
					 * units: the size Diatom puts it on the glass. */
					float ps = plat_scale();
					SDL_Rect at = { (TORTOS_SCREEN_W - nw) / 2,
					                TORTOS_SCREEN_H - nh - TORTOS_SCREEN_H / 24,
					                nw, nh };
					SDL_FRect atf = { (TORTOS_SCREEN_W - nw / ps) / 2,
					                  TORTOS_SCREEN_H - nh / ps - TORTOS_SCREEN_H / 24,
					                  nw / ps, nh / ps };
					SDL_Texture *nt = SDL_CreateTextureFromSurface(a->r, ns);
					if (nt) {
						SDL_SetTextureBlendMode(nt, SDL_BLENDMODE_BLEND);
						if (ps == 1.0f) SDL_RenderCopy(a->r, nt, NULL, &at);
						else SDL_RenderCopyF(a->r, nt, NULL, &atf);
						SDL_DestroyTexture(nt);
					}
				}
				if (ns) SDL_FreeSurface(ns);
			}
			fclose(nf);
			remove(dt);
		}
	}
	draw_battery(a->r);
	if (out) {
		/* Read BEFORE presenting: the backbuffer is invalid afterwards. */
		SDL_RenderReadPixels(a->r, NULL, SDL_PIXELFORMAT_RGBA32,
		                     out->pixels, out->pitch);
		plat_present(a->r);
		IMG_SavePNG(out, shot_path);
		SDL_FreeSurface(out);
		/* Say what was drawn, not just that something was: a tool whose whole
		 * job is rendering one state is far more useful when the state it
		 * chose is in the output beside the filename. */
		{
			sysview *v = &a->view[a->sys_cursor];
			const char *what = a->screen == SCREEN_GAMES && v->list.count > 0
			                 ? v->list.items[v->cursor].title
			                 : a->sys.systems[a->sys_cursor].name;
			/* Checked rather than assumed. This line once named a focused
			 * game on a shot with no cards in it. A card drawn on the frame
			 * always ends up with a texture - box art, a preview, or the
			 * generated card - so one without means the picture is not what
			 * the name beside it claims, and that is worth saying loudly. */
			const char *note =
				a->screen == SCREEN_GAMES && v->list.count > 0 &&
				v->tex && !v->tex[v->cursor]
				? "  (NO ART: the focused card had no texture on this frame)" : "";
			fprintf(stderr, "wrote %s  [%s] %s%s\n", shot_path,
			        a->screen == SCREEN_GAMES ? "games" : "systems", what, note);
		}
	}
}

int main(int argc, char *argv[])
{
	app a = { 0 };
	char path[CFG_STR * 2];
	char startup[CFG_STR];

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot_path = argv[++i];
		else if (!strcmp(argv[i], "--screen") && i + 1 < argc)
			shot_screen = strcmp(argv[++i], "games") == 0 ? SCREEN_GAMES
			                                              : SCREEN_SYSTEMS;
		/* --menu [row] draws the TortOS menu over whichever screen --screen
		 * asked for, so the panel can be looked at without a device. */
		else if (!strcmp(argv[i], "--menu")) {
			shot_menu = 1;
			if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
				shot_menu_sel = atoi(argv[++i]);
		}
		/* --slots <sel> [aspect] draws one frame of the save/load carousel
		 * over synthetic frames, which is the only way to look at it without
		 * a game running on a device. */
		/* --jump N applies N letter-jumps before the shot (negative for up),
		 * so the d-pad's behavior on a real library can be checked without a
		 * device or a hand on it. */
		else if (!strcmp(argv[i], "--jump") && i + 1 < argc) {
			shot_jump = atoi(argv[++i]);
		}
		/* --keyboard [layer] [text] draws one frame of the text-entry panel,
		 * for the same reason --menu and --slots exist: it is dense, and
		 * laying it out against a screenshot beats a round trip to a device. */
		else if (!strcmp(argv[i], "--cheevos-screen")) {
			shot_cheevos = 1;
			if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
				shot_cheevos_sel = atoi(argv[++i]);
			/* A game name, so the heading can be looked at with one of the
			 * long ones rather than only the short one that fit. */
			if (i + 1 < argc && argv[i + 1][0] != '-') shot_cheevos_game = argv[++i];
		}
		else if (!strcmp(argv[i], "--synopsis")) shot_syn = 1;
		/* --nowplaying ALBUM [TRACK [SECONDS]] draws Now Playing for an album
		 * of the card's Music folder, by its number in the scan, without a
		 * daemon: the cover has to be on the card already, or the album is
		 * drawn as one with none. --paused draws it paused. */
		else if (!strcmp(argv[i], "--nowplaying") && i + 1 < argc) {
			shot_np = atoi(argv[++i]);
			if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
				shot_np_track = atoi(argv[++i]);
			if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
				shot_np_at = (float)atof(argv[++i]);
		}
		else if (!strcmp(argv[i], "--paused")) shot_np_paused = 1;
		else if (!strcmp(argv[i], "--mode") && i + 1 < argc) shot_mode = argv[++i];
		else if (!strcmp(argv[i], "--sysmove") && i + 1 < argc)
			shot_sysmove = (float)atof(argv[++i]);
		else if (!strcmp(argv[i], "--gamemove") && i + 1 < argc)
			shot_gamemove = (float)atof(argv[++i]);
		else if (!strcmp(argv[i], "--info")) shot_info = 1;
		else if (!strcmp(argv[i], "--controls")) {
			shot_controls = 0;
			if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
				shot_controls = atoi(argv[++i]);
		}
		else if (!strcmp(argv[i], "--phase") && i + 1 < argc)
			shot_phase = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--art")) {
			shot_art = 1;
			if (i + 1 < argc && argv[i + 1][0] != '-') shot_art_now = argv[++i];
		}
		else if (!strcmp(argv[i], "--hare")) {
			shot_hare = 1;
			if (i + 1 < argc && argv[i + 1][0] != '-') shot_hare_addr = argv[++i];
			if (i + 1 < argc && argv[i + 1][0] != '-') shot_hare_who  = argv[++i];
			if (i + 1 < argc && argv[i + 1][0] != '-') shot_hare_head = argv[++i];
		}
		else if (!strcmp(argv[i], "--wait") && i + 1 < argc) {
			shot_wait = argv[++i];
			if (i + 1 < argc && argv[i + 1][0] != '-') shot_wait_msg = argv[++i];
		}
		else if (!strcmp(argv[i], "--notice") && i + 1 < argc) {
			shot_notice = argv[++i];
			if (i + 1 < argc && argv[i + 1][0] != '-') shot_notice_head = argv[++i];
		}
		else if (!strcmp(argv[i], "--keyboard")) {
			shot_kb = 1;
			if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '2')
				shot_kb_layer = atoi(argv[++i]);
			if (i + 1 < argc && argv[i + 1][0] != '-') shot_kb_text = argv[++i];
			if (i + 1 < argc && argv[i + 1][0] != '-') shot_kb_title = argv[++i];
		}
		else if (!strcmp(argv[i], "--slots")) {
			shot_slots = 1;
			if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
				shot_slot_sel = atoi(argv[++i]);
			if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
				shot_slot_aspect = (float)atof(argv[++i]);
		}
	}
	/* --wifi prints what the radio can see and exits. The wifi module talks
	 * to firmware that only exists on the device, so it cannot be exercised
	 * from the host at all, and a flag that reports its view is the cheapest
	 * way to tell "the machinery is wrong" from "the network is". */
	if (argc > 1 && !strcmp(argv[1], "--wifi")) {
		wifi_net nets[WIFI_MAX_NETS];
		char ssid[WIFI_SSID_MAX], ip[64];
		int n, i;
		wifi_state st;

		printf("bringing the supplicant up ...\n");
		printf("  wifi_up: %s\n", wifi_up() ? "ok" : "FAILED");
		st = wifi_status(ssid, sizeof ssid, ip, sizeof ip);
		printf("  status: %s  ssid=[%s] ip=[%s]\n",
		       st == WIFI_CONNECTED  ? "connected"  :
		       st == WIFI_CONNECTING ? "connecting" :
		       st == WIFI_IDLE       ? "idle"       : "off", ssid, ip);
		n = wifi_scan(nets, WIFI_MAX_NETS);
		printf("  scan: %d network(s)\n", n);
		for (i = 0; i < n; i++)
			printf("    %-32s %4d dBm  %-9s %s\n", nets[i].ssid,
			       nets[i].signal, nets[i].secured ? "secured" : "open",
			       nets[i].known ? "known" : "");
		return n < 0 ? 1 : 0;
	}

	signal(SIGTERM, on_sigterm);
	signal(SIGINT, on_sigterm);

	/* Nothing on the device can read a database - there is no sqlite3 binary -
	 * so losing the ability to SEE a setting would be a real loss where losing
	 * the ability to edit one is the point. Runs before video: it is a
	 * question asked over adb, not a screen. */
	/* Repair, not configuration. A wrong shipped default is permanent on a
	 * device that already seeded it - seeding fills absent keys only, and it
	 * cannot tell "the player chose this" from "we seeded it by mistake" - so
	 * there has to be a way to correct one in place. There is no sqlite3 on
	 * the device to do it with.
	 *
	 * It refuses a key that does not already exist, in either scope. That is
	 * what keeps it a repair tool: a typo cannot invent a setting that looks
	 * real, is written faithfully, and is read by nothing. */
	if (argc > 2 && !strcmp(argv[1], "--set")) {
		char dev[CFG_STR * 2], lib[CFG_STR * 2], env[CFG_STR * 2];
		char key[CFG_STR];
		const char *eq = strchr(argv[2], '=');
		db *target;

		if (!eq || eq == argv[2]) {
			fprintf(stderr, "usage: tortos.elf --set key=value\n");
			return 2;
		}
		snprintf(key, sizeof key, "%.*s", (int)(eq - argv[2]), argv[2]);
		paths_init();
		db_paths_ready(dev, sizeof dev, lib, sizeof lib, env, sizeof env);
		if (!db_init(dev, lib, env)) {
			fprintf(stderr, "cannot open the settings database\n");
			return 1;
		}
		target = db_has(db_dev(), key) ? db_dev()
		       : db_has(db_lib(), key) ? db_lib() : NULL;
		if (!target) {
			fprintf(stderr, "no setting named '%s' - --dump lists them\n", key);
			db_shutdown();
			return 1;
		}
		if (!db_set_str(target, key, eq + 1)) {
			fprintf(stderr, "could not write '%s'\n", key);
			db_shutdown();
			return 1;
		}
		/* Three of these keys are exported for launch.sh, and a value the
		 * database holds but boot.env does not is a setting that takes effect
		 * everywhere except at boot. Setting wifi=1 and rebooting into a
		 * radio that stays down is how that was found. */
		db_write_boot_env();
		printf("%s = %s\n", key, eq + 1);
		db_shutdown();
		return 0;
	}

	/* --meta <file>: fill the card's games table from scraped rows.
	 *
	 * The scrape runs on a computer for now (BACKLOG item 27), and from there
	 * the card's database cannot simply be written: the launcher holds it open
	 * in WAL mode for as long as the device is on, and swapping the file under
	 * a live SQLite handle is how a card loses its favorites and its play time.
	 * Measured on this device: a 76 KB library.db beside a 161 KB -wal.
	 *
	 * So the rows travel as a file and the writing happens HERE, in a second
	 * process, where SQLite arbitrates between the two the way it is built to.
	 * Runs before video, like --set and --dump: it is a question asked over
	 * adb, not a screen, and nothing here presents.
	 *
	 * A record is a tab-separated header - folder, file, year, publisher,
	 * developer, players, genres, esrb, note, title, and the synopsis length
	 * in bytes - then that many bytes of synopsis and a newline. COUNTED RATHER THAN
	 * ESCAPED, because the synopsis is the one field with newlines in it and 42
	 * of the 78 replies this card's games got have them; an escape is a second
	 * thing to get right at both ends.
	 *
	 * A game that already has a row is left alone unless --overwrite follows
	 * the file: a gamelist.xml import should fill gaps, not replace what a
	 * ScreenScraper hit already wrote (TortOS-1v7.4.1). A title the row lacks
	 * is one of those gaps (db_game_import).
	 *
	 * The title came in after the format did, and a record from either side
	 * of that change is rejected by the other rather than misread: an old
	 * record is one field short, and a new one gives an old reader a length
	 * that is not a number. */
	if (argc > 2 && !strcmp(argv[1], "--meta")) {
		char dev[CFG_STR * 2], lib[CFG_STR * 2], head[1024];
		FILE *f = fopen(argv[2], "rb");
		bool overwrite = argc > 3 && !strcmp(argv[3], "--overwrite");
		int wrote = 0, skipped = 0, bad = 0;

		if (!f) {
			fprintf(stderr, "cannot read %s\n", argv[2]);
			return 1;
		}
		paths_init();
		db_paths_ready(dev, sizeof dev, lib, sizeof lib, NULL, 0);
		if (!db_init(dev, lib, NULL)) {
			fprintf(stderr, "cannot open the library database\n");
			fclose(f);
			return 1;
		}
		while (fgets(head, sizeof head, f)) {
			game_meta m = { 0 };
			char *fld[11], *p = head;
			size_t want, take;
			long len;
			int i;

			for (i = 0; i < 10; i++) {
				char *t = strchr(p, '\t');

				if (!t) break;
				*t = '\0';
				fld[i] = p;
				p = t + 1;
			}
			if (i < 10) { bad++; continue; }
			fld[10] = p;
			/* A length that is not a length means the file is not what it says
			 * and every byte after it is at an unknown offset. Stop, rather
			 * than write whatever the rest happens to parse as.
			 *
			 * Checked digit by digit rather than left to strtol, which reads
			 * "nine" as 0 - a perfectly valid length, and the row lands with
			 * its synopsis quietly gone and the reader none the wiser. */
			for (p = fld[10]; *p >= '0' && *p <= '9'; p++)
				;
			if (p == fld[10] || (*p != '\n' && *p != '\r' && *p != '\0')) {
				bad++;
				break;
			}
			len = strtol(fld[10], NULL, 10);
			if (len < 0 || len > 1 << 20) { bad++; break; }
			want = (size_t)len;
			take = want < sizeof m.synopsis ? want : sizeof m.synopsis - 1;
			if (fread(m.synopsis, 1, take, f) != take) { bad++; break; }
			m.synopsis[take] = '\0';
			for (want -= take; want > 0; want--)
				if (fgetc(f) == EOF) break;
			fgetc(f);                            /* the record's newline */
			snprintf(m.year,      sizeof m.year,      "%s", fld[2]);
			snprintf(m.publisher, sizeof m.publisher, "%s", fld[3]);
			snprintf(m.developer, sizeof m.developer, "%s", fld[4]);
			snprintf(m.players,   sizeof m.players,   "%s", fld[5]);
			snprintf(m.genres,    sizeof m.genres,    "%s", fld[6]);
			snprintf(m.esrb,      sizeof m.esrb,      "%s", fld[7]);
			snprintf(m.note,      sizeof m.note,      "%s", fld[8]);
			snprintf(m.title,     sizeof m.title,     "%s", fld[9]);
			/* Decided only now, with the whole record read, so the next
			 * header is still where the file says it is. */
			switch (db_game_import(db_lib(), fld[0], fld[1], &m, overwrite)) {
			case 1:  wrote++;   break;
			case 0:  skipped++; break;
			default: bad++;     break;
			}
		}
		fclose(f);
		printf("%d written, %d skipped, %d rejected\n", wrote, skipped, bad);
		db_shutdown();
		return bad && !wrote ? 1 : 0;
	}

	/* --scrape [folder] runs the box art scraper with no video at all.
	 *
	 * The same art_begin/art_step the screen drives, stepped from here instead
	 * of from a frame loop, so a long run does not depend on somebody holding
	 * the device - and so that BACKLOG 27's "re-scraping just those four is a
	 * five-minute job" is a command rather than a menu to navigate over adb.
	 *
	 * Headless, so it must NOT touch video: the live launcher is presenting
	 * while this runs, and two presenters wedge the display engine. It also
	 * takes the one async network slot, which is per process, so the launcher
	 * is unaffected either way.
	 *
	 * The shrinker runs inline here rather than on a worker - art_shrink falls
	 * back to doing it on the spot when no thread was started - which is what
	 * a run with nothing to draw wants anyway. */
	if (argc > 1 && !strcmp(argv[1], "--scrape")) {
		char dev[CFG_STR * 2], lib[CFG_STR * 2], cp[CFG_STR * 2], path[CFG_STR * 2];
		const char *only_sys = argc > 2 ? argv[2] : NULL;
		systems_cfg all, one;
		art_progress st;
		char seen[128] = "";
		int r;

		paths_init();
		snprintf(path, sizeof path, "%s/systems.cfg", P_ROOT);
		if (!cfg_load_systems(path, &all)) {
			fprintf(stderr, "no usable %s\n", path);
			return 1;
		}
		db_paths_ready(dev, sizeof dev, lib, sizeof lib, NULL, 0);
		if (!db_init(dev, lib, NULL)) {
			fprintf(stderr, "cannot open the databases\n");
			return 1;
		}
		snprintf(cp, sizeof cp, "%s/cacert.pem", P_ROOT);
		net_set_ca_path(cp);
		ss_creds_load();
		IMG_Init(IMG_INIT_PNG);

		if (only_sys) {
			int i;

			memset(&one, 0, sizeof one);
			for (i = 0; i < all.count; i++)
				if (!strcmp(all.systems[i].folder, only_sys)) {
					one.systems[0] = all.systems[i];
					one.count = 1;
					break;
				}
			if (!one.count) {
				fprintf(stderr, "no shelf called %s\n", only_sys);
				return 1;
			}
		}
		fprintf(stderr, "scrape: %s, ScreenScraper %s\n",
		        only_sys ? only_sys : "every shelf",
		        ss_signed_in() ? "signed in" : "not signed in");
		art_begin(only_sys ? &one : &all, P_ROMS, fbneo_dat(), NULL);
		while ((r = art_step()) == 1) {
			art_status(&st);
			/* One line per thing it turns to, not per step: a step is mostly
			 * a poll of a request already in flight. Keyed on what it says it
			 * is doing rather than on the counters, because the interesting
			 * case - a game asked about and not found - moves no counter. */
			if (strcmp(st.now, seen)) {
				snprintf(seen, sizeof seen, "%s", st.now);
				fprintf(stderr, "scrape: %-44s  %d found, %d missing, %d had one\n",
				        st.now, st.found, st.missing, st.skipped);
			}
			usleep(20000);
		}
		art_status(&st);
		fprintf(stderr, "scrape: done - %d found, %d missing, %d already had one%s%s\n",
		        st.found, st.missing, st.skipped,
		        st.problem[0] ? " - " : "", st.problem);
		return 0;
	}

	if (argc > 1 && !strcmp(argv[1], "--dump")) {
		char dev[CFG_STR * 2], lib[CFG_STR * 2];
		paths_init();
		db_paths_ready(dev, sizeof dev, lib, sizeof lib, NULL, 0);
		if (!db_init(dev, lib, NULL)) {
			fprintf(stderr, "cannot open the settings database\n");
			return 1;
		}
		printf("device  %s\n", dev);
		db_dump(db_dev(), stdout);
		printf("library %s\n", lib);
		db_dump(db_lib(), stdout);
		db_shutdown();
		return 0;
	}

	t_boot0 = plat_now_ms();
	paths_init();
	build_child_env();

	/* Before anything reads a setting. Two databases, because the files this
	 * replaced kept per-device and per-card data apart on purpose - see db.h.
	 * A failure here is fatal rather than silently defaulted: every setting
	 * the launcher has would be a fallback, and the player would find their
	 * volume, brightness and account quietly reset with nothing said. */
	{
		char dev[CFG_STR * 2], lib[CFG_STR * 2], env[CFG_STR * 2];
		db_paths_ready(dev, sizeof dev, lib, sizeof lib, env, sizeof env);
		/* NOT fatal, and the reason is what "fatal" costs here. launch.sh
		 * restarts the launcher when it exits, and five exits inside five
		 * seconds each makes it give up and call poweroff - so returning 1
		 * from this point turns a settings problem into a device that will
		 * not boot. It is also BEFORE plat_resident_ready() below, which is
		 * what stops a running game so that only one process is presenting;
		 * exiting here skips that entirely.
		 *
		 * Every getter takes what to say instead, and db_dev()/db_lib()
		 * return NULL harmlessly, so the launcher comes up on the shipped
		 * defaults with no favorites, no turbo and no core options. Worse
		 * than working, far better than off - and it says so where anyone
		 * looking at the log will find it. */
		if (!db_init(dev, lib, env)) {
			fprintf(stderr, "settings: cannot open the database at %s\n", dev);
			if (!db_available())
				fprintf(stderr, "settings: libsqlite3 did not load\n");
			fprintf(stderr, "settings: continuing on the shipped defaults - "
			                "favorites, turbo and core options will be absent, "
			                "and nothing set here will be remembered\n");
		}
		/* Rewritten at every boot, not only on change: it is derived, so a
		 * card that lost it or never had one gets a correct one for free. */
		db_write_boot_env();
	}

	snprintf(path, sizeof path, "%s/systems.cfg", P_ROOT);
	if (!cfg_load_systems(path, &a.sys)) {
		fprintf(stderr, "no usable %s\n", path);
		return 1;
	}

	/* Before the scan, because the scan builds the Favorites shelf out of
	 * them and a shelf cannot be built from a list that has not been read. */
	fav_load();

	/* Close any session the device never saw end - a power cut, a flat
	 * battery - at its last checkpoint, before anything reads a total. See
	 * stats.h: short by up to the checkpoint interval beats lost entirely. */
	stats_recover();

	{	char cp[CFG_STR * 2];
		chv_earned_load();
		plat_resident_on_unlock(on_cheevo_unlocked);
		plat_resident_on_shot(on_shot);
		plat_resident_on_tick(on_game_tick);

		/* Without this every HTTPS request fails verification, because the
		 * device has no trust store of its own - res/ssl/README.md. */
		a.auto_off = auto_off_load();
		a.auto_poweroff = auto_poweroff_load();
		g_aout_policy = aout_load();

		snprintf(cp, sizeof cp, "%s/cacert.pem", P_ROOT);
		net_set_ca_path(cp);
		ra_creds_load();
		ss_creds_load();
		{
			char bin[CFG_STR * 2], music[CFG_STR * 2];

			snprintf(bin, sizeof bin, "%s/muse", P_ROOT);
			snprintf(music, sizeof music, "%s", P_CARD);
			/* A shot starts no daemon: it draws one frame and exits, and a
			 * cover it would ask for is drawn as missing instead - see
			 * muse_covers_settle. */
			musec_init(shot_path ? "" : bin, music);
			musec_on_before_heard(aout_before_muse);
		}
		/* The play mode the player left it in. */
		{
			char m[32];
			int k;

			db_get_str(db_dev(), "muse.mode", m, sizeof m, "in order");
			if (shot_mode) snprintf(m, sizeof m, "%s", shot_mode);
			for (k = 0; k < MUQ_MODES; k++)
				if (!strcmp(m, MUSE_MODES[k].key)) musec_set_mode((muq_mode)k);
		}
	}

	/* Scan every system now, not when one is opened: it is three directory
	 * reads, it happens behind the boot animation, and it means walking into
	 * a system is a frame rather than a wait. */
	scan_all(&a);
	/* After the scan, because it indexes by system, and before anything can
	 * launch, because the mode has to reach Diatom with the first RUN. */
	display_load(&a);
	shader_load(&a);
	engine_load(&a);
	sort_load(&a);
	sort_all(&a);
	t_mark("scan");

	/* BEFORE the first frame this process ever draws: if a previous launcher
	 * died while a game was running, the resident is still presenting through
	 * fbdev right now, and drawing the shelf over it is the two-presenter
	 * case that wedges the display engine in-kernel (Diatom's handoff spike;
	 * it cost a power cycle to prove, twice). plat_resident_ready() connects,
	 * and on READY state=running it stops the game and drains to EXIT - so by
	 * the time video comes up, only one presenter exists. The check at launch
	 * time was too late by definition: this process draws long before the
	 * player launches anything. */
	plat_resident_ready();

	if (!plat_video_init()) { fprintf(stderr, "video init failed\n"); return 1; }
	IMG_Init(IMG_INIT_PNG);
	/* After IMG_Init: the worker calls IMG_Load.
	 *
	 * Not for --shot. A shot draws the shelf exactly once, and with the workers
	 * running every card on that one frame is only queued - so the PNG came out
	 * with the title, the count and the rail and not a single card, while the
	 * log line named the focused game as though it had been drawn. With no
	 * worker, game_get_tex decodes on the frame the way it always did, which is
	 * what a tool for checking one frame wants: the right picture, not a fast
	 * one. */
	if (!shot_path) texload_start();
	a.r = plat_renderer();
	plat_input_init();
	ctl_bright_keys = plat_bright_keys();
	/* Nothing is handed in any more. The two-tier lookup this replaces - a
	 * shipped default and the player's saved level - is one key each in the
	 * database, seeded once and overwritten by a nudge. That is also the end
	 * of a bug it kept reintroducing: reapplying the config afterwards put
	 * the shipped default ahead of the level the player last chose. */
	plat_settings_init();
	plat_leds_off();
	t_mark("video+input");

	/* Before ui_init, which is where the sizes are decided; it persists across
	 * the ui_quit/ui_init pair the standalone-emulator fallback goes through. */
	/* The size the player chose beats the shipped default, the same way a
	 * saved brightness does. Read before ui_init, which is when the scale is
	 * applied. */
	if (!ui_init(a.r, P_FONT)) fprintf(stderr, "font init failed\n");
	{
		char set[CFG_STR];
		db_get_str(db_dev(), "cards", set, sizeof set, CARDS_DEFAULT);
		g_cards = cards_index(set);
		db_get_str(db_dev(), "cards_dir", set, sizeof set, CARDS_DIR_DEFAULT);
		g_dir = cards_dir_index(set);
	}
	t_mark("font+settings");

	a.sys_cursor = 0;
	db_get_str(db_lib(), "startup_system", startup, sizeof startup, "");
	if (startup[0])
		for (int i = 0; i < a.sys.count; i++)
			if (strcasecmp(a.sys.systems[i].name, startup) == 0) {
				a.sys_cursor = i;
				break;
			}
	restore_place(&a);
	/* After restore_place, so it wins: `.last` says where the shelf was and
	 * this says what was actually being played. Before the card priming
	 * below, because that loads textures for whatever the cursor is on, and
	 * a fallback to the shelf should find the right ones there. */
	a.resume_menu = playing_restore(&a);
	cf_reset(&a.cf_sys, a.sys_cursor);
	a.tint = a.sys.systems[a.sys_cursor].accent;
	prime_sys_window(&a);
	prime_window(&a, a.sys_cursor);
	t_mark("card assets");

	a.running = true;
	last_tint_ms = plat_now_ms();

	if (shot_path) {
		if (shot_screen >= 0) a.screen = (screen_id)shot_screen;
		if (shot_jump) {
			sysview *v = &a.view[a.sys_cursor];
			int k, dir = shot_jump > 0 ? 1 : -1;
			for (k = 0; k < (shot_jump < 0 ? -shot_jump : shot_jump); k++)
				if (v->list.count > 0) v->cursor = shelf_letter_jump(v, dir);
		}
		/* A shelf nobody has moved sits at position 0, wherever its cursor
		 * was restored to. On the device that never reaches the screen:
		 * update_games runs cf_set_cursor_dir every pass, which snaps an
		 * unmoved shelf to its cursor. A shot draws one frame and never runs
		 * that pass, so without this it drew the list's first card under the
		 * selected game's title. */
		cf_reset(&a.view[a.sys_cursor].cf, a.view[a.sys_cursor].cursor);
		/* CAUGHT MID-MOVE, for whatever only exists while one is happening.
		 *
		 * The pacing is applied first and the staging reads it, rather than the
		 * duration and the curve being named again here: they differ by
		 * direction mode, and a harness holding its own copy of them draws a
		 * frame the launcher never would. Cost of finding that out: --sysmove
		 * hardcoded Vertical's 360ms, so on a horizontal shelf it backdated the
		 * clock against a duration the draw did not use. */
		cf_clock_freeze();
		if (shot_sysmove != 0.0f) {
			shelf_pacing(&a.cf_sys);
			cf_stage(&a.cf_sys, (float)a.sys_cursor,
			         (float)a.sys_cursor + 1.0f, shot_sysmove);
		}
		/* The games shelf, with the cursor ALREADY at the destination - which
		 * is the state the device is in for the whole of a move, and the reason
		 * anything drawn from the cursor arrives early. */
		if (shot_gamemove != 0.0f) {
			sysview *v = &a.view[a.sys_cursor];

			if (v->list.count > 1) {
				int was = v->cursor;

				v->cursor = (v->cursor + 1) % v->list.count;
				shelf_pacing(&v->cf);
				cf_stage(&v->cf, (float)was, (float)was + 1.0f, shot_gamemove);
			}
		}
		if (a.screen == SCREEN_GAMES) prime_window(&a, a.sys_cursor);
		take_shot(&a);
		goto done;
	}

	/* Dev instrumentation, same standing as --shot: launch one game with no
	 * buttons pressed, so the resident path can be exercised over adb with
	 * nobody holding the device. TORTOS_AUTOLAUNCH="TAG<tab>rom-filename";
	 * pair with TORTOS_AUTOSTOP_S to end the game on a clock. */
	{
		const char *auto_spec = getenv("TORTOS_AUTOLAUNCH");
		if (auto_spec && strchr(auto_spec, '\t')) {
			char tag[64], file[LIB_PATH];
			const char *bar = strchr(auto_spec, '\t');
			int si, gi, found = 0;
			snprintf(tag, sizeof tag, "%.*s", (int)(bar - auto_spec), auto_spec);
			snprintf(file, sizeof file, "%s", bar + 1);
			for (si = 0; si < a.sys.count && !found; si++) {
				if (strcmp(a.sys.systems[si].tag, tag) != 0) continue;
				for (gi = 0; gi < a.view[si].list.count; gi++) {
					const char *b = strrchr(a.view[si].list.items[gi].file, '/');
					b = b ? b + 1 : a.view[si].list.items[gi].file;
					if (strcmp(b, file) == 0) {
						a.sys_cursor = si;
						a.view[si].cursor = gi;
						found = 1;
						break;
					}
				}
			}
			if (found) {
				fprintf(stderr, "autolaunch: %s / %s\n", tag, file);
				launch(&a);
			} else {
				fprintf(stderr, "autolaunch: no %s / %s in the library\n", tag, file);
			}
			goto done;
		}
	}

	/* Everything above ran while the boot animation was on screen. Only now
	 * is it this process's turn to own the framebuffer. */
	wait_for_boot_anim();

	/* Swallow the input noise a boot produces -- replayed wake presses, the
	 * bursts input devices emit as they come up -- before honoring anything. */
	{
		Uint32 grace = SDL_GetTicks() + 350;
		while (SDL_GetTicks() < grace) { plat_input_poll(&a.in); SDL_Delay(8); }
		memset(&a.in, 0, sizeof a.in);
	}

	/* Straight back into the game, before the shelf is ever drawn. After the
	 * input grace above, so a wake press replayed by the boot does not land in
	 * the menu that is about to open. */
	if (a.resume_menu && a.running) {
		a.screen = SCREEN_GAMES;
		launch(&a);
	}

	Uint32 last_pass = 0, last_render = 0;

	while (a.running) {
		plat_input_poll(&a.in);
		if (a.in.quit_requested || want_quit) break;

		/* Followed here as well as during a game. Diatom is resident and takes
		 * SETAUDIO while idle, so a cable plugged in at the shelf moves the
		 * sound before the next launch rather than at it. */
		aout_apply(false);

		/* An account answer that outlived its game, merged when it lands, and
		 * then whatever was waiting on it sent. */
		sync_poll();

		/* Unlocks sent in the background: apply what the account answered. */
		ra_flush_poll();

		/* What Muse said since the last pass - including END, which is how
		 * the next track of an album starts while nobody is on its screen -
		 * and the covers it was asked for by Muse's shelf, which that shelf
		 * has to be drawn again to ask the worker for. */
		muse_poll();
		if (cover_answers()) redraw_now();

		/* Auto Off is the same line as the power button, on every screen
		 * that draws. Diatom watches it during a game, because it owns the
		 * pad then; everywhere else the launcher can see input itself.
		 *
		 * The charger HOLDS the clock, inside idle_due. It used to gate only
		 * the shot, which let the countdown run to zero while plugged in and
		 * sit there expired - so unplugging powered the device off in the
		 * same instant, however long it had been on the cable. Found on the
		 * device 2026-08-30 at a 30s timeout. Counting the charger as
		 * activity is also what the setting says: unplugging starts a whole
		 * fresh countdown, because until then the timeout was not running. */
		{
			pwr_action pa = power_check(&a);
			if (pa == PWR_POWEROFF) { power_off(&a); break; }
		}

		if (in_repeat(&a.in, IN_VOLUP))    plat_volume_nudge(+1);
		if (in_repeat(&a.in, IN_VOLDN))    plat_volume_nudge(-1);
		if (in_repeat(&a.in, IN_BRIGHTUP)) plat_brightness_nudge(+1);
		if (in_repeat(&a.in, IN_BRIGHTDN)) plat_brightness_nudge(-1);

		/* Back on the shelf, so any menu MENU closed is closed: put the flag
		 * down before anything here can open another. See menu_leaving. */
		g_menu_closing = false;

		/* SELECT is Muse, on the shelf and on every screen but a running
		 * game - see muse_open. Before the shelf's own keys, which do not use
		 * it, so every layout gets it from this one line. */
		if (a.in.pressed[IN_SELECT]) { muse_open(&a, NULL, NULL); continue; }

		/* MENU on the shelf is TortOS's own menu, the counterpart to the one
		 * MENU opens in a game. It draws over the shelf and returns here. */
		if (a.in.pressed[IN_MENU]) { tortos_menu(&a); continue; }
		if (a.screen == SCREEN_SYSTEMS) update_systems(&a);
		else update_games(&a);
		if (!a.running) break;

		if (shelf_draw_due(&a, &last_pass, last_render)) {
			tick_tint(&a);
			render(&a);
			last_render = plat_now_ms();
		} else {
			SDL_Delay(IDLE_POLL_MS);
		}
	}

done:
	free_all_textures(&a);
	/* First, and before IMG_Quit: the worker is inside IMG_Load, and the
	 * views it decodes into are freed just below. The resizer too, for the
	 * same IMG_Quit - it is in IMG_Load and IMG_SavePNG. */
	texload_stop();
	art_shrink_stop();
	/* Told, not waited for - see ra_flush_stop. */
	ra_flush_stop();
	for (int i = 0; i < a.sys.count; i++) {
		free(a.view[i].tex); free(a.view[i].tw); free(a.view[i].th);
		lib_free(&a.view[i].list);
	}
	ui_quit();
	IMG_Quit();
	plat_input_quit();
	plat_video_quit();
	SDL_Quit();

	/* Close the databases, which is what checkpoints their WAL and removes
	 * it. Without this the launcher never called sqlite3_close on a normal
	 * exit, so a checkpoint only ever happened when SQLite's own 1000-page
	 * autocheckpoint fired mid-write. tortos.db had crossed that and held 20
	 * keys; library.db had not, and its .db file was one empty page with the
	 * schema and every row living in library.db-wal. Measured 2026-09-07.
	 *
	 * That is not a growth problem - the WAL sits at its high-water mark
	 * either way. It is a portability one: a .db that means nothing without
	 * its sidecar is lost by any backup, card swap or cleanup that treats a
	 * -wal as scratch, and it reseeds silently rather than failing. Closing
	 * here leaves a complete file behind on every clean exit.
	 *
	 * A battery pull still leaves a WAL, which is what WAL recovery is for. */
	db_shutdown();
	return 0;
}
