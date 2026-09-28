/* What does a menu contain, for a given state?
 *
 * This check is the point of ADR-0001. A screen's build function takes state
 * and produces rows, touching no renderer and no device, so the answer can be
 * had here rather than by looking at a handheld. Every menu defect this
 * project has had was some form of "nobody noticed the list was wrong", and
 * every one of them needed a device to see.
 *
 * If this file ever needs SDL to link, the decision has failed. Do not add it -
 * reopen ADR-0001 instead.
 */
#include "../src/wifi_menu.h"
#include "../src/sys_menu.h"
#include "../src/cards.h"
#include "../src/game_menu.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

static int fails;

static void ck(int cond, const char *what)
{
	if (!cond) { printf("  FAIL %s\n", what); fails++; }
}

static int live_count(const menu_row *r, int n)
{
	int i, k = 0;
	for (i = 0; i < n; i++) if (r[i].live) k++;
	return k;
}

static int rule_at(const menu_row *r, int n)
{
	int i;
	for (i = 0; i < n; i++) if (!r[i].label) return i;
	return -1;
}

/* Every screen must be able to say "nothing here" without offering a row that
 * does nothing when pressed. The radio-off case is the one that used to draw a
 * selectable placeholder. */
static void off_state(void)
{
	wifi_ui w;
	menu_row rows[64];
	const char *heading;
	int n;

	memset(&w, 0, sizeof w);
	w.on = false;
	n = wifi_build(&w, rows, 64, &heading);

	printf("radio off:\n");
	ck(heading && !strcmp(heading, "Wi-Fi"), "the screen names itself");
	ck(n == 3, "switch, rule, note");
	ck(rows[0].live, "the switch is live");
	ck(!strcmp(rows[0].label, "Wi-Fi"), "row 0 is the switch");
	ck(!strcmp(rows[0].value, "off"), "the switch reads off");
	ck(rule_at(rows, n) == 1, "the rule separates the footer");
	ck(ROW_IS_NOTE(rows[2]), "the footer is a note");
	ck(!rows[2].live, "the footer is not selectable");
	ck(live_count(rows, n) == 1, "only the switch can be chosen");
}

/* The state the async scan produces on entry: saved networks up immediately,
 * the footer saying what is happening, nothing claiming a signal it has not
 * measured. */
static void scanning_state(void)
{
	wifi_ui w;
	menu_row rows[64];
	const char *heading;
	int n;

	memset(&w, 0, sizeof w);
	w.on = true;
	w.scanning = true;
	w.scanned = false;
	w.n = 2;
	snprintf(w.nets[0].ssid, sizeof w.nets[0].ssid, "ReinFi");
	w.nets[0].known = true;
	snprintf(w.nets[1].ssid, sizeof w.nets[1].ssid, "Transport");
	w.nets[1].known = true;
	wifi_label(&w);
	n = wifi_build(&w, rows, 64, &heading);

	printf("scanning, two saved networks:\n");
	ck(n == 5, "switch, two networks, rule, note");
	ck(live_count(rows, n) == 3, "switch and both networks are choosable");
	ck(!strcmp(rows[1].label, "ReinFi"), "the first saved network is listed");
	ck(!strcmp(rows[1].value, "saved"), "no signal word before the scan lands");
	ck(rule_at(rows, n) == 3, "rule after the networks");
	ck(!strcmp(rows[4].label, "Scanning..."), "footer says what is happening");
	ck(ROW_IS_NOTE(rows[4]), "the progress line is a note");
}

/* After results land the footer becomes the key legend, and the legend only
 * offers forget when there is something to forget. */
static void scanned_state(void)
{
	wifi_ui w;
	menu_row rows[64];
	const char *heading;
	int n;

	memset(&w, 0, sizeof w);
	w.on = true;
	w.scanned = true;
	w.n = 1;
	snprintf(w.nets[0].ssid, sizeof w.nets[0].ssid, "Cafe");
	w.nets[0].known = false;
	w.nets[0].secured = true;
	w.nets[0].signal = -55;
	wifi_label(&w);
	n = wifi_build(&w, rows, 64, &heading);

	printf("scan done, one unsaved network:\n");
	ck(n == 4, "switch, network, rule, note");
	ck(!strcmp(rows[1].value, "strong"), "-55 dBm reads as strong");
	ck(!strcmp(rows[3].label, "Y: rescan"), "no forget offered with nothing saved");

	w.nets[0].known = true;
	wifi_label(&w);
	n = wifi_build(&w, rows, 64, &heading);
	ck(!strcmp(rows[3].label, "Y: rescan   X: forget"), "forget offered once saved");
	ck(!strcmp(rows[1].value, "strong - saved"), "a saved network in range shows both");
}

/* The bands are a judgment on a measured number and the boundary is not
 * validated - see BACKLOG. Pinned here so it cannot move by accident. */
static void strength_words(void)
{
	printf("signal words:\n");
	ck(!strcmp(wifi_strength(-40), "strong"), "-40 strong");
	ck(!strcmp(wifi_strength(-60), "strong"), "-60 is the strong boundary");
	ck(!strcmp(wifi_strength(-61), "good"),   "-61 good");
	ck(!strcmp(wifi_strength(-72), "good"),   "-72 is the good boundary");
	ck(!strcmp(wifi_strength(-73), "weak"),   "-73 weak, the measured cliff");
}

/* A build must never write past what it was given. */
static void respects_max(void)
{
	wifi_ui w;
	menu_row rows[4];
	const char *heading;
	int n, i;

	memset(&w, 0, sizeof w);
	w.on = true;
	w.scanned = true;
	w.n = WIFI_MAX_NETS;
	for (i = 0; i < WIFI_MAX_NETS; i++)
		snprintf(w.nets[i].ssid, sizeof w.nets[i].ssid, "net%d", i);
	wifi_label(&w);
	n = wifi_build(&w, rows, 4, &heading);

	printf("a small buffer:\n");
	ck(n <= 4, "never returns more rows than it was offered");
}


/* ---------- the two MENU-button menus ------------------------------------- */

static const char *val(const menu_row *r) { return r->value ? r->value : ""; }

/* A device with no network. Three rows depend on one, and all three have to
 * say so rather than silently doing nothing: Over The Hare, Box Art, and Box
 * Art's counterpart in the system menu. */
static void tortos_menu_offline(void)
{
	sys_ui u;
	menu_bufs b;
	menu_row rows[MENU_MAX_ROWS];
	const char *heading;
	int n;

	memset(&u, 0, sizeof u);
	u.wifi = WIFI_OFF;
	u.cards = "Plain Jane";
	u.cards_dir = "Horizontal";
	u.auto_off = 120;
	u.suspend_timeout = 90;
	n = sys_menu_build(&u, rows, &b, &heading);

	printf("TortOS menu, radio off:\n");
	ck(n == PM_ROWS, "every row is filled");
	ck(!strcmp(heading, "TortOS"), "heading");
	ck(!strcmp(val(&rows[PM_WIFI]), "off"), "Wi-Fi reads off");
	ck(!strcmp(val(&rows[PM_XFER]), "needs Wi-Fi"), "OTH says why it is dead");
	ck(!rows[PM_XFER].live, "OTH is not selectable offline");
	ck(!strcmp(val(&rows[PM_SCRAPE]), "needs Wi-Fi"), "Box Art says why");
	ck(!rows[PM_SCRAPE].live, "Box Art is not selectable offline");
	ck(!strcmp(val(&rows[PM_ACHIEVEMENTS]), "sign in"), "Cheevos invites a sign in");
	ck(rows[PM_ACHIEVEMENTS].live, "Cheevos is reachable signed out");
	ck(!strcmp(val(&rows[PM_SLEEP]), "2m"), "120s reads as 2m");
	ck(!strcmp(val(&rows[PM_SUSPEND]), "90s"), "90s stays in seconds, as NextUI spells it");
	ck(rows[PM_SUSPEND].live, "Suspend Timeout is reachable offline");
	ck(!strcmp(val(&rows[PM_KEEPAWAKE]), "off"), "Keep Awake Over USB defaults off");
	ck(!strcmp(val(&rows[PM_MUTESW]), "mute"), "Mute Switch defaults to mute");
	ck(!strcmp(val(&rows[PM_THEME]), "Plain Jane"), "the card set names itself");
	ck(!strcmp(val(&rows[PM_DIR]), "Horizontal"), "and so does the direction");
	ck(rows[PM_DIR].live, "UI Direction is reachable offline too");
	ck(rows[PM_THEME].live, "UI Theme is reachable offline, being a look and not a service");
	/* Live since 2026-09-06, when the pairing screen landed. The row used to
	 * be dead and read "not yet", which was true of the screen and false of
	 * the feature - game audio had been going to a headset for three days. */
	ck(rows[PM_BT].live, "Bluetooth is reachable now that pairing exists");
	/* Always reachable, even with no radio and no cable: it is the row you go
	 * to in order to say "not Bluetooth", so it must not vanish with the thing
	 * it refuses. */
	ck(rows[PM_AUDIO].live, "Audio Output is reachable offline");
	ck(!strcmp(val(&rows[PM_AUDIO]), "auto (speaker)"),
	   "and Auto names where it landed rather than only saying Auto");
}

/* The row has to say a PLACE. "Auto" alone makes the player guess which of
 * three they are about to hear. */
static void audio_row(void)
{
	sys_ui u;
	menu_bufs b;
	menu_row rows[MENU_MAX_ROWS];
	const char *heading;

	memset(&u, 0, sizeof u);
	printf("the Audio Output row:\n");

	u.audio_policy = AOUT_AUTO;
	u.audio_dest = AOUT_WIRED;
	sys_menu_build(&u, rows, &b, &heading);
	ck(!strcmp(val(&rows[PM_AUDIO]), "auto (wired)"), "auto, on a cable");

	u.audio_dest = AOUT_BT;
	sys_menu_build(&u, rows, &b, &heading);
	ck(!strcmp(val(&rows[PM_AUDIO]), "auto (bluetooth)"), "auto, on a headset");

	/* Pinned says the place with no "auto", because there is no rule left to
	 * describe - it is just where the sound is. */
	u.audio_policy = AOUT_SPEAKER;
	u.audio_dest = AOUT_SPK;
	sys_menu_build(&u, rows, &b, &heading);
	ck(!strcmp(val(&rows[PM_AUDIO]), "speaker"), "pinned reads as the place");

	/* Pinned to Speaker with a cable in is still the cable: the pin refuses
	 * Bluetooth, never the jack. */
	u.audio_dest = AOUT_WIRED;
	sys_menu_build(&u, rows, &b, &heading);
	ck(!strcmp(val(&rows[PM_AUDIO]), "wired"),
	   "and a cable still shows through the Speaker pin");
}

/* Connected and signed in. The Wi-Fi row shows the network's NAME - a settings
 * row says what the setting is, and the address lives on the About page. */
static void tortos_menu_online(void)
{
	sys_ui u;
	menu_bufs b;
	menu_row rows[MENU_MAX_ROWS];
	const char *heading;
	int n;

	memset(&u, 0, sizeof u);
	u.wifi = WIFI_CONNECTED;
	u.ssid = "kitchen";
	u.ra_in = true;
	u.ra_name = "eric";
	u.auto_off = 0;
	n = sys_menu_build(&u, rows, &b, &heading);

	ck(n == PM_ROWS, "row count does not depend on the network");
	printf("TortOS menu, connected:\n");
	ck(!strcmp(val(&rows[PM_WIFI]), "kitchen"), "Wi-Fi shows the network name");
	ck(rows[PM_XFER].live && !rows[PM_XFER].value, "OTH is live and unqualified");
	ck(rows[PM_SCRAPE].live, "Box Art is live");
	ck(!strcmp(val(&rows[PM_ACHIEVEMENTS]), "eric"), "Cheevos shows the account");
	ck(!strcmp(val(&rows[PM_SLEEP]), "never"), "0s reads as never");
	ck(!strcmp(val(&rows[PM_AUTO_OFF]), "never"), "Auto Off unset reads as never");
	u.keep_awake_usb = true;
	sys_menu_build(&u, rows, &b, &heading);
	ck(!strcmp(val(&rows[PM_KEEPAWAKE]), "on"), "Keep Awake Over USB reads on");
	u.mute_lock = true;
	sys_menu_build(&u, rows, &b, &heading);
	ck(!strcmp(val(&rows[PM_MUTESW]), "muse button lock"), "Mute Switch reads muse button lock");

	/* THE SCREENSCRAPER ROW HAS THREE STATES, one more than the Cheevos row
	 * beside it: the developer key comes from the environment at build time,
	 * so a build can exist that cannot sign anyone in at all. That state is
	 * the one worth pinning - a row that opens a keyboard and then refuses
	 * whatever is typed would look like a rejected password. */
	printf("the ScreenScraper account row:\n");
	ck(!u.ss_have, "the fixture above has no key");
	ck(!strcmp(val(&rows[PM_SS]), "not in this build"), "and the row says so");
	ck(!rows[PM_SS].live, "and does not open");

	u.ss_have = true;
	n = sys_menu_build(&u, rows, &b, &heading);
	ck(!strcmp(val(&rows[PM_SS]), "sign in"), "a key with no account invites one");
	ck(rows[PM_SS].live, "and opens");

	u.ss_in = true;
	u.ss_name = "someone";
	n = sys_menu_build(&u, rows, &b, &heading);
	ck(!strcmp(val(&rows[PM_SS]), "someone"), "an account shows its name");
	ck(rows[PM_SS].live, "and stays open, to change it");
	ck(n == PM_ROWS, "and none of that changes the row count");

	/* Controls needs nothing of the device - no network, no account, no
	 * card - so it is live in every state this menu can be in, and it sits
	 * with About at the end where the rows you only read live. */
	printf("the Controls row:\n");
	ck(!strcmp(rows[PM_CONTROLS].label, "Controls"), "it is there");
	ck(rows[PM_CONTROLS].live, "and live with the radio down and nobody signed in");
	ck(!rows[PM_CONTROLS].value, "and says nothing in the value column");
	ck(PM_CONTROLS == PM_ABOUT - 1, "and sits just above About TortOS");
}

/* Connecting and idle are not the same as off, and the row must not flatten
 * them: "not connected" and "connecting" answer different questions. */
static void wifi_row_wording(void)
{
	sys_ui u;
	menu_bufs b;
	menu_row rows[MENU_MAX_ROWS];
	const char *heading;

	memset(&u, 0, sizeof u);
	printf("the Wi-Fi row's three off states:\n");

	u.wifi = WIFI_CONNECTING;
	sys_menu_build(&u, rows, &b, &heading);
	ck(!strcmp(val(&rows[PM_WIFI]), "connecting"), "connecting");

	u.wifi = WIFI_IDLE;
	sys_menu_build(&u, rows, &b, &heading);
	ck(!strcmp(val(&rows[PM_WIFI]), "not connected"), "up but unassociated");

	/* Connected with no name yet: better to say nothing useful than to print
	 * an empty value where a network name belongs. */
	u.wifi = WIFI_CONNECTED;
	u.ssid = "";
	sys_menu_build(&u, rows, &b, &heading);
	ck(val(&rows[PM_WIFI])[0] != 0, "connected with no SSID still says something");
}

/* The system menu. Games and Core carry real values, so a wrong count here is
 * a row lying about the machine it describes. */
static void system_menu(void)
{
	sys_ui u;
	menu_bufs b;
	menu_row rows[MENU_MAX_ROWS];
	const char *heading;
	int n;

	memset(&u, 0, sizeof u);
	u.games = true;
	u.wifi = WIFI_CONNECTED;
	u.sys_name = "Game Boy";
	u.sys_core = "gambatte";
	u.game_count = 412;
	u.dmode = "Sharp";
	n = sys_menu_build(&u, rows, &b, &heading);

	printf("system menu:\n");
	ck(n == SM_ROWS, "every row is filled");
	ck(!strcmp(heading, "Game Boy"), "heading is the system, not TortOS");
	ck(!strcmp(val(&rows[SM_GAMES]), "412"), "the count is the real one");
	ck(!strcmp(val(&rows[SM_CORE]), "gambatte"), "the core is the real one");
	ck(!strcmp(val(&rows[SM_DISPLAY]), "Sharp"), "display mode is passed through");
	ck(rows[SM_DISPLAY].live && rows[SM_RESCAN].live, "the two live rows are live");
	ck(!rows[SM_GAMES].live && !rows[SM_CORE].live, "reported facts are not rows to press");
	ck(rows[SM_BOXART].live, "Box Art is live on a network");

	/* The same menu with the radio down. Only Box Art changes. */
	u.wifi = WIFI_OFF;
	sys_menu_build(&u, rows, &b, &heading);
	ck(!rows[SM_BOXART].live, "Box Art dies with the radio");
	ck(!strcmp(val(&rows[SM_BOXART]), "needs Wi-Fi"), "and says why");
	ck(rows[SM_RESCAN].live, "Rescan does not need a network");

	/* Favorites, which is not a system: no core and no folder, so four of the
	 * six rows have nothing to describe and one of them - Display Mode -
	 * would give a game a different aspect depending on the shelf it was
	 * launched from. */
	{
		sys_ui f = u;
		menu_row frows[MENU_MAX_ROWS];
		menu_bufs fb;
		const char *fhead = NULL;
		int fn, k;

		f.fav = true;   /* by what it has: no core, no folder */
		f.sys_name = "Favorites";
		f.sys_core = "";
		fn = sys_menu_build(&f, frows, &fb, &fhead);
		printf("the Favorites shelf's menu:\n");
		ck(fn == SM_FAV_ROWS, "two rows and a count, not six");
		ck(!strcmp(frows[0].label, "Games"), "how many are on it");
		ck(!strcmp(frows[1].label, "Sort By"), "and what order they are in");
		for (k = 0; k < fn; k++) {
			ck(strcmp(frows[k].label, "Core") != 0, "no Core: it has none");
			ck(strcmp(frows[k].label, "Display Mode") != 0,
			   "no Display Mode: the owner's system decides that");
			ck(strcmp(frows[k].label, "Box Art") != 0, "no Box Art: no folder");
			ck(strcmp(frows[k].label, "Rescan Folder") != 0, "and nothing to rescan");
		}
	}

	/* Muse, which is not a system either: no core, no ROM folder, no display
	 * mode. What it does have is a folder of music, two orders to see it in,
	 * and Rescan Folder for music copied in. */
	{
		sys_ui m = u;
		menu_row mrows[MENU_MAX_ROWS];
		menu_bufs mb;
		const char *mhead = NULL;
		int mn, k;

		m.muse = true;
		m.sys_name = "Muse";
		m.sys_core = "";
		m.game_count = 7;
		m.sort = NULL;
		m.wifi = WIFI_CONNECTED;     /* the case above left the radio down */
		mn = sys_menu_build(&m, mrows, &mb, &mhead);
		printf("Muse's shelf menu:\n");
		ck(mn == SM_MUSE_ROWS, "five rows, not six");
		ck(!strcmp(mrows[0].label, "Albums") && !strcmp(val(&mrows[0]), "7"),
		   "how many albums");
		ck(!strcmp(mrows[1].label, "Sort By") && mrows[1].live &&
		   !strcmp(val(&mrows[1]), "Artist"),
		   "Sort By, and by artist when nothing says otherwise");
		ck(!strcmp(mrows[2].label, "Album Art") && mrows[2].live,
		   "covers to fetch, on a network");
		ck(!strcmp(mrows[3].label, "Rescan Folder") && mrows[3].live,
		   "and a rescan that works");
		ck(!strcmp(mrows[4].label, "Muse Settings") && mrows[4].live,
		   "and Muse's own settings");
		m.sort = "Album";
		m.wifi = WIFI_OFF;
		sys_menu_build(&m, mrows, &mb, &mhead);
		ck(!strcmp(val(&mrows[1]), "Album"), "the order it is in");
		ck(!mrows[2].live && !strcmp(val(&mrows[2]), "needs Wi-Fi"),
		   "Album Art says why it cannot, off the network");
		ck(mrows[1].live && mrows[3].live,
		   "and neither Sort By nor Rescan Folder needs one");
		for (k = 0; k < mn; k++) {
			ck(strcmp(mrows[k].label, "Core") != 0, "no Core: it has none");
			ck(strcmp(mrows[k].label, "Display Mode") != 0, "no Display Mode");
		}
	}
}

/* The labels, on their own - NextUI's spelling (settings.cpp's
 * screen_timeout_labels/sleep_timeout_labels): seconds through 90s, whole
 * minutes from 2m. A minute and a half reads "90s" there, so it does here. */
static void auto_off_words(void)
{
	char s[16];
	struct { int sec; const char *want; } t[] = {
		{ 0, "never" }, { 5, "5s" }, { 30, "30s" }, { 60, "60s" },
		{ 90, "90s" }, { 120, "2m" }, { 300, "5m" }, { 600, "10m" },
	};
	size_t i;

	printf("auto off labels:\n");
	for (i = 0; i < sizeof t / sizeof t[0]; i++) {
		sys_menu_auto_off_label(t[i].sec, s, sizeof s);
		ck(!strcmp(s, t[i].want), t[i].want);
	}
}


/* ---------- where the cursor can go --------------------------------------- */

/* Walk the whole menu and collect every row the cursor can actually rest on.
 * A row that is drawn but cannot be selected is fine; a row that CAN be
 * selected and then does nothing when pressed is the defect. */
static int reachable(const menu_row *rows, int n, int *out, int max)
{
	int sel = 0, k = 0, i;

	if (!rows[0].live) sel = menu_step_sel(rows, n, 0, +1);
	if (!rows[sel].live) return 0;      /* nothing live at all */
	for (i = 0; i < n && k < max; i++) {
		out[k++] = sel;
		sel = menu_step_sel(rows, n, sel, +1);
		if (sel == out[0]) break;       /* wrapped */
	}
	return k;
}

static bool holds(const int *v, int n, int want)
{
	int i;
	for (i = 0; i < n; i++) if (v[i] == want) return true;
	return false;
}

/* The migration of the system menu onto the runner changed this on purpose:
 * the cursor used to walk every row, including the four that only report a
 * fact. Now it visits what can be pressed. */
static void cursor_reaches(void)
{
	sys_ui u;
	menu_bufs b;
	menu_row rows[MENU_MAX_ROWS];
	const char *heading;
	int got[MENU_MAX_ROWS], n, k;

	memset(&u, 0, sizeof u);
	u.games = true;
	u.wifi = WIFI_CONNECTED;
	u.sys_name = "NES"; u.sys_core = "nestopia";
	u.game_count = 3; u.dmode = "Native";
	n = sys_menu_build(&u, rows, &b, &heading);
	k = reachable(rows, n, got, MENU_MAX_ROWS);

	printf("system menu, what the cursor can reach:\n");
	ck(k == 4, "four rows, not seven");
	ck(holds(got, k, SM_DISPLAY), "Display Mode");
	/* Live since sorting stopped being alphabetical-only. It was a
	 * placeholder for long enough that this check counted three. */
	ck(holds(got, k, SM_SORT), "Sort By");
	ck(holds(got, k, SM_BOXART), "Box Art");
	ck(holds(got, k, SM_RESCAN), "Rescan Folder");
	ck(!holds(got, k, SM_GAMES) && !holds(got, k, SM_CORE),
	   "the reported facts are not stops");

	memset(&u, 0, sizeof u);
	u.wifi = WIFI_OFF;
	n = sys_menu_build(&u, rows, &b, &heading);
	k = reachable(rows, n, got, MENU_MAX_ROWS);

	printf("TortOS menu offline, what the cursor can reach:\n");
	/* No network needed to pair a headset, so it stays reachable offline -
	 * unlike the three rows above, which do need one. */
	ck(holds(got, k, PM_BT), "Bluetooth is reachable with no network");
	ck(!holds(got, k, PM_XFER), "OTH is skipped with no network");
	ck(!holds(got, k, PM_SCRAPE), "Box Art is skipped with no network");
	ck(holds(got, k, PM_WIFI), "Wi-Fi is reachable, which is how you fix that");
	ck(holds(got, k, PM_ABOUT), "About is reachable");
	/* Play Time reads what is already stored and asks nothing of the network,
	 * so it stays reachable when everything else is grayed out. */
	ck(holds(got, k, PM_STATS), "Play Time is reachable offline");
}

/* The bound is the point. An all-dead menu must terminate, not spin. */
static void step_terminates(void)
{
	menu_row dead[3];
	int i;

	for (i = 0; i < 3; i++) dead[i] = (menu_row){ "x", NULL, false };
	printf("a menu with nothing live:\n");
	ck(menu_step_sel(dead, 3, 1, +1) == 1, "forward stays put");
	ck(menu_step_sel(dead, 3, 1, -1) == 1, "backward stays put");
	ck(menu_step_sel(dead, 0, 0, +1) == 0, "an empty menu is survivable");
}

/* Walking down a list and back must end up looking at the top of it again.
 *
 * The list here is the game info screen as a scraped game builds it: Cheevos
 * and Year lead, and neither is a stop for the cursor. On the device on
 * 2026-09-16, one walk down to Favorite scrolled both off the top and nothing
 * afterwards brought them back - the window moved only when the CURSOR would
 * leave it, and no cursor can ever be up there.
 *
 * Walked rather than asserted at one position, because every single frame of
 * that walk was correct on its own. */
static void window_returns(void)
{
	menu_row rows[9];
	int vis = 8, first = 0, sel, i;

	for (i = 0; i < 9; i++) rows[i] = (menu_row){ "row", NULL, false };
	rows[2].live = rows[7].live = rows[8].live = true;   /* Synopsis, art, fav */

	printf("a list whose first two rows cannot be selected:\n");
	sel = menu_step_sel(rows, 9, 0, +1);
	ck(sel == 2, "opens on the first row that can be");
	first = menu_window_first(rows, 9, sel, vis, first);
	ck(first == 0, "and shows the rows above it");

	sel = menu_step_sel(rows, 9, sel, +1);
	first = menu_window_first(rows, 9, sel, vis, first);
	sel = menu_step_sel(rows, 9, sel, +1);
	first = menu_window_first(rows, 9, sel, vis, first);
	ck(sel == 8 && first == 1, "walking to the bottom scrolls the top away");

	sel = menu_step_sel(rows, 9, sel, -1);
	first = menu_window_first(rows, 9, sel, vis, first);
	ck(sel == 7 && first == 1, "one step back does not move the window");

	sel = menu_step_sel(rows, 9, sel, -1);
	first = menu_window_first(rows, 9, sel, vis, first);
	ck(sel == 2 && first == 0, "reaching the top row brings the top back");

	/* The cursor still wins when what is above it cannot fit. */
	for (i = 0; i < 9; i++) rows[i].live = false;
	rows[8].live = true;
	ck(menu_window_first(rows, 9, 8, 4, 0) == 5, "a cursor is never pushed off");
	ck(menu_window_first(rows, 9, 8, 9, 0) == 0, "a list that fits never scrolls");
}


/* What scrolls, and what is cut short.
 *
 * The rule reads "the row under the cursor, and rows the cursor can never
 * reach" - and it used to infer the second half from `live`, which is right
 * everywhere except the one list that uses `live` for something else. There
 * `live` means EARNED, so a long achievement title scrolled or was cut
 * depending on whether the player had it. Reported from the device
 * 2026-09-16. */
static void what_scrolls(void)
{
	menu_row live_row = { "a long label", "v", true };
	menu_row dead_row = { "a long label", "v", false };

	printf("which rows scroll:\n");
	ck(menu_row_moves(live_row, true, false), "the row under the cursor does");
	ck(!menu_row_moves(live_row, false, false), "one the cursor left does not");
	ck(menu_row_moves(dead_row, false, false),
	   "a row the cursor cannot reach does, having no other way to be read");

	/* A list whose cursor visits every row: `live` is about color there, so
	 * only the cursor decides. */
	ck(menu_row_moves(live_row, true, true), "with visits_all, the cursor still does");
	ck(menu_row_moves(dead_row, true, true), "and an unearned row under it does too");
	ck(!menu_row_moves(dead_row, false, true),
	   "but an unearned row elsewhere is cut like any other");
	ck(!menu_row_moves(live_row, false, true), "exactly like an earned one");
}


/* ---------- the game info screen ------------------------------------------ */

/* Five rows at most: the three that describe the game, then the two that do
 * something about it. The order reversed on 2026-09-17 - it used to lead with
 * what the cursor could act on - so it is asserted rather than assumed, which
 * is also how the old order got pinned and had to be unpinned here.
 *
 * What is NOT here is as deliberate as what is: no File, no Size, no Box Art
 * byte count, no Favorite, which Y does from the shelf, and no Saves, which
 * the carousel shows with the frames themselves. A row is easy to put back by
 * habit. */
static void info_rows(void)
{
	game_info gi;
	menu_row rows[GI_MAX];
	int got[GI_MAX], n, k, i;

	memset(&gi, 0, sizeof gi);
	snprintf(gi.cheevos, sizeof gi.cheevos, "12 of 78");
	gi.has_cheevos = true;
	gi.has_art = true;

	n = gi_rows(rows, &gi, true);
	printf("game info, nothing scraped, art present and a network:\n");
	ck(n == 2, "Cheevos and the art action, and nothing else to say");
	ck(!strcmp(rows[0].label, "Cheevos"), "Cheevos leads when nothing describes the game");
	ck(!strcmp(val(&rows[0]), "12 of 78"), "and carries the count");
	ck(rows[0].live, "and opens the set, the way the in-game row does");
	ck(!strcmp(rows[1].label, "Replace Box Art"), "art present offers a replace");
	for (i = 0; i < n; i++) {
		ck(strcmp(rows[i].label, "File") != 0, "no File row");
		ck(strcmp(rows[i].label, "Size") != 0, "no Size row");
		ck(strcmp(rows[i].label, "Box Art") != 0, "no Box Art row");
		ck(strcmp(rows[i].label, "Favorite") != 0, "no Favorite row");
		ck(strcmp(rows[i].label, "Saves") != 0, "no Saves row");
	}
	k = reachable(rows, n, got, GI_MAX);
	ck(k == 2, "two stops: the set and the action");

	gi.has_cheevos = false;
	snprintf(gi.cheevos, sizeof gi.cheevos, "none");
	gi.has_art = false;
	n = gi_rows(rows, &gi, false);
	printf("game info, no set, no art and no network:\n");
	ck(!rows[0].live, "a game with no set has nothing to open");
	ck(!strcmp(rows[1].label, "Get Box Art"), "no art offers a get, not a replace");
	ck(!strcmp(val(&rows[1]), "needs Wi-Fi"), "and says why it is dead");
	ck(!rows[1].live, "which it is");
	k = reachable(rows, n, got, GI_MAX);
	ck(k == 0, "nothing on the screen can be chosen");

	/* Scraped, which adds the three rows ABOVE Cheevos and nowhere else. The
	 * shapes below are the whole of what a scrape can leave behind. */
	gi.has_cheevos = true;
	snprintf(gi.cheevos, sizeof gi.cheevos, "12 of 78");
	gi.has_art = true;
	gi.scraped = true;
	gi.has_synopsis = true;
	snprintf(gi.year, sizeof gi.year, "1995");
	snprintf(gi.genre, sizeof gi.genre, "Platform,Shoot'em Up");
	n = gi_rows(rows, &gi, true);
	printf("game info, scraped with a year, a genre and prose:\n");
	ck(n == GI_MAX, "three rows more than an unscraped game");
	ck(!strcmp(rows[0].label, "Synopsis"), "what the game IS leads");
	ck(rows[0].live, "and opens, because the prose does not fit on a row");
	ck(!strcmp(rows[1].label, "Year") && !strcmp(val(&rows[1]), "1995"),
	   "then when it came out");
	ck(!strcmp(rows[2].label, "Genre") && !strcmp(val(&rows[2]), "Platform,Shoot'em Up"),
	   "then what kind, as the scrape joined it");
	ck(!strcmp(rows[3].label, "Cheevos"),
	   "then what there is to shoot for, which is worth seeing before starting");
	ck(!strcmp(rows[4].label, "Replace Box Art"), "and the action last");
	k = reachable(rows, n, got, GI_MAX);
	ck(k == 3, "three stops: the synopsis, the set and the action");

	gi.has_synopsis = false;
	n = gi_rows(rows, &gi, true);
	printf("game info, scraped with no prose:\n");
	ck(!strcmp(val(&rows[0]), "none"), "Synopsis says none");
	ck(!rows[0].live, "and does nothing, the way the in-game Cheevos row does");

	gi.year[0] = '\0';
	gi.has_synopsis = true;
	n = gi_rows(rows, &gi, true);
	printf("game info, scraped with no year:\n");
	ck(n == GI_MAX - 1, "the Year row is simply absent");
	ck(!strcmp(rows[0].label, "Synopsis"), "and Synopsis still leads");
	ck(!strcmp(rows[1].label, "Genre"), "with the genre where the year was");

	gi.genre[0] = '\0';
	n = gi_rows(rows, &gi, true);
	printf("game info, scraped with neither year nor genre:\n");
	ck(n == GI_MAX - 2, "both are simply absent");
	ck(!strcmp(rows[1].label, "Cheevos"), "leaving the prose and the two actions");
}


/* ---------- the in-game menu ---------------------------------------------- */

/* Cheevos and Sleep are the only rows that can be dead: Cheevos exactly when
 * the game has no set, Sleep exactly when the kernel does not offer suspend.
 * Everything else is always something A does. */
static void ingame_rows(void)
{
	gm_ui u;
	gm_bufs b;
	menu_row rows[GM_ROWS];
	int got[GM_ROWS], n, k;

	u.dmode = "Native"; u.earned = 12; u.total = 40; u.sleep_supported = true;
	n = gm_rows(&u, rows, &b);

	printf("in-game menu, a game with a set, sleep supported:\n");
	ck(n == GM_ROWS, "nine rows");
	ck(!strcmp(rows[GM_CONTINUE].label, "Continue"), "Continue leads");
	ck(!strcmp(val(&rows[GM_DISPLAY]), "Native"), "Display carries the mode");
	ck(!strcmp(val(&rows[GM_CHEEVOS]), "12 / 40"), "Cheevos counts the set");
	ck(rows[GM_CHEEVOS].live, "and is reachable");
	ck(rows[GM_SLEEP].live, "Sleep is reachable");
	ck(!strcmp(rows[GM_HOTKEYS].label, "Hotkeys"), "Hotkeys is there");
	ck(rows[GM_HOTKEYS].live, "and is reachable");
	k = reachable(rows, n, got, GM_ROWS);
	ck(k == GM_ROWS, "every row is a stop");

	u.earned = 0; u.total = 0;
	n = gm_rows(&u, rows, &b);
	printf("in-game menu, a game with no set:\n");
	ck(!strcmp(val(&rows[GM_CHEEVOS]), "none"), "Cheevos says none");
	ck(!rows[GM_CHEEVOS].live, "and does nothing");
	k = reachable(rows, n, got, GM_ROWS);
	ck(k == GM_ROWS - 1, "so the cursor steps over it");
	ck(!holds(got, k, GM_CHEEVOS), "and never rests on it");
	ck(holds(got, k, GM_QUIT) && holds(got, k, GM_CONTINUE),
	   "the rows either side of it still work");

	u.earned = 12; u.total = 40; u.sleep_supported = false;
	n = gm_rows(&u, rows, &b);
	printf("in-game menu, sleep unsupported:\n");
	ck(!strcmp(val(&rows[GM_SLEEP]), "unsupported"), "Sleep says why");
	ck(!rows[GM_SLEEP].live, "and does nothing");
	k = reachable(rows, n, got, GM_ROWS);
	ck(k == GM_ROWS - 1, "so the cursor steps over it too");
	ck(!holds(got, k, GM_SLEEP), "and never rests on it");
}

/* The save slots, and the one constant that survived a rename by being written
 * out as a number.
 *
 * The carousel's Auto entry mapped to a hardcoded 9 - MinUI's number for the
 * resume slot. When it was renamed to "auto" on 2026-08-27 every use of the
 * CONSTANT was updated and the literal was missed, so Load -> Auto asked for a
 * `.9.state` that could not exist. Ten days later that turned into a wedged
 * device, because the rejection it caused was misread as a dead emulator.
 *
 * Cheap to assert, and it is the assertion that was missing. */
static void slots(void)
{
	int i;

	printf("save slots:\n");
	ck(gm_slot_at(0) == SLOT_AUTO, "the carousel's first entry is the resume slot");
	ck(gm_slot_at(0) != 9, "and is not MinUI's number for it");
	for (i = 1; i <= GM_SLOTS; i++)
		ck(gm_slot_at(i) == i, "a numbered slot maps to itself");

	/* The resume slot must not collide with one the player can pick, or a
	 * save to the last slot would overwrite the state the exit funnel owns. */
	ck(SLOT_AUTO > GM_SLOTS, "the resume slot is outside the numbered range");
}

/* Where Save lands. A double-tap of A used to overwrite slot 1 every time,
 * which is the slot most likely to hold something wanted. */
static void save_slot(void)
{
	bool have[GM_SLOTS + 1] = { false };
	bool ok;
	int  i;

	printf("which slot Save opens on:\n");
	ck(gm_save_slot(have) == 1, "nothing saved yet: the first slot");

	have[1] = true;
	ck(gm_save_slot(have) == 2, "slot 1 taken: the next empty one");

	have[2] = have[3] = true;
	ck(gm_save_slot(have) == 4, "three taken: still the first empty");

	/* A gap in the middle is where a new save belongs - it is empty, and
	 * nothing is lost by using it. */
	for (i = 1; i <= GM_SLOTS; i++) have[i] = true;
	have[3] = false;
	ck(gm_save_slot(have) == 3, "a freed middle slot is used before any reuse");
	have[3] = true;

	/* An empty last slot is reached the ordinary way, not as the fallback -
	 * the two rules agree here, which is what makes the full case readable. */
	have[GM_SLOTS] = false;
	ck(gm_save_slot(have) == GM_SLOTS, "the last slot, while it is still empty");
	have[GM_SLOTS] = true;

	printf("and when every slot is taken:\n");
	ck(gm_save_slot(have) == GM_SLOTS, "the last slot, so the target never moves");

	/* The case the rule exists for. Slot 1 is where a finished playthrough
	 * or a point-of-no-return save tends to sit, and it is the slot a blind
	 * double-tap used to land on. */
	ck(gm_save_slot(have) != 1, "never slot 1, whatever else is on the card");

	/* Every arrangement of the card, not a sampled few: 64 is small enough to
	 * enumerate, and a rule this cheap has no excuse for an untested corner. */
	printf("and it never aims at Auto:\n");
	for (i = 0, ok = true; i < (1 << GM_SLOTS); i++) {
		int j, got;

		for (j = 1; j <= GM_SLOTS; j++) have[j] = (i >> (j - 1)) & 1;
		got = gm_save_slot(have);
		if (got < 1 || got > GM_SLOTS) ok = false;
	}
	ck(ok, "a numbered slot, for every arrangement of the card");
	ck(SLOT_AUTO > GM_SLOTS, "so the resume slot stays with the exit funnel");
}

/* Card sets. The rules are in cards.h so they can be checked without a
 * renderer, a device or a directory to read. */
static void card_sets(void)
{
	int i, j, ok;

	printf("card sets:\n");
	ck(CARD_SET_COUNT >= 2, "there is something to switch between");
	ck(!strcmp(CARD_SETS[0].id, CARDS_DEFAULT),
	   "the first set is the default one");
	/* Anything that falls back lands on the default set, so that set is the one
	 * that must never leave a card unnamed. */
	ck(CARD_SETS[0].labeled, "the default set's art names its systems");

	for (i = 0, ok = 1; i < CARD_SET_COUNT; i++) {
		if (!CARD_SETS[i].id[0] || !CARD_SETS[i].dir[0] || !CARD_SETS[i].name[0])
			ok = 0;
		/* The directory goes into a path. A separator in it would build a
		 * path nobody meant. */
		if (strchr(CARD_SETS[i].dir, '/') || strchr(CARD_SETS[i].dir, '\\')) ok = 0;
	}
	ck(ok, "every set has a non-empty id, dir and name, and no separators");

	/* Ids and names must be unique - one is what gets written down and the
	 * other is what gets read off the row. Directories must NOT have to be:
	 * a preset is art and presentation together, so two of them drawing the
	 * same folder a different way is the point, not a mistake. */
	for (i = 0, ok = 1; i < CARD_SET_COUNT; i++)
		for (j = 0; j < i; j++) {
			if (!strcmp(CARD_SETS[i].id, CARD_SETS[j].id)) ok = 0;
			if (!strcmp(CARD_SETS[i].name, CARD_SETS[j].name)) ok = 0;
		}
	ck(ok, "no two sets share an id or a name");

	printf("choosing one by id:\n");
	ck(cards_index("fancy") == 1, "a known id selects its set");
	ck(cards_index("classic") == 0, "so does the default");

	/* The setting outlives the directory. A set that is removed, or a card
	 * written by a build that had one more, must not leave the shelf blank. */
	ck(cards_index("no-such-set") == 0, "an unknown id falls back to the default");
	ck(cards_index("") == 0, "so does an empty one");
	ck(cards_index(NULL) == 0, "and so does none at all");

	printf("which way the shelves run:\n");
	ck(CARD_DIR_COUNT == 3, "three directions");
	/* Both is the two-axis surface, and it runs its systems vertically. A
	 * mode that says otherwise would send the system turn along the axis its
	 * games are already using. */
	ck(CARD_DIRS[2].both && CARD_DIRS[2].vertical,
	   "Both is vertical as well, since systems still run that way");
	ck(!CARD_DIRS[0].both && !CARD_DIRS[1].both,
	   "and it is the only one that merges the two shelves");
	ck(!strcmp(CARD_DIRS[0].id, CARDS_DIR_DEFAULT),
	   "the first is the default one");
	ck(!CARD_DIRS[0].vertical, "which is the horizontal row the shelf has always been");
	ck(CARD_DIRS[1].vertical, "and the other one actually differs");
	ck(cards_dir_index("vertical") == 1, "a known id selects its direction");
	/* Same rule as the themes: the setting outlives what it names. */
	ck(cards_dir_index("sideways") == 0, "an unknown id falls back to horizontal");
	ck(cards_dir_index(NULL) == 0, "and so does none at all");
	ck(cards_dir_step(0, 1) == 1 && cards_dir_step(2, 1) == 0, "stepping wraps");
	ck(cards_dir_step(0, -1) == CARD_DIR_COUNT - 1, "in both directions");

	printf("stepping through them:\n");
	ck(cards_step(0, 1) == 1, "forward");
	ck(cards_step(CARD_SET_COUNT - 1, 1) == 0, "and wraps at the end");
	ck(cards_step(0, -1) == CARD_SET_COUNT - 1, "back wraps too");
	for (i = 0, ok = 1; i < CARD_SET_COUNT; i++) {
		int k = cards_step(i, 1);
		if (k < 0 || k >= CARD_SET_COUNT || k == i) ok = 0;
	}
	ck(ok, "a step always lands on a different, real set");
}

int main(void)
{
	off_state();
	scanning_state();
	scanned_state();
	strength_words();
	respects_max();
	tortos_menu_offline();
	tortos_menu_online();
	wifi_row_wording();
	system_menu();
	auto_off_words();
	cursor_reaches();
	slots();
	save_slot();
	card_sets();
	step_terminates();
	window_returns();
	what_scrolls();
	info_rows();
	ingame_rows();
	audio_row();
	if (fails) { printf("\n%d menu check(s) failed\n", fails); return 1; }
	printf("\nok: menus contain what they should\n");
	return 0;
}
