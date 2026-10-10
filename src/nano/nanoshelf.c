/* SPDX-License-Identifier: MIT */
/* nanoshelf: plorpOS's frontend on the RG Nano (plorpos-ggv).
 *
 * The Nano has 56 MB, one Cortex-A7 and no GPU, so this is not tortos.elf: it
 * is SDL 1.2 on the device's own libraries, text on a 240x240 panel, and
 * PicoArch (a fork, ~/projects/git/picoarch) for the games. Music comes first:
 * Muse plays on the shelf and through games.
 *
 * WHO TALKS TO MUSE. Muse takes one client at a time (a new connection
 * replaces the old), and the play queue lives in the client (src/musec.c). So
 * nanoshelf is the client, always, and stays running while a game does - it
 * lets go of the display, then ticks Muse at 10 Hz until the game exits, the
 * way tortos.elf does on the other devices. The game learns what Muse is doing
 * from a file this writes, and asks for play/pause/next/prev through a FIFO
 * (see PicoArch's plorpos.h for both):
 *
 *   /tmp/plorpos/now   <playing|paused|stopped>\t<title>\t<artist>
 *   /tmp/plorpos/cmd   toggle | next | prev, a line each
 *
 * "playing" is said from the moment a PLAY is sent (musec_heard), so the game
 * closes its sound device inside the second Muse waits for it (MUSE_HANDOVER).
 *
 * Draws only when something changed - input, or what is playing on a screen
 * that shows it. */
#include <SDL/SDL.h>
#include <SDL/SDL_ttf.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../audioout.h"
#include "../musec.h"
#include "../muselib.h"
#include "../platproc.h"

#ifndef TORTOS_VERSION
#define TORTOS_VERSION "dev"
#endif

/* The same shelf on the TrimUI Model S (plorpos-80b), built with -DTRIMUI:
 * a 320x240 panel, the card at /mnt/SDCARD, no FunKey around it. */
#ifdef TRIMUI
#define W     320
#define DEVICE "plorpOS trimui"
#else
#define W     240
#define DEVICE "plorpOS nano"
#endif
#define H     240
/* One text size, the rows from it; the bars stay as they were (at 15 that
 * fills the list exactly: 10 rows of 20). */
#define FONT_SZ  15
#define FONT_BIG (FONT_SZ + 4)
#define TOP   22
#define BOT   18
#define ROW   (FONT_SZ + 5)
#define ROWS  ((H - TOP - BOT) / ROW)

#ifdef TRIMUI
#define CARD     "/mnt/SDCARD"
#else
#define CARD     "/mnt"
#endif
#define DATA     CARD "/plorpOS"      /* what plorpOS keeps on the card */
#define RECENT   DATA "/recent.txt"
#define FAVS     DATA "/favorites.txt"
#define QUEUE    DATA "/queue.txt"
#define STATE    "/tmp/plorpos"
#define NOW      STATE "/now"
#define CMD      STATE "/cmd"
#ifdef TRIMUI
#define GAME_HOME DATA              /* PicoArch's .picoarch */
#else
#define GAME_HOME "/mnt/FunKey"     /* where FunKey keeps .picoarch */
#endif
#define SAVES    CARD "/Saves"        /* Saves/<console folder>/, as on the other devices */
#define BIOS     CARD "/Bios"         /* BIOS files, loose, as on the other devices */
#define ROMS     CARD "/Roms"         /* Roms/<console folder>/, as on the other devices */

/* Where the programs are: the folder above this one's bin/. Installed, that
 * is /usr/local/plorpos on the system partition (install-root.sh), so that
 * nothing runs from the card while it is lent to a computer as a USB drive;
 * tried out without installing, it is the card's /mnt/plorpOS. */
static char picoarch_bin[PATH_MAX], muse_bin[PATH_MAX], font_path[PATH_MAX], pico8rt[PATH_MAX], maps_dir[PATH_MAX];
static char cores_dir[PATH_MAX];

static void app_paths(void)
{
	char exe[PATH_MAX], *slash;
	ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
	int i;

	if (n <= 0) n = (ssize_t)strlen(strcpy(exe, DATA "/bin/nanoshelf"));
	exe[n] = '\0';
	for (i = 0; i < 2 && (slash = strrchr(exe, '/')); i++) *slash = '\0';
	snprintf(picoarch_bin, sizeof picoarch_bin, "%s/bin/picoarch", exe);
	snprintf(muse_bin, sizeof muse_bin, "%s/bin/muse", exe);
	snprintf(font_path, sizeof font_path, "%s/res/menu.ttf", exe);
	snprintf(maps_dir, sizeof maps_dir, "%s/res/maps", exe);
	snprintf(pico8rt, sizeof pico8rt, "%s/pico8rt", exe);
	snprintf(cores_dir, sizeof cores_dir, "%s/cores", exe);
}

/* ---------------------------------------------------------------- systems */

/* The consoles, from FunKey-OS's own launchers (/usr/games/launchers/<name>_launch.sh,
 * collections/<dir>/settings.conf): the folder in Roms/ (named as on the other
 * devices), the PicoArch core, and the extensions RetroFE listed. A core in /mnt/Libretro/cores wins over
 * the stock one, as there. Shown only when its folder has a game in it. */
typedef struct { const char *dir, *label, *core, *ext; } sys_t;
static const sys_t SYS[] = {
	{ "Game Boy",              "Game Boy",          "gambatte",            "gb,dmg,zip" },
	{ "Game Boy Color",        "Game Boy Color",    "gambatte",            "gbc,zip" },
	{ "Game Boy Advance",      "Game Boy Advance",  "gpsp",                "gba,bin,agb,gbz,u1,zip" },
	{ "NES",                   "NES",               "fceumm",              "fds,nes,unf,unif,zip" },
#ifdef TRIMUI
	{ "SNES",                  "Super NES",         "snes9x2002",          "smc,fig,sfc,gd3,gd7,dx2,bsx,swc,zip" },
#else
	{ "SNES",                  "Super NES",         "snes9x2005",          "smc,fig,sfc,gd3,gd7,dx2,bsx,swc,zip" },
#endif
	{ "Genesis",               "Genesis",           "picodrive",           "bin,gen,smd,md,32x,cue,iso,chd,cso,m3u,68k,sgd,pco,zip" },
	{ "Master System",         "Master System",     "picodrive",           "bin,sms,gg,sg,sc,zip" },
	{ "Game Gear",             "Game Gear",         "picodrive",           "gg,zip" },
	{ "TurboGrafx-16",         "PC Engine",         "mednafen_supergrafx", "pce,sgx,cue,ccd,chd,toc,m3u,zip" },
	{ "Neo Geo Pocket",        "Neo Geo Pocket",    "mednafen_ngp",        "ngp,ngc,ngpc,npc,zip" },
	{ "WonderSwan",            "WonderSwan",        "mednafen_wswan",      "ws,wsc,pc2,zip" },
	{ "Atari Lynx",            "Atari Lynx",        "mednafen_lynx",       "lnx,lyx,o,zip" },
	{ "Pokemon Mini",          "Pokemon Mini",      "pokemini",            "min,zip" },
	{ "Pico-8",                "PICO-8",            "fake08",              "p8,png,zip" },
	{ "PlayStation",           "PlayStation",       "pcsx_rearmed",        "bin,cue,img,mdf,pbp,toc,cbn,m3u,chd,iso" },
	{ "Arcade (MAME 2000)",    "Arcade (MAME)",     "mame2000",            "zip" },
	{ "Arcade (FBA 2012)",     "Arcade (FBA)",      "fbalpha2012",         "zip" },
};
#define NSYS ((int)(sizeof SYS / sizeof SYS[0]))

static bool sys_has_ext(const sys_t *s, const char *name)
{
	const char *dot = strrchr(name, '.'), *p = s->ext;
	size_t n;

	if (!dot || name[0] == '.') return false;
	dot++;
	n = strlen(dot);
	while (*p) {
		size_t k = strcspn(p, ",");

		if (k == n && !strncasecmp(p, dot, n)) return true;
		p += k + (p[k] == ',');
	}
	return false;
}

/* The user's own core on the card first, then the one plorpOS updated
 * (plorpos-ggv.39), then FunKey-OS's: a core plorpOS does not ship stays
 * stock. */
static void core_path(const sys_t *s, char *out, size_t n)
{
#ifdef TRIMUI
	/* No stock cores: the ones plorpOS carries, in its cores/. */
	const char *const dirs[] = { CARD "/Libretro/cores", cores_dir };
#else
	static const char *const dirs[] = {
		"/mnt/Libretro/cores", "/usr/local/plorpos/cores", "/usr/games",
	};
#endif
	unsigned i;

	for (i = 0; i < sizeof dirs / sizeof *dirs; i++) {
		snprintf(out, n, "%s/%s_libretro.so", dirs[i], s->core);
		if (access(out, R_OK) == 0)
			return;
	}
}

/* Whether a console's folder has a game: the first match is enough. */
static bool sys_has_games(const sys_t *s)
{
	char dir[256];
	DIR *d;
	struct dirent *e;
	bool any = false;

#ifdef TRIMUI
	/* Not every console has a core here yet: one without is not shown. */
	core_path(s, dir, sizeof dir);
	if (access(dir, R_OK) != 0) return false;
#endif
	snprintf(dir, sizeof dir, ROMS "/%s", s->dir);
	if (!(d = opendir(dir))) return false;
	while (!any && (e = readdir(d)))
		any = sys_has_ext(s, e->d_name);
	closedir(d);
	return any;
}

/* ----------------------------------------------------------- game names */

/* Arcade games are named by set ("mslug.zip"): res/maps/<core>.txt gives
 * the title, `set.zip<TAB>Title` a line, from the core's own romset (ggv.46).
 * A console's map is read the first time one of its games is named, then
 * kept, sorted by file for bsearch. No map, or a file not in it: the file.
 * A title starting with "." hides the set (BIOS sets). */
typedef struct { const char *file, *title; } map_ent;
static struct { bool read; char *buf; map_ent *e; int n; } maps[NSYS];

static int cmp_map(const void *a, const void *b)
{
	return strcasecmp(((const map_ent *)a)->file, ((const map_ent *)b)->file);
}

static void map_read(int s)
{
	char path[PATH_MAX], *p, *nl, *tab;
	FILE *f;
	long len;
	int cap = 0;

	maps[s].read = true;
	snprintf(path, sizeof path, "%s/%s.txt", maps_dir, SYS[s].core);
	if (!(f = fopen(path, "r"))) return;
	if (fseek(f, 0, SEEK_END) || (len = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) ||
	    !(maps[s].buf = malloc((size_t)len + 1)) || fread(maps[s].buf, 1, (size_t)len, f) != (size_t)len) {
		fclose(f);
		return;
	}
	fclose(f);
	maps[s].buf[len] = '\0';
	for (p = maps[s].buf; p && *p; p = nl) {
		size_t k;

		if ((nl = strchr(p, '\n'))) *nl++ = '\0';
		if ((k = strlen(p)) && p[k - 1] == '\r') p[k - 1] = '\0';
		if (!(tab = strchr(p, '\t')) || !tab[1]) continue;
		*tab = '\0';
		if (maps[s].n == cap) {
			map_ent *e = realloc(maps[s].e, (cap = cap ? cap * 2 : 1024) * sizeof *e);

			if (!e) break;
			maps[s].e = e;
		}
		maps[s].e[maps[s].n++] = (map_ent){ p, tab + 1 };
	}
	qsort(maps[s].e, maps[s].n, sizeof *maps[s].e, cmp_map);
	fprintf(stderr, "nanoshelf: %d titles for %s\n", maps[s].n, SYS[s].dir);
}

/* "Sonic 3D Blast.md" -> "Sonic 3D Blast"; "mslug.zip" -> "Metal Slug: Super Vehicle-001" */
static void game_name(int s, const char *file, char *out, size_t n)
{
	map_ent key = { file, NULL }, *m;
	const char *dot = strrchr(file, '.');
	size_t k = dot && dot != file ? (size_t)(dot - file) : strlen(file);

	if (!maps[s].read) map_read(s);
	if (maps[s].n && (m = bsearch(&key, maps[s].e, maps[s].n, sizeof key, cmp_map))) {
		snprintf(out, n, "%s", m->title);
		return;
	}
	if (k >= n) k = n - 1;
	memcpy(out, file, k);
	out[k] = '\0';
}

/* ------------------------------------------------------------------ games */

static char **games;            /* file names in the open console's folder */
static int    ngames, games_sys = -1;

/* A list sorts by the name shown, looked up once a game, then by file
 * (clones share a title). */
typedef struct { char *file; char name[200]; } game_sort;

static int cmp_name(const void *a, const void *b)
{
	const game_sort *x = a, *y = b;
	int c = strcasecmp(x->name, y->name);

	return c ? c : strcasecmp(x->file, y->file);
}

static void games_load(int s)
{
	char dir[256];
	DIR *d;
	struct dirent *e;
	int cap = 0;

	if (games_sys == s) return;
	while (ngames) free(games[--ngames]);
	free(games);
	games = NULL;
	games_sys = s;
	snprintf(dir, sizeof dir, ROMS "/%s", SYS[s].dir);
	if (!(d = opendir(dir))) return;
	while ((e = readdir(d))) {
		if (!sys_has_ext(&SYS[s], e->d_name)) continue;
		if (ngames == cap) {
			char **g = realloc(games, (cap = cap ? cap * 2 : 64) * sizeof *games);

			if (!g) break;
			games = g;
		}
		games[ngames++] = strdup(e->d_name);
	}
	closedir(d);
	game_sort *g = malloc((size_t)ngames * sizeof *g);
	int i, k = 0;

	if (!g) return;
	for (i = 0; i < ngames; i++) {
		g[k].file = games[i];
		game_name(s, games[i], g[k].name, sizeof g[k].name);
		/* A title starting with "." is a BIOS set the core loads by
		 * itself (neogeo.zip): not a game to show. */
		if (g[k].name[0] == '.') free(games[i]);
		else k++;
	}
	ngames = k;
	qsort(g, ngames, sizeof *g, cmp_name);
	for (i = 0; i < ngames; i++) games[i] = g[i].file;
	free(g);
}


/* --------------------------------------------------------- recently played */

/* A game on the card: its console and its file in that console's folder.
 * recent.txt and favorites.txt keep them a line each, `folder<TAB>file`. */
typedef struct { int sys; char file[256]; } game_ref;

#define NRECENT 20
static game_ref recent[NRECENT];
static int nrecent;

static int refs_load(const char *path, game_ref *g, int max)
{
	char line[400];
	int n = 0;
	FILE *f = fopen(path, "r");

	if (!f) return 0;
	while (n < max && fgets(line, sizeof line, f)) {
		char *tab = strchr(line, '\t');
		int s;

		line[strcspn(line, "\n")] = '\0';
		if (!tab) continue;
		*tab = '\0';
		if (!tab[1]) continue;
		for (s = 0; s < NSYS && strcmp(SYS[s].dir, line); s++) { }
		if (s == NSYS) continue;
		g[n].sys = s;
		snprintf(g[n].file, sizeof g[0].file, "%s", tab + 1);
		n++;
	}
	fclose(f);
	return n;
}

static void refs_save(const char *path, const game_ref *g, int n)
{
	char tmp[PATH_MAX];
	int i;
	FILE *f;

	snprintf(tmp, sizeof tmp, "%s.new", path);
	if (!(f = fopen(tmp, "w"))) return;
	for (i = 0; i < n; i++) fprintf(f, "%s\t%s\n", SYS[g[i].sys].dir, g[i].file);
	if (fclose(f) == 0) rename(tmp, path);
	else unlink(tmp);
}

static void recent_load(void)
{
	nrecent = refs_load(RECENT, recent, NRECENT);
}

static void recent_add(int s, const char *file)
{
	int i, j;

	for (i = 0; i < nrecent; i++)
		if (recent[i].sys == s && !strcmp(recent[i].file, file)) break;
	if (i == nrecent && nrecent < NRECENT) nrecent++;
	if (i >= NRECENT) i = NRECENT - 1;
	for (j = i; j > 0; j--) recent[j] = recent[j - 1];
	recent[0].sys = s;
	snprintf(recent[0].file, sizeof recent[0].file, "%s", file);
	refs_save(RECENT, recent, nrecent);
}

/* -------------------------------------------------------------- favorites */

/* Y on a game puts it in Favorites or takes it out (ggv.31), kept A-Z by its
 * name; the game lists mark one with a "*". */
#define NFAVS 200
static game_ref favs[NFAVS];
static int nfavs;

static void favs_load(void)
{
	nfavs = refs_load(FAVS, favs, NFAVS);
}

static int fav_find(int s, const char *file)
{
	int i;

	for (i = 0; i < nfavs; i++)
		if (favs[i].sys == s && !strcmp(favs[i].file, file)) return i;
	return -1;
}

/* True when the game is a favorite now. */
static bool fav_toggle(int s, const char *file_in)
{
	char file[256], a[200], b[200];
	int i = fav_find(s, file_in);

	snprintf(file, sizeof file, "%s", file_in);   /* may be in favs[] */
	if (i >= 0) {
		memmove(&favs[i], &favs[i + 1], (size_t)(nfavs - i - 1) * sizeof favs[0]);
		nfavs--;
		refs_save(FAVS, favs, nfavs);
		return false;
	}
	if (nfavs == NFAVS) return false;
	game_name(s, file, a, sizeof a);
	for (i = 0; i < nfavs; i++) {
		game_name(favs[i].sys, favs[i].file, b, sizeof b);
		if (strcasecmp(a, b) < 0) break;
	}
	memmove(&favs[i + 1], &favs[i], (size_t)(nfavs - i) * sizeof favs[0]);
	favs[i].sys = s;
	snprintf(favs[i].file, sizeof favs[i].file, "%s", file);
	nfavs++;
	refs_save(FAVS, favs, nfavs);
	return true;
}

/* ------------------------------------------------------------------ music */

static ml_lib lib;
static volatile int lib_ready;   /* 1 once the walk below has finished */

/* The card's Music folder, walked once in the background at start: 4167
 * tracks took 1.8 s cold on the Nano (plorpos-ggv.4), which is not a wait to
 * put in front of the first screen. */
static void *lib_walk(void *arg)
{
	(void)arg;
	ml_scan_card(CARD, &lib);
	__sync_synchronize();
	lib_ready = 1;
	return NULL;
}

static int  cmd_fd = -1, cmd_keep = -1;
static char published[400];

/* What the game reads: see the top of the file. Written only when it changes,
 * to a new file renamed into place so a reader never sees half of one. */
static void publish(void)
{
	const mu_now *m = musec_now();
	const char *st = musec_heard() ? "playing" : m->state == MU_PAUSED ? "paused" : "stopped";
	char line[sizeof published];
	FILE *f;

	snprintf(line, sizeof line, "%s\t%s\t%s\n", st, m->title, m->artist);
	if (!strcmp(line, published)) return;
	if (!(f = fopen(NOW ".new", "w"))) return;
	fputs(line, f);
	if (fclose(f) == 0 && rename(NOW ".new", NOW) == 0)
		snprintf(published, sizeof published, "%s", line);
}

static void commands(void)
{
	char buf[128];
	ssize_t n;

	if (cmd_fd < 0) return;
	while ((n = read(cmd_fd, buf, sizeof buf - 1)) > 0) {
		char *p = buf, *nl;

		buf[n] = '\0';
		while ((nl = strchr(p, '\n'))) {
			*nl = '\0';
			if (!strcmp(p, "toggle")) musec_toggle();
			else if (!strcmp(p, "next")) musec_next();
			else if (!strcmp(p, "prev")) musec_prev();
			p = nl + 1;
		}
	}
}

/* A Muse left playing by a nanoshelf that died: the queue died with it, so
 * that track would play out and stop, with this process saying "stopped" and
 * a game fighting it for the codec meanwhile. Stopped now instead. */
static void stop_orphan(void)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);

	snprintf(sa.sun_path, sizeof sa.sun_path, "/tmp/muse.sock");
	if (fd >= 0 && connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0 &&
	    write(fd, "STOP\n", 5) == 5)
		fprintf(stderr, "nanoshelf: stopped a Muse left playing\n");
	if (fd >= 0) close(fd);
}

static void queue_load(void);

static void music_init(void)
{
	pthread_t th;

	stop_orphan();

	mkdir(STATE, 0777);
	if (mkfifo(CMD, 0666) != 0 && errno != EEXIST)
		fprintf(stderr, "nanoshelf: no %s: %s\n", CMD, strerror(errno));
	/* Read end non-blocking, and a write end of our own held open, so an
	 * empty FIFO reads as "nothing yet" rather than end of file. */
	cmd_fd = open(CMD, O_RDONLY | O_NONBLOCK);
	cmd_keep = open(CMD, O_WRONLY | O_NONBLOCK);
	musec_init(muse_bin, CARD);
	queue_load();
	if (pthread_create(&th, NULL, lib_walk, NULL) == 0) pthread_detach(th);
	else lib_walk(NULL);
}

/* A USB DAC (plorpos-ggv.8). Muse is moved to it while it is in, and falls
 * back to the speaker paused by itself when it is pulled (MUSE_HANDOVER's
 * "the DAC went"). Looked for every 2 s: the Nano has one USB port, so a DAC
 * comes and goes by hand and not often. */
static char dac[64];
static int  dac_card = -1;

/* Whether FunKey's `audio` poller is done with a DAC that has just come: it
 * writes /mnt/FunKey/.asoundrc, restarts alsa-utils (alsactl, which leaves a
 * DAC it has no state for at the DAC's own level - full, on a FiiO KA13) and
 * only then sets the saved level (`volume usbaudioinit`). Music moved over
 * before that blasted for a second (plorpos-ggv.19), so it waits for the file
 * and for none of those to be running. */
static bool funkey_dac_ready(void)
{
	static const char *const busy[] = { "alsactl", "alsa-utils", "sbin/volume", "amixer" };
	char path[288], cmd[256];
	struct dirent *e;
	bool ready = true;
	DIR *d;

	if (access("/mnt/FunKey/.asoundrc", F_OK) != 0) return false;
	if (!(d = opendir("/proc"))) return true;
	while (ready && (e = readdir(d))) {
		size_t n, i;
		FILE *f;

		if (!isdigit((unsigned char)e->d_name[0])) continue;
		snprintf(path, sizeof path, "/proc/%s/cmdline", e->d_name);
		if (!(f = fopen(path, "r"))) continue;
		n = fread(cmd, 1, sizeof cmd - 1, f);
		fclose(f);
		for (i = 0; i < n; i++) if (!cmd[i]) cmd[i] = ' ';
		cmd[n] = '\0';
		for (i = 0; i < sizeof busy / sizeof busy[0]; i++)
			if (strstr(cmd, busy[i])) ready = false;
	}
	closedir(d);
	return ready;
}

static void dac_poll(void)
{
	static unsigned last;
	char cards[2048], dev[64];
	int card = -1;
	size_t n = 0;
	FILE *f;

	if (last && plat_now_ms() - last < 2000) return;
	last = plat_now_ms();
	if ((f = fopen("/proc/asound/cards", "r"))) {
		n = fread(cards, 1, sizeof cards - 1, f);
		fclose(f);
	}
	cards[n] = '\0';
	if (!aout_usb_card(cards, dev, sizeof dev, &card)) { dev[0] = '\0'; card = -1; }
	if (!strcmp(dev, dac)) return;
	if (dev[0] && !funkey_dac_ready()) return;
	snprintf(dac, sizeof dac, "%s", dev);
	dac_card = card;
	musec_sink(dac);
	fprintf(stderr, "nanoshelf: output %s\n", dac[0] ? dac : "speaker");
}

/* Where a game's sound goes. The Nano's SDL plays through the kernel's OSS
 * emulation and opens /dev/dsp whatever AUDIODEV says (measured 2026-10-08),
 * so with a DAC in, /dev/dsp is pointed at the DAC's OSS device for the game
 * - card N's is char 14, minor 3 + 16N - and back at the codec's after it,
 * and at every start in case a game was running when this process died. */
static void dsp_point(int card)
{
	int minor = 3 + 16 * (card > 0 ? card : 0);
	dev_t want = makedev(14, minor);
	struct stat st;

	if (stat("/dev/dsp", &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == want) return;
	unlink("/dev/dsp.plorpos");
	if (mknod("/dev/dsp.plorpos", S_IFCHR | 0600, want) == 0 &&
	    rename("/dev/dsp.plorpos", "/dev/dsp") == 0)
		fprintf(stderr, "nanoshelf: /dev/dsp is card %d's\n", card > 0 ? card : 0);
	else
		fprintf(stderr, "nanoshelf: /dev/dsp for card %d: %s\n", card > 0 ? card : 0, strerror(errno));
}

/* The queue and its track kept over a restart (plorpos-ggv.25): written to the
 * card once it has stood still for QUEUE_SETTLE_MS - twenty skips through an
 * album are one write - and loaded stopped at start, for pause/play to go on
 * from that track. Track by track: the place in a song is not kept. */
#define QUEUE_SETTLE_MS 5000

static unsigned queue_sig(void)
{
	const char *const *paths, *artist, *album;
	int cur, n = musec_queue(&paths, &cur, &artist, &album), i;
	unsigned h = 2166136261u;

	if (n <= 0) return 0;
	h = (h ^ (unsigned)n) * 16777619u;
	h = (h ^ (unsigned)cur) * 16777619u;
	h = (h ^ (unsigned)musec_mode()) * 16777619u;
	for (i = 0; paths[0][i]; i++) h = (h ^ (unsigned char)paths[0][i]) * 16777619u;
	return h ? h : 1;
}

static void queue_save(void)
{
	const char *const *paths, *artist, *album;
	int cur, n = musec_queue(&paths, &cur, &artist, &album), i;
	FILE *f;

	if (n <= 0 || !(f = fopen(QUEUE ".new", "w"))) return;
	fprintf(f, "track\t%d\nmode\t%d\nartist\t%s\nalbum\t%s\n", cur, (int)musec_mode(), artist, album);
	for (i = 0; i < n; i++) fprintf(f, "%s\n", paths[i]);
	if (fclose(f) == 0) rename(QUEUE ".new", QUEUE);
	else unlink(QUEUE ".new");
}

static void queue_keep(void)
{
	static unsigned saved, seen, since;
	unsigned sig = queue_sig(), now = plat_now_ms();

	if (!saved) saved = seen = sig;            /* what start loaded, or nothing */
	if (sig != seen) { seen = sig; since = now; }
	if (sig && sig != saved && now - since >= QUEUE_SETTLE_MS) {
		queue_save();
		saved = sig;
	}
}

static void queue_load(void)
{
	char line[LIB_PATH + 16], artist[128] = "", album[128] = "";
	char **paths = NULL;
	int n = 0, cap = 0, cur = 0, mode = -1, i;
	FILE *f = fopen(QUEUE, "r");

	if (!f) return;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\n")] = '\0';
		if (!strncmp(line, "track\t", 6)) cur = atoi(line + 6);
		else if (!strncmp(line, "mode\t", 5)) mode = atoi(line + 5);
		else if (!strncmp(line, "artist\t", 7)) snprintf(artist, sizeof artist, "%s", line + 7);
		else if (!strncmp(line, "album\t", 6)) snprintf(album, sizeof album, "%s", line + 6);
		else if (line[0]) {
			if (n == cap) {
				char **p = realloc(paths, (size_t)(cap = cap ? cap * 2 : 64) * sizeof *p);

				if (!p) break;
				paths = p;
			}
			if (!(paths[n] = strdup(line))) break;
			n++;
		}
	}
	fclose(f);
	if (mode >= 0 && mode < MUQ_MODES) musec_set_mode((muq_mode)mode);
	if (n > 0) musec_load((const char *const *)paths, n, cur, 0, false, 1.0, artist, album);
	for (i = 0; i < n; i++) free(paths[i]);
	free(paths);
}

static void music_tick(void)
{
	dac_poll();
	musec_poll();
	commands();
	publish();
	queue_keep();
}

static void fmt_time(double s, char *out, size_t n)
{
	int t = s > 0 ? (int)s : 0;

	snprintf(out, n, "%d:%02d", t / 60, t % 60);
}


/* -------------------------------------------------------------- the screen */

static SDL_Surface *screen;
static TTF_Font    *font, *font_big;
static const SDL_Color C_TEXT = { 230, 230, 225, 0 }, C_DIM = { 140, 145, 150, 0 },
                       C_SEL = { 20, 20, 24, 0 }, C_ACC = { 120, 200, 160, 0 };
static Uint32 c_bg, c_bar, c_selbg, c_track, c_fill;

static bool video_start(void)
{
	if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) return false;
	screen = SDL_SetVideoMode(W, H, 16, SDL_SWSURFACE);
	if (!screen) return false;
	SDL_ShowCursor(0);
	SDL_EnableKeyRepeat(300, 60);
	c_bg    = SDL_MapRGB(screen->format, 16, 20, 24);
	c_bar   = SDL_MapRGB(screen->format, 32, 38, 44);
	c_selbg = SDL_MapRGB(screen->format, 120, 200, 160);
	c_track = SDL_MapRGB(screen->format, 60, 66, 72);
	c_fill  = SDL_MapRGB(screen->format, 120, 200, 160);
	return true;
}

static void video_stop(void)
{
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
	screen = NULL;
}

static void fill(int x, int y, int w, int h, Uint32 c)
{
	SDL_Rect r = { (Sint16)x, (Sint16)y, (Uint16)w, (Uint16)h };

	SDL_FillRect(screen, &r, c);
}

/* The play mode as a mark, not a word (ggv.15) - the shapes of the other
 * ports' ui.c (g_build there; menu.ttf has no arrows): strokes and arrowheads
 * in a unit square, 4x4 supersampled into an alpha mask once per mode. In
 * order has none, as there: it is what plays when nothing was asked for. */
typedef struct { float x0, y0, x1, y1; } g_seg;
typedef struct { float ax, ay, bx, by, cx, cy; } g_tri;
#define G_W   0.055f            /* half a stroke */
#define G_MAX 24
typedef struct {
	g_seg seg[G_MAX];
	g_tri tri[4];
	int   nseg, ntri;
	float w[G_MAX];
} g_shape;

static void g_line(g_shape *s, float x0, float y0, float x1, float y1, float w)
{
	if (s->nseg == G_MAX) return;
	s->w[s->nseg] = w;
	s->seg[s->nseg++] = (g_seg){ x0, y0, x1, y1 };
}

static void g_arc(g_shape *s, float cx, float cy, float r, float a0, float a1)
{
	int i;

	for (i = 0; i < 6; i++) {
		float t0 = a0 + (a1 - a0) * (float)i / 6.0f, t1 = a0 + (a1 - a0) * (float)(i + 1) / 6.0f;

		g_line(s, cx + r * cosf(t0), cy + r * sinf(t0), cx + r * cosf(t1), cy + r * sinf(t1), G_W);
	}
}

static void g_head(g_shape *s, float tx, float ty, float dx, float dy)
{
	float bx = tx - dx * 0.17f, by = ty - dy * 0.17f, px = -dy * 0.13f, py = dx * 0.13f;

	if (s->ntri == 4) return;
	s->tri[s->ntri++] = (g_tri){ tx, ty, bx + px, by + py, bx - px, by - py };
}

static void g_build(muq_mode m, g_shape *s)
{
	const float pi = 3.14159265f;

	memset(s, 0, sizeof *s);
	if (m == MUQ_SHUFFLE) {
		g_line(s, 0.10f, 0.32f, 0.30f, 0.32f, G_W);
		g_line(s, 0.30f, 0.32f, 0.60f, 0.68f, G_W);
		g_line(s, 0.60f, 0.68f, 0.72f, 0.68f, G_W);
		g_head(s, 0.89f, 0.68f, 1.0f, 0.0f);
		g_line(s, 0.10f, 0.68f, 0.30f, 0.68f, G_W);
		g_line(s, 0.30f, 0.68f, 0.60f, 0.32f, G_W);
		g_line(s, 0.60f, 0.32f, 0.72f, 0.32f, G_W);
		g_head(s, 0.89f, 0.32f, 1.0f, 0.0f);
		return;
	}
	g_line(s, 0.12f, 0.60f, 0.12f, 0.42f, G_W);
	g_arc(s, 0.30f, 0.42f, 0.18f, pi, 1.5f * pi);
	g_line(s, 0.30f, 0.24f, 0.68f, 0.24f, G_W);
	g_head(s, 0.86f, 0.24f, 1.0f, 0.0f);
	g_line(s, 0.88f, 0.40f, 0.88f, 0.58f, G_W);
	g_arc(s, 0.70f, 0.58f, 0.18f, 0.0f, 0.5f * pi);
	g_line(s, 0.70f, 0.76f, 0.32f, 0.76f, G_W);
	g_head(s, 0.14f, 0.76f, -1.0f, 0.0f);
	if (m == MUQ_REPEAT_ONE) {
		g_line(s, 0.50f, 0.38f, 0.50f, 0.62f, 0.04f);
		g_line(s, 0.50f, 0.38f, 0.445f, 0.43f, 0.04f);
	}
}

static int g_inside(const g_shape *s, float x, float y)
{
	int i;

	for (i = 0; i < s->nseg; i++) {
		const g_seg *g = &s->seg[i];
		float vx = g->x1 - g->x0, vy = g->y1 - g->y0, len2 = vx * vx + vy * vy;
		float t = len2 > 0 ? ((x - g->x0) * vx + (y - g->y0) * vy) / len2 : 0, dx, dy;

		if (t < 0) t = 0;
		if (t > 1) t = 1;
		dx = x - (g->x0 + t * vx);
		dy = y - (g->y0 + t * vy);
		if (dx * dx + dy * dy <= s->w[i] * s->w[i]) return 1;
	}
	for (i = 0; i < s->ntri; i++) {
		const g_tri *t = &s->tri[i];
		float d1 = (x - t->bx) * (t->ay - t->by) - (t->ax - t->bx) * (y - t->by);
		float d2 = (x - t->cx) * (t->by - t->cy) - (t->bx - t->cx) * (y - t->cy);
		float d3 = (x - t->ax) * (t->cy - t->ay) - (t->cx - t->ax) * (y - t->ay);
		int neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;

		if (!(neg && pos)) return 1;
	}
	return 0;
}

#define GLYPH_SZ (FONT_SZ + 1)

/* The mark for m in colour c, made the first time it is drawn. */
static SDL_Surface *mode_glyph(muq_mode m, SDL_Color c)
{
	static SDL_Surface *made[MUQ_MODES];
	g_shape shape;
	SDL_Surface *g;
	int x, y, i, j;

	if (m <= MUQ_IN_ORDER || m >= MUQ_MODES) return NULL;
	if (made[m]) return made[m];
	g = SDL_CreateRGBSurface(SDL_SRCALPHA, GLYPH_SZ, GLYPH_SZ, 32,
	                         0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
	if (!g) return NULL;
	g_build(m, &shape);
	SDL_LockSurface(g);
	for (y = 0; y < GLYPH_SZ; y++) {
		Uint32 *row = (Uint32 *)((Uint8 *)g->pixels + (size_t)y * g->pitch);

		for (x = 0; x < GLYPH_SZ; x++) {
			int hit = 0;

			for (j = 0; j < 4; j++)
				for (i = 0; i < 4; i++)
					hit += g_inside(&shape, ((float)x + ((float)i + 0.5f) / 4.0f) / GLYPH_SZ,
					                ((float)y + ((float)j + 0.5f) / 4.0f) / GLYPH_SZ);
			row[x] = (Uint32)(hit * 255 / 16) << 24 | (Uint32)c.r << 16 | (Uint32)c.g << 8 | c.b;
		}
	}
	SDL_UnlockSurface(g);
	return made[m] = g;
}

/* Text at x,y no wider than maxw, cut with "..." when it is. align: 0 left,
 * 1 centred in maxw, 2 right-aligned to x+maxw. */
static void text(TTF_Font *f, int x, int y, int maxw, const char *s, SDL_Color c, int align)
{
	char buf[256];
	int w = 0, h = 0;
	size_t n;
	SDL_Surface *t;
	SDL_Rect r;

	if (!s || !*s) return;
	snprintf(buf, sizeof buf, "%s", s);
	TTF_SizeUTF8(f, buf, &w, &h);
	n = strlen(buf);
	while (w > maxw && n > 3) {
		/* back off one UTF-8 character, then add the ellipsis */
		do n--; while (n > 0 && (buf[n] & 0xC0) == 0x80);
		memcpy(buf + n, "...", 4);
		TTF_SizeUTF8(f, buf, &w, &h);
	}
	if (!(t = TTF_RenderUTF8_Blended(f, buf, c))) return;
	r.x = (Sint16)(align == 1 ? x + (maxw - t->w) / 2 : align == 2 ? x + maxw - t->w : x);
	r.y = (Sint16)y;
	SDL_BlitSurface(t, NULL, screen, &r);
	SDL_FreeSurface(t);
}

/* ---------------------------------------------------------------- settings */

/* What FunKey's own menus offered that plorpOS needs, done with FunKey's own
 * tools: lending the card to a computer (share), the USB mode for the next
 * start (its flag files), restart and power off. Volume and brightness stay
 * on FN + A/Y and FN + X/B, which fkgpiod handles everywhere. */
enum { S_OUTPUT, S_LIBRARY, S_USB_DRIVE, S_USB_MODE, S_SLEEP, S_LOCK, S_RESTART, S_POWEROFF,
       S_KEYS, S_COUNT };
/* The rows shown: mounting the card on a computer only when this start made
 * the Nano a USB drive (its mass-storage function exists only then). */
static int srow[S_COUNT], nsrow;

static void settings_rows(void)
{
	int i;

	nsrow = 0;
	for (i = 0; i < S_COUNT; i++) {
#ifdef TRIMUI
		if (i == S_USB_DRIVE || i == S_USB_MODE) continue;   /* FunKey's */
#endif
		if (i != S_USB_DRIVE ||
		    !access("/sys/kernel/config/usb_gadget/FunKey/functions/mass_storage.mmcblk0p4", F_OK))
			srow[nsrow++] = i;
	}
}
static int confirm = -1;           /* the row whose A asks to be pressed again */
static char note[96];              /* a line over the footer for a few seconds */
static unsigned note_until;

static void set_note_ms(const char *msg, unsigned ms)
{
	snprintf(note, sizeof note, "%s", msg);
	note_until = plat_now_ms() + ms;
}

static void set_note(const char *msg) { set_note_ms(msg, 3000); }

static bool run_ok(const char *cmd)
{
	int r = system(cmd);

	return r != -1 && WIFEXITED(r) && WEXITSTATUS(r) == 0;
}

/* FunKey's /mnt flag files, read at start (/usr/local/sbin/usb_gadget): adb
 * wins over usbnet, and neither is a USB drive. */
static int usb_mode(void)
{
	return access(CARD "/adb", F_OK) == 0 ? 2 : access(CARD "/usbnet", F_OK) == 0 ? 1 : 0;
}

/* plorpOS's own settings, `name value` a line (ggv.28): the inactive
 * shutdown in minutes (0 = never) and the button lock while dimmed. */
#define SETTINGS DATA "/settings.txt"
#ifdef TRIMUI
#define POWEROFF "poweroff"
#else
#define POWEROFF "powerdown now"
#endif
static const int sleep_choices[] = { 1, 2, 5, 10, 30, 0 };
#define NSLEEP ((int)(sizeof sleep_choices / sizeof sleep_choices[0]))
static int set_sleep = 5, set_lock = 1;
static int set_pico8;              /* 1: the owner's PICO-8 for PICO-8 carts (ggv.42) */

static void settings_load(void)
{
	char k[32];
	int v;
	FILE *f = fopen(SETTINGS, "r");

	if (!f) return;
	while (fscanf(f, "%31s %d", k, &v) == 2) {
		if (!strcmp(k, "shutdown")) set_sleep = v;
		else if (!strcmp(k, "lock")) set_lock = !!v;
		else if (!strcmp(k, "pico8")) set_pico8 = !!v;
	}
	fclose(f);
}

static void settings_save(void)
{
	FILE *f = fopen(SETTINGS ".new", "w");

	if (!f) return;
	fprintf(f, "shutdown %d\nlock %d\npico8 %d\n", set_sleep, set_lock, set_pico8);
	if (fclose(f) == 0) rename(SETTINGS ".new", SETTINGS);
	else unlink(SETTINGS ".new");
}

static void sleep_step(int dir)
{
	int i;

	for (i = 0; i < NSLEEP && sleep_choices[i] != set_sleep; i++) ;
	set_sleep = sleep_choices[(i + NSLEEP + dir) % NSLEEP];
}

static void usb_mode_set(int m)
{
	unlink(CARD "/adb");
	unlink(CARD "/usbnet");
	if (m) close(open(m == 2 ? CARD "/adb" : CARD "/usbnet", O_WRONLY | O_CREAT, 0644));
	sync();
}

static const char *usb_mode_name(int m)
{
	return m == 2 ? "adb" : m == 1 ? "Network (ssh)" : "USB drive";
}

/* ------------------------------------------------------------------- views */

enum { V_HOME, V_GAMES, V_RECENT, V_SETTINGS, V_NOW, V_ARTISTS, V_ALBUMS, V_TRACKS, V_QUEUE, V_CONTROLS, V_FAVS };
typedef struct { int v, arg, sel, top; } view;
static view stack[8];
static int  depth;
#define CUR (stack[depth])

/* Home's rows, rebuilt each time it is drawn: what they are depends on what
 * is on the card and whether anything is queued. */
enum { H_NOW, H_MUSIC, H_RECENT, H_FAVS, H_SYS, H_SETTINGS };
static struct { int kind, sys; } home[NSYS + 5];
static int  nhome;
static bool sys_shown[NSYS];

static void home_build(void)
{
	int s;

	nhome = 0;
	if (musec_now()->count > 0) home[nhome++].kind = H_NOW;
	home[nhome++].kind = H_MUSIC;
	if (nrecent) home[nhome++].kind = H_RECENT;
	if (nfavs) home[nhome++].kind = H_FAVS;
	for (s = 0; s < NSYS; s++)
		if (sys_shown[s]) { home[nhome].kind = H_SYS; home[nhome++].sys = s; }
	home[nhome++].kind = H_SETTINGS;
}

static int view_len(const view *v)
{
	switch (v->v) {
	case V_HOME:     return nhome;
	case V_GAMES:    return ngames;
	case V_RECENT:   return nrecent;
	case V_FAVS:     return nfavs;
	case V_SETTINGS: return nsrow;
	case V_ARTISTS:  return lib_ready ? lib.nartists : 0;
	case V_ALBUMS:   return lib_ready ? lib.artists[v->arg].n : 0;
	case V_TRACKS:   return lib_ready ? lib.albums[v->arg].n : 0;
	case V_QUEUE:    return musec_now()->count;
	}
	return 0;
}

static const char *output_name(void)
{
	const char *s = musec_sink_now();

	return !strncmp(s, "plughw:CARD=", 12) ? "USB DAC" : "Speaker";
}

/* Row i's text for a list view, into buf. */
static void row_label(const view *v, int i, char *buf, size_t n)
{
	const mu_now *m = musec_now();

	buf[0] = '\0';
	switch (v->v) {
	case V_HOME:
		switch (home[i].kind) {
		case H_NOW:      snprintf(buf, n, "Now Playing: %s", m->title[0] ? m->title : "-"); break;
		case H_MUSIC:    snprintf(buf, n, "Music"); break;
		case H_RECENT:   snprintf(buf, n, "Recently Played"); break;
		case H_FAVS:     snprintf(buf, n, "Favorites"); break;
		case H_SYS:      snprintf(buf, n, "%s", SYS[home[i].sys].label); break;
		case H_SETTINGS: snprintf(buf, n, "Settings"); break;
		}
		break;
	case V_GAMES:
	case V_RECENT: {
		char g[200];
		int s = v->v == V_GAMES ? v->arg : recent[i].sys;
		const char *file = v->v == V_GAMES ? games[i] : recent[i].file;

		game_name(s, file, g, sizeof g);
		snprintf(buf, n, "%s%s", fav_find(s, file) >= 0 ? "* " : "", g);
		break;
	}
	case V_FAVS:
		game_name(favs[i].sys, favs[i].file, buf, n);
		break;
	case V_SETTINGS:
		switch (srow[i]) {
		case S_OUTPUT:    snprintf(buf, n, "Output: %s", output_name()); break;
		case S_LIBRARY:   snprintf(buf, n, "Library: %d tracks", lib_ready ? lib.ntracks : 0); break;
		case S_USB_DRIVE: snprintf(buf, n, "Mount card on computer"); break;
		case S_USB_MODE:  snprintf(buf, n, "USB at start: %s", usb_mode_name(usb_mode())); break;
		case S_SLEEP:
			if (set_sleep) snprintf(buf, n, "Inactive shutdown: %d min", set_sleep);
			else snprintf(buf, n, "Inactive shutdown: Never");
			break;
		case S_LOCK:      snprintf(buf, n, "Dimmed button lock: %s", set_lock ? "On" : "Off"); break;
		case S_RESTART:   snprintf(buf, n, confirm == i ? "Restart? Press A again" : "Restart"); break;
		case S_POWEROFF:  snprintf(buf, n, confirm == i ? "Power off? Press A again" : "Power off"); break;
		case S_KEYS:      snprintf(buf, n, "Controls"); break;
		}
		break;
	case V_ARTISTS:
		snprintf(buf, n, "%s", lib.artists[i].name);
		break;
	case V_ALBUMS:
		snprintf(buf, n, "%s", lib.albums[lib.artists[v->arg].first + i].name);
		break;
	case V_TRACKS: {
		const ml_album *a = &lib.albums[v->arg];
		const char *path = lib.tracks[a->first + i].path;

		snprintf(buf, n, "%s%s", !strcmp(path, musec_path()) ? "> " : "", lib.tracks[a->first + i].name);
		break;
	}
	case V_QUEUE: {
		const char *t = musec_track(i);
		const char *slash = t ? strrchr(t, '/') : NULL;

		if (t) ml_track_name(slash ? slash + 1 : t, buf, (int)n);
		if (i == m->index) {
			char tmp[256];

			snprintf(tmp, sizeof tmp, "> %s", buf);
			snprintf(buf, n, "%s", tmp);
		}
		break;
	}
	}
}

/* Settings > Controls: what each button does, a page per place (ggv.21). */
typedef struct { const char *name; const char *rows[8][2]; } controls_t;
#ifdef TRIMUI
static const controls_t controls[] = {
	{ "Shelf", {
		{ "Up / Down",    "move" },
		{ "Left / Right", "page" },
		{ "A",            "open" },
		{ "B",            "back" },
		{ "X or MENU",    "Now Playing" },
		{ "Y (a game)",   "favorite on/off" },
		{ "START",        "play / pause" } } },
	{ "Now Playing", {
		{ "L / R",        "track back / next" },
		{ "Left / Right", "seek 10 s" },
		{ "Y",            "play mode" },
		{ "X",            "queue" } } },
	{ "In a game", {
		{ "MENU",         "game menu" },
		{ "B",            "close menu" } } },
};
#else
static const controls_t controls[] = {
	{ "Shelf", {
		{ "Up / Down",    "move" },
		{ "Left / Right", "page" },
		{ "A",            "open" },
		{ "B",            "back" },
		{ "X",            "Now Playing" },
		{ "Y (a game)",   "favorite on/off" },
		{ "START",        "play / pause" },
		{ "Power tap",    "dim Now Playing" } } },
	{ "Now Playing", {
		{ "L / R",        "track back / next" },
		{ "Left / Right", "seek 10 s" },
		{ "Y",            "play mode" },
		{ "X",            "queue" },
		{ "Power tap",    "brightness back" } } },
	{ "In a game", {
		{ "MENU (power tap)", "game menu" },
		{ "Up / Down",    "menu page" },
		{ "MUSIC page: A", "play / pause" },
		{ "B",            "close menu" } } },
	{ "Anywhere", {
		{ "SELECT (FN)+A / Y", "volume up / down" },
		{ "SELECT (FN)+X / B", "brightness up / down" },
		{ "SELECT (FN)+Up",    "screenshot" },
		{ "Power hold",        "off; a game resumes" } } },
};
#endif
#define NCONTROLS ((int)(sizeof controls / sizeof controls[0]))

static const char *view_title(const view *v)
{
	static char t[160];

	switch (v->v) {
	case V_HOME:     return DEVICE;
	case V_GAMES:    return SYS[v->arg].label;
	case V_RECENT:   return "Recently Played";
	case V_FAVS:     return "Favorites";
	case V_SETTINGS: return DEVICE " v" TORTOS_VERSION;
	case V_NOW:      return "Now Playing";
	case V_ARTISTS:  return "Music";
	case V_ALBUMS:   snprintf(t, sizeof t, "%s", lib.artists[v->arg].name); return t;
	case V_TRACKS:   snprintf(t, sizeof t, "%s", lib.albums[v->arg].name); return t;
	case V_QUEUE:    return "Queue";
	case V_CONTROLS:
		snprintf(t, sizeof t, "%s  %d/%d", controls[v->sel].name, v->sel + 1, NCONTROLS);
		return t;
	}
	return "";
}

/* -------------------------------------------------------- native PICO-8 */

/* The owner's PICO-8 instead of fake08 (plorpos-ggv.42): pico8_dyn and
 * pico8.dat, from PICO-8's Raspberry Pi download, beside the other BIOS
 * files, run on the glibc + SDL2 runtime in pico8rt with its preload
 * (mk/build-nano-pico8rt.sh, tools/nano-pico8.c). fake08 stays the default;
 * R on the PICO-8 list turns it over, only when all of it is there. */

static bool is_pico8(int s) { return !strcmp(SYS[s].core, "fake08"); }

static bool pico8_native_ready(void)
{
	char ld[PATH_MAX + 32];

	snprintf(ld, sizeof ld, "%s/ld-linux-armhf.so.3", pico8rt);
	return access(BIOS "/pico8_dyn", R_OK) == 0 &&
	       access(BIOS "/pico8.dat", R_OK) == 0 && access(ld, X_OK) == 0;
}

/* In the child: PICO-8 sized to the panel by its own flags, so its
 * config.txt stays the owner's. SDL draws nowhere and reads no keys; the
 * preload does both, Muse's handover, and the power tap's menu - FunKey's,
 * from PicoArch run as `picoarch --menu` (see tools/nano-pico8.c). */
static void pico8_exec(const char *rom, const char *home)
{
	char ld[PATH_MAX + 32], pre[PATH_MAX + 32], root[512], desk[640];
	const char *slash = strrchr(rom, '/');

	snprintf(ld, sizeof ld, "%s/ld-linux-armhf.so.3", pico8rt);
	snprintf(pre, sizeof pre, "%s/nano-pico8.so", pico8rt);
	snprintf(root, sizeof root, "%.*s", slash ? (int)(slash - rom) : 0, rom);
	snprintf(desk, sizeof desk, "%s/desktop", home);
	setenv("PLORPOS_PICO8_EXE", BIOS "/pico8_dyn", 1);
	setenv("PLORPOS_MENU", picoarch_bin, 1);      /* the power tap's: picoarch --menu */
	setenv("SDL_VIDEODRIVER", "dummy", 1);
	setenv("SDL_AUDIODRIVER", "dsp", 1);
	execl(ld, "ld-linux-armhf.so.3", "--library-path", pico8rt, "--preload", pre,
	      BIOS "/pico8_dyn", "-home", home, "-root_path", root,
	      "-desktop", desk, "-windowed", "1", "-width", "240",
	      "-height", "240", "-draw_rect", "0,0,240,240", "-software_blit", "1",
	      "-foreground_sleep_ms", "2", "-run", rom, (char *)NULL);
}

static const char *view_hint(const view *v)
{
	switch (v->v) {
	case V_HOME:   return "A open  X music  START pause";
	case V_NOW:    return "A pause  L/R track  Y mode";
	case V_TRACKS: return "A play  B back  X music";
	case V_QUEUE:  return "B back";
	case V_CONTROLS: return "Left/Right page  B back";
	case V_GAMES:
		/* R turns PICO-8's engine over, when there is a choice (ggv.42). */
		if (is_pico8(v->arg) && pico8_native_ready())
			return set_pico8 ? "A play  Y fav  R pico8" : "A play  Y fav  R fake08";
		return "A play  Y fav  B back";
	case V_RECENT: case V_FAVS: return "A play  Y fav  B back";
	}
	return "A open  B back  X music";
}

/* The battery, from the AXP209 (ggv.30): read every 30 s, "+" while it
 * charges. */
#define BATTERY "/sys/class/power_supply/axp20x-battery/"
static int  bat_pct = -1;
static bool bat_chg;

static void battery_poll(void)
{
	static unsigned last;
	char st[32] = "";
	int pct = -1;
	FILE *f;

	if (last && plat_now_ms() - last < 30000) return;
	last = plat_now_ms();
	if ((f = fopen(BATTERY "capacity", "r"))) { if (fscanf(f, "%d", &pct) != 1) pct = -1; fclose(f); }
	if ((f = fopen(BATTERY "status", "r"))) { if (!fgets(st, sizeof st, f)) st[0] = '\0'; fclose(f); }
	bat_pct = pct;
	bat_chg = !strncmp(st, "Charging", 8);
}

static void draw_header(const view *v)
{
	const mu_now *m = musec_now();
	const char *mark = musec_heard() ? ">" : m->state == MU_PAUSED ? "||" : "";
	char bat[16] = "";
	int bw = 0, mw = 0, h;

	if (bat_pct >= 0) snprintf(bat, sizeof bat, "%s%d%%", bat_chg ? "+" : "", bat_pct);
	if (bat[0]) TTF_SizeUTF8(font, bat, &bw, &h);
	if (mark[0]) TTF_SizeUTF8(font, mark, &mw, &h);
	fill(0, 0, W, TOP, c_bar);
	if (bat[0]) text(font, W - 6 - bw, 3, bw, bat, C_DIM, 0);
	if (mark[0]) text(font, W - 6 - bw - (bw ? 8 : 0) - mw, 3, mw, mark, C_ACC, 0);
	text(font, 6, 3, W - 12 - bw - mw - 16, view_title(v), C_TEXT, 0);
}

static void draw_list(view *v)
{
	char buf[256];
	int n = view_len(v), i;

	if (v->sel >= n) v->sel = n ? n - 1 : 0;
	if (v->sel < v->top) v->top = v->sel;
	if (v->sel >= v->top + ROWS) v->top = v->sel - ROWS + 1;
	if (n == 0) {
		const char *msg = v->v >= V_ARTISTS && !lib_ready ? "Reading the music library..." :
		                  v->v == V_RECENT ? "Nothing played yet" : "Nothing here";

		text(font, 8, TOP + 8, W - 16, msg, C_DIM, 0);
		return;
	}
	for (i = v->top; i < n && i < v->top + ROWS; i++) {
		int y = TOP + (i - v->top) * ROW;

		row_label(v, i, buf, sizeof buf);
		if (i == v->sel) fill(0, y, W, ROW, c_selbg);
		text(font, 8, y + 1, W - 16, buf, i == v->sel ? C_SEL : C_TEXT, 0);
	}
	if (n > ROWS) {     /* where in the list: a thin bar down the right */
		int bh = (H - TOP - BOT) * ROWS / n, by = TOP + (H - TOP - BOT - bh) * v->top / (n - ROWS);

		fill(W - 3, by, 2, bh < 6 ? 6 : bh, c_track);
	}
}

static void draw_now(void)
{
	const mu_now *m = musec_now();
	char a[16], b[16], line[200];
	const char *next = musec_upcoming();
	int y = TOP + 10;

	if (m->count == 0 && !m->title[0]) {
		text(font, 8, y, W - 16, "Nothing playing. Music > an album.", C_DIM, 0);
		return;
	}
	text(font_big, 8, y, W - 16, m->title, C_TEXT, 1);           y += FONT_BIG + 9;
	text(font, 8, y, W - 16, m->artist, C_ACC, 1);                y += FONT_SZ + 7;
	text(font, 8, y, W - 16, m->album, C_DIM, 1);                 y += FONT_SZ + 17;
	fill(12, y, W - 24, 4, c_track);
	if (m->len > 0) fill(12, y, (int)((W - 24) * (m->at / m->len > 1 ? 1 : m->at / m->len)), 4, c_fill);
	y += 8;
	fmt_time(m->at, a, sizeof a);
	fmt_time(m->len, b, sizeof b);
	text(font, 12, y, 60, a, C_DIM, 0);
	text(font, W - 72, y, 60, b, C_DIM, 2);                      y += FONT_SZ + 9;
	snprintf(line, sizeof line, "%s  -  %d of %d",
	         m->state == MU_PAUSED ? "Paused" : musec_heard() ? "Playing" : "Stopped",
	         m->count ? m->index + 1 : 0, m->count);
	{   /* the line and the mode's mark after it, centred together */
		SDL_Surface *g = mode_glyph(musec_mode(), C_DIM);
		int lw = 0, lh = 0, gw = g ? GLYPH_SZ + 8 : 0, x0;

		TTF_SizeUTF8(font, line, &lw, &lh);
		if (lw > W - 16 - gw) lw = W - 16 - gw;
		x0 = (W - lw - gw) / 2;
		text(font, x0, y, lw, line, C_DIM, 0);
		if (g) {
			SDL_Rect r = { (Sint16)(x0 + lw + 8), (Sint16)(y + (lh - GLYPH_SZ) / 2), 0, 0 };

			SDL_BlitSurface(g, NULL, screen, &r);
		}
	}
	y += FONT_SZ + 7;
	if (next) {
		const char *slash = strrchr(next, '/');
		char nm[160];

		ml_track_name(slash ? slash + 1 : next, nm, sizeof nm);
		snprintf(line, sizeof line, "Next: %s", nm);
		text(font, 8, y, W - 16, line, C_DIM, 1);
	}
}

/* A button left, what it does right; below it, indented, when both do not
 * fit on one row. */
static void draw_controls(int page)
{
	const controls_t *c = &controls[page];
	int i, y = TOP + 4;

	for (i = 0; i < 8 && c->rows[i][0]; i++) {
		int kw = 0, aw = 0, h;

		TTF_SizeUTF8(font, c->rows[i][0], &kw, &h);
		TTF_SizeUTF8(font, c->rows[i][1], &aw, &h);
		text(font, 8, y, W - 16, c->rows[i][0], C_ACC, 0);
		if (kw + 12 + aw > W - 16) y += ROW;
		text(font, 8, y, W - 16, c->rows[i][1], C_TEXT, 2);
		y += ROW;
	}
}

static void draw(void)
{
	view *v = &CUR;

	if (v->v == V_HOME) home_build();
	fill(0, 0, W, H, c_bg);
	draw_header(v);
	if (v->v == V_NOW) draw_now();
	else if (v->v == V_CONTROLS) draw_controls(v->sel);
	else draw_list(v);
	fill(0, H - BOT, W, BOT, c_bar);
	if ((int)(note_until - plat_now_ms()) > 0)
		text(font, 6, H - BOT + 1, W - 12, note, C_ACC, 0);
	else
		text(font, 6, H - BOT + 1, W - 12, view_hint(v), C_DIM, 0);
	SDL_Flip(screen);
}

/* What a redraw is owed to, other than input: the music, but only where it
 * shows. The header's mark is on every screen; the rest only where they
 * appear. */
static unsigned music_sig(void)
{
	const mu_now *m = musec_now();
	unsigned h = (unsigned)musec_heard() * 3u + (unsigned)m->state * 7u + (unsigned)m->index * 131u;
	const char *p;

	for (p = m->title; *p; p++) h = h * 31u + (unsigned char)*p;
	h += (unsigned)lib_ready * 977u;
	h += (unsigned)(bat_pct + 1) * 7919u + (unsigned)bat_chg * 15731u;
	h += ((int)(note_until - plat_now_ms()) > 0) * 4099u;
	if (CUR.v == V_NOW) h += (unsigned)m->at * 1009u + (unsigned)musec_mode() * 17u;
	return h;
}

/* ------------------------------------------------------------------ launch */

static volatile sig_atomic_t want_quit;
static unsigned active_at;          /* the last button, music or game (ggv.28) */
static void on_term(int sig) { (void)sig; want_quit = 1; }

/* FunKey's power key signals the pid in /var/run/funkey.pid (its `pid` tool):
 * the game while one runs - PicoArch saves and powers off - and this process
 * otherwise. */
static void pid_record(pid_t pid)
{
	char exe[64], path[PATH_MAX];
	ssize_t n;
	FILE *f = fopen("/var/run/funkey.pid", "w");

	if (f) { fprintf(f, "%d\n", (int)pid); fclose(f); }
	snprintf(exe, sizeof exe, "/proc/%d/exe", (int)pid);
	if ((n = readlink(exe, path, sizeof path - 1)) > 0) {
		char *slash;

		path[n] = '\0';
		if ((slash = strrchr(path, '/'))) *slash = '\0';
		if ((f = fopen("/var/run/pid_path", "w"))) { fputs(path, f); fclose(f); }
	}
}

/* Latency, input to the frame on the panel, kept as a summary per run (lean
 * logging): written when a game starts and at exit. */
static unsigned lat_n, lat_sum, lat_max;
static bool     launched;   /* the input started a game: its redraw is after it */

static void lat_report(const char *when)
{
	if (lat_n)
		fprintf(stderr, "nanoshelf: %s: %u redraws after input, %u ms average, %u ms worst\n",
		        when, lat_n, lat_sum / lat_n, lat_max);
	lat_n = lat_sum = lat_max = 0;
}

#ifdef TRIMUI
/* The Model S has no cpufreq: the clock is PLL_CPU's low half, written
 * through /dev/mem as MinUI does, with its values. 720 MHz everywhere:
 * MinUI's "highest stable" 864 (0x1a32) froze the whole device three times
 * in games on the test unit (plorpos-80b.7). */
#include <sys/mman.h>
#define CPU_SHELF 0x1d22
#define CPU_GAME  0x1d22
static void cpu_clock(unsigned low)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	volatile uint32_t *pll;

	if (fd < 0) return;
	pll = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x01c20000);
	close(fd);
	if (pll == MAP_FAILED) return;
	*pll = (*pll & 0xffff0000u) | low;
	munmap((void *)pll, 4096);
}
#endif

/* A game at ROMS/<folder>/<file> saves in SAVES/<folder>, made here. */
static void saves_dir(const char *rom, char *out, size_t n)
{
	const char *f = rom + sizeof ROMS, *e;

	if (strncmp(rom, ROMS "/", sizeof ROMS) || !(e = strchr(f, '/'))) e = f = "";
	mkdir(SAVES, 0755);
	snprintf(out, n, SAVES "/%.*s", (int)(e - f), f);
	mkdir(out, 0755);
}

/* PicoArch with core and rom - or the owner's PICO-8 when core is NULL -
 * music going on around it, then the shelf's screen back. Its saves go to
 * Saves/<console folder>/ (PicoArch reads PLORPOS_SAVES; PICO-8 gets
 * native/ in it as its home, cartdata and all); BIOS files come from Bios/
 * (PLORPOS_BIOS). */
static void run_game(const char *core, const char *rom)
{
	char saves[600], home[620];
	unsigned t0;
	pid_t pid;
	int st;

	saves_dir(rom, saves, sizeof saves);
	if (!core) {
		/* PICO-8 makes neither its -home nor its -desktop. */
		snprintf(home, sizeof home, "%s/native", saves);
		mkdir(home, 0755);
		snprintf(home, sizeof home, "%s/native/desktop", saves);
		mkdir(home, 0755);
		snprintf(home, sizeof home, "%s/native", saves);
	}

	launched = true;
#ifdef TRIMUI
	cpu_clock(CPU_GAME);
#endif
	dac_poll();
	dsp_point(dac_card);
	video_stop();
	t0 = plat_now_ms();
	pid = fork();
	if (pid == 0) {
		int fd;

		for (fd = 3; fd < 256; fd++) close(fd);
		signal(SIGUSR1, SIG_DFL);
		setenv("PLORPOS_SAVES", saves, 1);
		setenv("PLORPOS_BIOS", BIOS, 1);
		if (core) execl(picoarch_bin, "picoarch", core, rom, (char *)NULL);
		else pico8_exec(rom, home);
		_exit(127);
	}
	if (pid > 0) {
		pid_record(pid);
		while (waitpid(pid, &st, WNOHANG) == 0) {
			music_tick();
			usleep(100000);
		}
		fprintf(stderr, "nanoshelf: %s: %u s, exit %d\n", rom,
		        (plat_now_ms() - t0) / 1000, WIFEXITED(st) ? WEXITSTATUS(st) : -WTERMSIG(st));
	}
	pid_record(getpid());
	dsp_point(-1);
	active_at = plat_now_ms();          /* a game is not inactivity */
	/* What FunKey's frontend loop does after every program: a game killed
	 * mid-way leaves VT switching locked and the keyboard dead. */
#ifdef TRIMUI
	cpu_clock(CPU_SHELF);
#else
	system("termfix_all >/dev/null 2>&1; keymap default >/dev/null 2>&1");
#endif
	if (!video_start()) {
		fprintf(stderr, "nanoshelf: video after the game: %s\n", SDL_GetError());
		want_quit = 1;
	}
}

static void launch(int s, const char *file_in)
{
	char core[256], rom[512], file[256];

	/* A copy: from Recently Played, file_in is in recent[], which
	 * recent_add rewrites. */
	snprintf(file, sizeof file, "%s", file_in);

	core_path(&SYS[s], core, sizeof core);
	snprintf(rom, sizeof rom, ROMS "/%s/%s", SYS[s].dir, file);
	recent_add(s, file);
	if (!strcmp(SYS[s].core, "mednafen_lynx") &&
	    access(BIOS "/lynxboot.img", R_OK) != 0)
		system("mkdir -p " BIOS " && cp /usr/games/lynxboot.img " BIOS "/");
	lat_report("before a game");
	run_game(is_pico8(s) && set_pico8 && pico8_native_ready() ? NULL : core, rom);
}

/* ------------------------------------------------------------ instant play */

/* The power key in a game: PicoArch saves, FunKey's `instant_play save`
 * writes the command line to INSTANT_PLAY as a little script,
 *   'program' 'core' 'rom' &
 *   pid record $! ...
 * and powers off. FunKey's /root/.profile ran it at the next boot, before
 * any menu - so no music during it. install-root.sh turned that off; the
 * shelf runs it here instead, before its first screen, with the queue
 * already back (stopped on its track: the MUSIC page plays it). Taken out of
 * the way first, as FunKey's `instant_play load` does, so a crash in the game
 * does not bring it back at every boot. */
#define INSTANT_PLAY "/mnt/instant_play"
#define RESUME_PLAY  "/mnt/resume_play"

/* The next 'quoted' word of a line `instant_play save` wrote ('\'' inside). */
static const char *ip_word(const char *p, char *out, size_t n)
{
	size_t k = 0;

	while (*p == ' ') p++;
	if (*p++ != '\'') return NULL;
	for (;;) {
		if (!*p || *p == '\n') return NULL;
		if (*p == '\'') {
			if (strncmp(p, "'\\''", 4)) break;
			p += 3;
		}
		if (k + 1 < n) out[k++] = *p;
		p++;
	}
	out[k] = '\0';
	return p + 1;
}

static unsigned uptime_ms(void)
{
	double up = 0;
	FILE *f = fopen("/proc/uptime", "r");

	if (f) { if (fscanf(f, "%lf", &up) != 1) up = 0; fclose(f); }
	return (unsigned)(up * 1000);
}

static void resume_instant_play(unsigned up_start)
{
	char line[1024], prog[PATH_MAX], core[256], rom[512];
	const char *p, *name;
	FILE *f;

	if (rename(INSTANT_PLAY, RESUME_PLAY) != 0) return;
	line[0] = '\0';
	if ((f = fopen(RESUME_PLAY, "r"))) {
		if (!fgets(line, sizeof line, f)) line[0] = '\0';
		fclose(f);
	}
	if ((p = ip_word(line, prog, sizeof prog)) && (p = ip_word(p, core, sizeof core)) &&
	    ip_word(p, rom, sizeof rom) &&
	    !strcmp((name = strrchr(prog, '/')) ? name + 1 : prog, "picoarch")) {
		fprintf(stderr, "nanoshelf: instant play %s, %u ms after the shelf started (uptime %u ms)\n",
		        rom, uptime_ms() - up_start, uptime_ms());
		run_game(core, rom);
	} else {
		/* Not PicoArch's: as FunKey ran it, without the music page. */
		fprintf(stderr, "nanoshelf: instant play, not PicoArch: %s", line);
		video_stop();
		system("sh " RESUME_PLAY "; termfix_all >/dev/null 2>&1; keymap default >/dev/null 2>&1");
		pid_record(getpid());
		if (!video_start()) want_quit = 1;
	}
	remove(RESUME_PLAY);
}

/* ---------------------------------------------------------------- USB drive */

static void message(const char *l1, const char *l2)
{
	fill(0, 0, W, H, c_bg);
	fill(0, 0, W, TOP, c_bar);
	text(font, 6, 3, W - 12, "USB drive", C_TEXT, 0);
	text(font, 8, 90, W - 16, l1, C_TEXT, 1);
	text(font, 8, 90 + FONT_SZ + 9, W - 16, l2, C_DIM, 1);
	SDL_Flip(screen);
}

/* What is on the card, read again after the computer had it. */
static void rescan_card(void)
{
	pthread_t th;
	int s;

	for (s = 0; s < NSYS; s++) sys_shown[s] = sys_has_games(&SYS[s]);
	recent_load();
	games_sys = -1;
	ngames = 0;
	lib_ready = 0;
	__sync_synchronize();
	ml_free(&lib);
	if (pthread_create(&th, NULL, lib_walk, NULL) == 0) pthread_detach(th);
	else lib_walk(NULL);
}

/* Lend the card's partition to a computer: FunKey's `share start` unmounts
 * /mnt and offers it over USB, which only works with nothing open on it - so
 * the music stops first, and an install that runs from the card itself (not
 * install-root.sh's /usr/local/plorpos) cannot do it at all. B takes it back
 * once the computer has let go. */
static void usb_drive(void)
{
	SDL_Event e;

	if (!lib_ready) { set_note("Still reading the music library"); return; }
	if (!run_ok("share is_usb_data_connected >/dev/null 2>&1")) {
		set_note("Connect a computer by USB first");
		return;
	}
	/* The mass-storage function exists only when the Nano started as a USB
	 * drive; started for adb or the network, it is not there to lend. */
	if (access("/sys/kernel/config/usb_gadget/FunKey/functions/mass_storage.mmcblk0p4/lun.0/file", F_OK)) {
		set_note("Set USB at start: USB drive, restart");
		return;
	}
	musec_stop();
	music_tick();
	message("Lending the card...", "");
	usleep(300000);
	if (!run_ok("share start >/tmp/plorpos/share.log 2>&1")) {
		set_note(access("/usr/local/plorpos/bin/nanoshelf", X_OK) ? "Needs install-root.sh first"
		                                                          : "The card is in use");
		return;
	}
	fprintf(stderr, "nanoshelf: card lent to a computer\n");
	message("The computer has the card.", "Eject it there, then press B.");
	while (!want_quit) {
		if (!SDL_WaitEvent(&e)) break;
		if (e.type != SDL_KEYDOWN || e.key.keysym.sym != SDLK_b) continue;
		if (run_ok("share stop >>/tmp/plorpos/share.log 2>&1")) break;
		message("The computer still has it.", "Eject it there, then press B.");
	}
	fprintf(stderr, "nanoshelf: card back\n");
	rescan_card();
}

/* ------------------------------------------------------------------- input */

static void push(int v, int arg)
{
	if (depth + 1 >= (int)(sizeof stack / sizeof stack[0])) return;
	depth++;
	stack[depth] = (view){ v, arg, 0, 0 };
}

static void open_now(void)
{
	int i;

	for (i = 0; i <= depth; i++)
		if (stack[i].v == V_NOW) { depth = i; return; }
	push(V_NOW, 0);
}

static void play_album(int al, int start)
{
	const ml_album *a = &lib.albums[al];
	const char *paths[a->n ? a->n : 1];
	const char *artist = "";
	int i;

	for (i = 0; i < lib.nartists; i++)
		if (al >= lib.artists[i].first && al < lib.artists[i].first + lib.artists[i].n)
			artist = lib.artists[i].name;
	for (i = 0; i < a->n; i++) paths[i] = lib.tracks[a->first + i].path;
	musec_play(paths, a->n, start, 0, a->book, 1.0, artist, a->name);
	publish();     /* "playing" now, so a game would let go in time */
}

/* A row with choices, one along (dir 1) or back (-1): A and Right go along,
 * Left back. Other rows ignore it. */
static void setting_step(int id, int dir)
{
	switch (id) {
	case S_USB_MODE:
		usb_mode_set((usb_mode() + 3 + dir) % 3);
		set_note_ms("Reboot to apply", 2000);
		break;
	case S_SLEEP: sleep_step(dir); settings_save(); break;
	case S_LOCK:  set_lock = !set_lock; settings_save(); break;
	}
}

static void activate(void)
{
	view *v = &CUR;

	switch (v->v) {
	case V_HOME:
		switch (home[v->sel].kind) {
		case H_NOW:      open_now(); break;
		case H_MUSIC:    push(V_ARTISTS, 0); break;
		case H_RECENT:   push(V_RECENT, 0); break;
		case H_FAVS:     push(V_FAVS, 0); break;
		case H_SYS:      games_load(home[v->sel].sys); push(V_GAMES, home[v->sel].sys); break;
		case H_SETTINGS: push(V_SETTINGS, 0); break;
		}
		break;
	case V_GAMES:
		if (ngames) launch(v->arg, games[v->sel]);
		break;
	case V_RECENT:
		if (nrecent) launch(recent[v->sel].sys, recent[v->sel].file);
		break;
	case V_FAVS:
		if (nfavs) launch(favs[v->sel].sys, favs[v->sel].file);
		break;
	case V_ARTISTS:
		if (lib_ready && lib.nartists) push(V_ALBUMS, v->sel);
		break;
	case V_ALBUMS:
		if (lib_ready) push(V_TRACKS, lib.artists[v->arg].first + v->sel);
		break;
	case V_TRACKS:
		if (lib_ready && lib.albums[v->arg].n) { play_album(v->arg, v->sel); open_now(); }
		break;
	case V_NOW:
		musec_toggle();
		break;
	case V_SETTINGS:
		switch (srow[v->sel]) {
		case S_USB_DRIVE: usb_drive(); break;
		case S_KEYS:      push(V_CONTROLS, 0); break;
		case S_USB_MODE:
		case S_SLEEP:
		case S_LOCK:
			setting_step(srow[v->sel], 1);
			break;
		case S_RESTART:
		case S_POWEROFF:
			if (confirm != v->sel) { confirm = v->sel; break; }
			musec_stop();
			music_tick();
			sync();
			if (srow[v->sel] == S_RESTART) system("touch /run/rebooting; sync; reboot");
			else system(POWEROFF);
			want_quit = 1;
			break;
		}
		return;
	}
}

/* ------------------------------------------------------- dim, shutdown */

/* The power key's tap is the MENU key (fkgpiod: q). On the shelf it opens Now
 * Playing dimmed (ggv.28): backlight level 0, the lowest the RG Nano has (its
 * LEDs hang off the always-on 5 V rail, so they never go fully dark), still
 * working - unless the lock is on, when only another tap does anything.
 * Only another tap brings the brightness back (B and X do nothing while
 * dimmed: Now Playing is where it stays). The key's hold stays FunKey's
 * clean power off.
 * Inactive shutdown: no button and no music for the set minutes, anywhere
 * outside a game, powers off with the queue kept. */
#define BACKLIGHT "/sys/class/backlight/backlight/brightness"
static bool     dimmed;
static unsigned dim_at;
static char     backlight[16];

#ifndef TRIMUI
static void dim(void)
{
	int fd = open(BACKLIGHT, O_RDONLY);
	ssize_t n = fd >= 0 ? read(fd, backlight, sizeof backlight - 1) : -1;

	if (fd >= 0) close(fd);
	backlight[n > 0 ? n : 0] = '\0';
	if ((fd = open(BACKLIGHT, O_WRONLY)) >= 0) {
		if (write(fd, "0", 1) != 1) { /* stays bright */ }
		close(fd);
	}
	open_now();
	dimmed = true;
	dim_at = plat_now_ms();
}
#endif

static void undim(const char *by)
{
	char now[16];
	int fd = open(BACKLIGHT, O_RDONLY);
	ssize_t n = fd >= 0 ? read(fd, now, sizeof now - 1) : -1;

	if (fd >= 0) close(fd);
	/* FN + X/B while dimmed set a level of its own: keep that one. */
	if (n > 0 && now[0] == '0' && backlight[0] && (fd = open(BACKLIGHT, O_WRONLY)) >= 0) {
		if (write(fd, backlight, strlen(backlight)) < 0) { /* stays dim */ }
		close(fd);
	}
	dimmed = false;
	fprintf(stderr, "nanoshelf: dimmed %u s, back by %s\n", (plat_now_ms() - dim_at) / 1000, by);
}

/* A button while dimmed: true when it is done with. */
static bool dimmed_key(SDLKey k)
{
	if (k == SDLK_q) { undim("power"); return true; }
	return set_lock || k == SDLK_x || k == SDLK_b;
}

static void shutdown_tick(void)
{
	unsigned now = plat_now_ms();

	if (musec_now()->state == MU_PLAYING) active_at = now;
	if (!set_sleep || now - active_at < (unsigned)set_sleep * 60000u) return;
	fprintf(stderr, "nanoshelf: inactive %d min: power off\n", set_sleep);
	queue_save();
	sync();
	system(POWEROFF);
	want_quit = 1;
}

static void key(SDLKey k)
{
	view *v = &CUR;
	int n = view_len(v);

	if (k != SDLK_a) confirm = -1;

	switch (k) {
	case SDLK_u: if (n) v->sel = (v->sel + n - 1) % n; break;
	case SDLK_d: if (n) v->sel = (v->sel + 1) % n; break;
	case SDLK_l:
		if (v->v == V_NOW) musec_seek_by(-10);
		else if (v->v == V_CONTROLS) v->sel = (v->sel + NCONTROLS - 1) % NCONTROLS;
		else if (v->v == V_SETTINGS) setting_step(srow[v->sel], -1);
		else if (n) v->sel = v->sel > ROWS ? v->sel - ROWS : 0;
		break;
	case SDLK_r:
		if (v->v == V_NOW) musec_seek_by(10);
		else if (v->v == V_CONTROLS) v->sel = (v->sel + 1) % NCONTROLS;
		else if (v->v == V_SETTINGS) setting_step(srow[v->sel], 1);
		else if (n) v->sel = v->sel + ROWS < n ? v->sel + ROWS : n - 1;
		break;
	case SDLK_a: activate(); break;
	case SDLK_b: if (depth > 0) depth--; break;
	case SDLK_x:
		if (v->v == V_NOW) {
			push(V_QUEUE, 0);
			CUR.sel = musec_now()->index;     /* open on the track playing */
		} else {
			open_now();
		}
		break;
	case SDLK_y:
		if (v->v == V_NOW) musec_set_mode((muq_mode)((musec_mode() + 1) % MUQ_MODES));
		else if (n && (v->v == V_GAMES || v->v == V_RECENT || v->v == V_FAVS)) {
			int s = v->v == V_GAMES ? v->arg : v->v == V_RECENT ? recent[v->sel].sys : favs[v->sel].sys;
			const char *file = v->v == V_GAMES ? games[v->sel] : v->v == V_RECENT ? recent[v->sel].file
			                                                                     : favs[v->sel].file;

			set_note_ms(fav_toggle(s, file) ? "Added to Favorites" : "Removed from Favorites", 2000);
		}
		break;
	case SDLK_s: musec_toggle(); break;          /* START, everywhere */
#ifdef TRIMUI
	case SDLK_q: open_now(); break;              /* MENU: no backlight file to dim */
#else
	case SDLK_q: dim(); break;                   /* the power key's tap */
#endif
	case SDLK_m: if (v->v == V_NOW) musec_prev(); break;   /* L */
	case SDLK_n:                                           /* R */
		if (v->v == V_NOW) musec_next();
		else if (v->v == V_GAMES && is_pico8(v->arg) && pico8_native_ready()) {
			set_pico8 = !set_pico8;
			settings_save();
		}
		break;
	default: break;
	}
	publish();
}

/* The keys, as the code above names them: FunKey's fkgpiod sends the Nano's
 * buttons as letters, and the Model S's gpio-keys send PicoArch's trimui
 * keys (plat_trimui.c), which are turned into those letters here. */
static SDLKey key_from(SDLKey k)
{
#ifdef TRIMUI
	switch (k) {
	case SDLK_UP:        return SDLK_u;
	case SDLK_DOWN:      return SDLK_d;
	case SDLK_LEFT:      return SDLK_l;
	case SDLK_RIGHT:     return SDLK_r;
	case SDLK_SPACE:     return SDLK_a;
	case SDLK_LCTRL:     return SDLK_b;
	case SDLK_LSHIFT:    return SDLK_x;
	case SDLK_LALT:      return SDLK_y;
	case SDLK_RETURN:    return SDLK_s;
	case SDLK_TAB:       return SDLK_m;      /* L */
	case SDLK_BACKSPACE: return SDLK_n;      /* R */
	case SDLK_ESCAPE:    return SDLK_q;      /* MENU */
	default:             return SDLK_UNKNOWN;
	}
#else
	return k;
#endif
}

/* -------------------------------------------------------------------- main */

int main(void)
{
	unsigned sig = 0, last_tick = 0, up_start;
	int s;

	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	signal(SIGUSR1, SIG_IGN);    /* the power key: powerdown does the rest */
	signal(SIGPIPE, SIG_IGN);
	setenv("HOME", GAME_HOME, 1);
	app_paths();
	mkdir(DATA, 0777);
	/* Muse's speaker by name. FunKey's USB-audio watcher points ALSA's
	 * "default" at a DAC while one is in (its ~/.asoundrc, which is also
	 * what moves the volume keys to the DAC), so "default" is not always
	 * the speaker; monocard (/etc/asound.conf) is. A DAC is chosen with
	 * SINK instead (dac_poll). */
#ifndef TRIMUI
	setenv("MUSE_CODEC", "monocard", 1);
#else
	cpu_clock(CPU_SHELF);
#endif
	up_start = uptime_ms();
	pid_record(getpid());

	if (SDL_Init(0) != 0 || TTF_Init() != 0 || !video_start()) {
		fprintf(stderr, "nanoshelf: %s\n", SDL_GetError());
		return 1;
	}
	font = TTF_OpenFont(font_path, FONT_SZ);
	font_big = TTF_OpenFont(font_path, FONT_BIG);
	if (!font || !font_big) {
		fprintf(stderr, "nanoshelf: %s: %s\n", font_path, TTF_GetError());
		return 1;
	}
	for (s = 0; s < NSYS; s++) sys_shown[s] = sys_has_games(&SYS[s]);
	recent_load();
	favs_load();
	settings_load();
	settings_rows();
	battery_poll();
	active_at = plat_now_ms();
	dsp_point(-1);
	music_init();
	resume_instant_play(up_start);
	stack[0] = (view){ V_HOME, 0, 0, 0 };
	draw();

	while (!want_quit) {
		SDL_Event e;
		unsigned t_in = 0, now;
		bool dirty = false;

		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_KEYDOWN) {
				if (!t_in) t_in = plat_now_ms();
				active_at = t_in;
				SDLKey k = key_from(e.key.keysym.sym);

				if (!dimmed || !dimmed_key(k)) key(k);
				dirty = true;
			} else if (e.type == SDL_QUIT) {
				want_quit = 1;
			}
		}
		now = plat_now_ms();
		if (now - last_tick >= 100) {
			unsigned s2;

			last_tick = now;
			music_tick();
			battery_poll();
			shutdown_tick();
			s2 = music_sig();
			if (s2 != sig) { sig = s2; dirty = true; }
		}
		if (dirty && screen) {
			draw();
			if (t_in && !launched) {
				unsigned d = plat_now_ms() - t_in;

				lat_n++;
				lat_sum += d;
				if (d > lat_max) lat_max = d;
			}
		}
		launched = false;
		SDL_Delay(15);
	}
	lat_report("at exit");
	TTF_Quit();
	SDL_Quit();
	return 0;
}
