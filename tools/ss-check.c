/* Can this build talk to ScreenScraper, and does an account survive a restart?
 *
 * Two halves, deliberately. The first needs nothing: it asserts what a build
 * WITHOUT the developer pair promises, which is that ScreenScraper reports
 * itself unavailable rather than half-working - the condition every caller
 * falls back to libretro on.
 *
 * The second needs a network and an account, and skips cleanly without one,
 * the same way check-raset does. It is the only place the real request is
 * exercised: a sign-in that cannot be tested is a sign-in nobody finds out
 * about until a player types their password into it. Credentials come from
 * .screenscraper.env (the working copy's, else ~/'s), are never printed, and
 * the password is wiped from memory here as well as in the module.
 *
 * Links src/ss.c, src/db.c, src/net.c and src/rajson.c, and NOT SDL.
 */
#include "../src/ss.h"

#include "../src/db.h"
#include "../src/ssfetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;

static void ck(int cond, const char *what)
{
	if (!cond) { printf("  FAIL %s\n", what); fails++; }
}

#define DEV "/tmp/tortos-ss-check-device.db"
#define LIB "/tmp/tortos-ss-check-library.db"

static void scrub(void)
{
	const char *suffix[] = { "", "-wal", "-shm" };
	char p[256];
	size_t i;

	for (i = 0; i < sizeof suffix / sizeof suffix[0]; i++) {
		snprintf(p, sizeof p, "%s%s", DEV, suffix[i]); unlink(p);
		snprintf(p, sizeof p, "%s%s", LIB, suffix[i]); unlink(p);
	}
}

/* One value out of .screenscraper.env: the working copy's (gitignored, and where
 * the Makefile reads the developer pair from) if there is one, else ~/'s. make
 * runs this from the repository root. Absent is not a failure. */
static bool env_value(const char *key, char *out, size_t n)
{
	char path[512], line[512];
	const char *home = getenv("HOME");
	FILE *f;
	size_t klen = strlen(key);

	out[0] = '\0';
	f = fopen(".screenscraper.env", "r");
	if (!f && home) {
		snprintf(path, sizeof path, "%s/.screenscraper.env", home);
		f = fopen(path, "r");
	}
	if (!f) return false;
	while (fgets(line, sizeof line, f)) {
		char *nl = strchr(line, '\n');

		if (nl) *nl = '\0';
		if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
			snprintf(out, n, "%s", line + klen + 1);
			break;
		}
	}
	fclose(f);
	memset(line, 0, sizeof line);
	return out[0] != '\0';
}

/* ---- reading a reply, with no network in sight -------------------------- */

/* Not a captured reply: a written one.
 *
 * A real jeuInfos answer is 40 KB of ScreenScraper's own text, which is not
 * ours to keep in this repository, and it would pin one game rather than the
 * SHAPES that are worth pinning - a name in four regions, a synopsis in five
 * languages, a genre list where only some entries are English, and eleven
 * media of which two are the cover. Each of those is a decision this file
 * makes and a place it could quietly make the wrong one. The live half below
 * is what holds this against the real thing. */
static const char *FIXTURE =
"{\"response\":{"
" \"ssuser\":{\"id\":\"someone\",\"maxthreads\":\"1\","
"            \"requeststoday\":\"20\",\"maxrequestsperday\":\"20000\"},"
" \"jeu\":{"
" \"noms\":[{\"region\":\"jp\",\"text\":\"Rokudenashi Blues\"},"
"           {\"region\":\"us\",\"text\":\"Good For Nothing Blues 2\"},"
"           {\"region\":\"eu\",\"text\":\"Nothing Blues\"}],"
" \"dates\":[{\"region\":\"jp\",\"text\":\"1993-03-05\"},"
"            {\"region\":\"us\",\"text\":\"1994-11-22\"}],"
" \"editeur\":{\"text\":\"Banpresto\"},"
" \"developpeur\":{\"text\":\"Nova\"},"
" \"joueurs\":{\"text\":\"1-2\"},"
" \"note\":{\"text\":\"14\"},"
" \"classifications\":[{\"type\":\"PEGI\",\"text\":\"12\"},"
"                      {\"type\":\"ESRB\",\"text\":\"T\"}],"
" \"genres\":[{\"noms\":[{\"langue\":\"fr\",\"text\":\"Combat\"},"
"                        {\"langue\":\"en\",\"text\":\"Fighting\"}]},"
"            {\"noms\":[{\"langue\":\"en\",\"text\":\"Beat em Up\"}]}],"
" \"synopsis\":[{\"langue\":\"fr\",\"text\":\"Un jeu de combat.\"},"
"              {\"langue\":\"en\",\"text\":\"Taison says &quot;hello&quot; with his fists.\\n\\nThen he says it again.\"}],"
" \"medias\":[{\"type\":\"wheel\",\"region\":\"us\",\"url\":\"https://x/wheel\"},"
"            {\"type\":\"box-2D\",\"region\":\"jp\",\"url\":\"https://x/box-jp\"},"
"            {\"type\":\"box-3D\",\"region\":\"us\",\"url\":\"https://x/box3d\"},"
"            {\"type\":\"box-2D\",\"region\":\"us\",\"url\":\"https://x/box-us\"},"
"            {\"type\":\"box-2D-back\",\"region\":\"us\",\"url\":\"https://x/back\"}]"
"}}}";

static void reads_a_reply(void)
{
	ss_result r;

	printf("reading a reply:\n");
	ck(ss_parse(FIXTURE, strlen(FIXTURE), "Good For Nothing Blues 2 (USA).zip", &r),
	   "it parses");
	ck(r.found, "and says the game is known");
	ck(!strcmp(r.name, "Good For Nothing Blues 2"), "the US name is preferred");
	ck(r.name_ok, "and the file's number agrees with it");
	ck(!strcmp(r.meta.year, "1994"), "the US date wins, and only its year is kept");
	ck(!strcmp(r.meta.publisher, "Banpresto"), "publisher");
	ck(!strcmp(r.meta.developer, "Nova"), "developer");
	ck(!strcmp(r.meta.players, "1-2"), "players");
	ck(!strcmp(r.meta.note, "14"), "their score");
	ck(!strcmp(r.meta.esrb, "T"), "the ESRB rating, not the PEGI one beside it");
	ck(!strcmp(r.meta.genres, "Fighting,Beat em Up"), "English genres only, joined");
	ck(strstr(r.meta.synopsis, "\"hello\"") != NULL, "&quot; is decoded");
	ck(strstr(r.meta.synopsis, "Un jeu") == NULL, "and the French one is not taken");
	ck(strstr(r.meta.synopsis, "\n\n") != NULL, "the paragraph break survives");
	ck(!strcmp(r.art, "https://x/box-us"), "the US box-2D is the cover");
	ck(!strcmp(r.art_region, "us"), "and it says which region that is");
	/* The account's counters, which a bulk run stops on. They arrive as
	 * STRINGS, and js_int answers 0 for a string - which would read as a
	 * fresh quota on every reply, so the run would never stop. */
	ck(r.used_today == 20 && r.max_today == 20000,
	   "the account's day is read off the same reply, strings and all");

	/* A reply about a different game, which is the case the whole check
	 * exists for: 4 of this card's 1,708 games get one. */
	ck(ss_parse(FIXTURE, strlen(FIXTURE), "Good For Nothing Blues 3 (USA).zip", &r),
	   "a reply about another game still parses");
	ck(r.found && !r.name_ok, "and is flagged rather than dropped");
}

/* The nine the whole card flagged, settled by reading each ROM's own header.
 * Four were genuinely another game; five were the right game under a romanized
 * or regional name. Both kinds are here, because a rule that only ever sees
 * one of them is a rule nobody can tune. */
static void the_name_check(void)
{
	static const struct { const char *file; const char *name; bool ok; } CASE[] = {
		{ "Kid Niki 2 (Japan) (Translated).zip", "Kid Niki - Radical Ninja", false },
		{ "Dragon Quest III (Japan) (Translated).zip", "Dragon Quest 1 And 2", false },
		{ "Shin Megami Tensei II (Japan) (Translated).zip", "Shin Megami Tensei", false },
		{ "Mother 3 (Japan) (Translated).zip", "ZZZ(notgame):#NONGAME", false },
		/* FLAGGED AND RIGHT: the two false alarms this rule accepts paying
		 * for. The romanized title drops the numeral in one, and buries it
		 * inside "GB2" in the other, where it is not a number any more. Both
		 * cost a libretro cover in place of a correct one, which is the cheap
		 * side of the trade - the four above would have put another game's
		 * synopsis on the shelf, where nothing about it looks wrong. */
		{ "Gargoyle's Quest II - The Demon Darkness (Japan) (Translated).zip",
		  "Makai Mura Gaiden - The Demon Darkness", false },
		{ "Pokemon Trading Card Game 2 (Japan) (Translated).zip",
		  "Pokemon Card GB2 - GR Dan Sanjou!", false },
		/* NOT FLAGGED, AND RIGHT, which is the ordinary case: 1,699 of the
		 * card's 1,708 games came through here without comment. */
		{ "Sonic The Hedgehog 2 (World).zip", "Sonic the Hedgehog 2", true },
		{ "Castlevania - Bloodlines (USA).zip", "Vampire Killer", true },
		{ "Samurai Pizza Cats (Japan) (Translated).zip", "Kyatto Ninden Teyandee", true },
		{ "Legend of Zelda, The - A Link to the Past (USA).zip",
		  "The Legend of Zelda: A Link to the Past", true },
		/* A dot in THEIR title is not an extension. This one was cut to
		 * "Super Mario Bros" and lost its 3, on the fresh card 2026-09-26. */
		{ "Super Mario Bros. 3 (USA) (Rev 1).zip", "Super Mario Bros. 3", true },
	};
	size_t i;

	printf("the sequel-number check:\n");
	for (i = 0; i < sizeof CASE / sizeof CASE[0]; i++) {
		const char *names[1] = { CASE[i].name };

		ck(ss_name_ok(CASE[i].file, names, 1) == CASE[i].ok, CASE[i].file);
	}
	/* A game whose name carries no number is not checked at all, because
	 * there is nothing here to check it with. */
	{
		const char *nope[1] = { "Something Else Entirely" };

		ck(ss_name_ok("Contra (USA).zip", nope, 1), "no number means no opinion");
	}
}

static void the_system_ids(void)
{
	printf("the system table:\n");
	ck(ss_system_id("NES") == 3, "NES");
	ck(ss_system_id("Genesis") == 1, "Genesis");
	ck(ss_system_id("TurboGrafx-16") == 31, "TurboGrafx-16, CD games included");
	ck(ss_system_id("Neo Geo Pocket Color") == 82, "Neo Geo Pocket Color");
	ck(ss_system_id("PlayStation") == 57, "PlayStation");
	ck(ss_system_id("Game Boy Advance") == 12, "Game Boy Advance");
	ck(ss_system_id("Dreamcast") == 0, "a folder they have no system for is 0");
	ck(ss_system_id(NULL) == 0, "and no folder at all is survivable");
}

int main(void)
{
	char user[SS_USER_MAX], pass[SS_PASS_MAX], err[160] = "";
	bool have_account;

	the_system_ids();
	the_name_check();
	reads_a_reply();

	printf("the developer pair:\n");
	if (ss_have_dev()) {
		printf("  built in - the live half of this check will run\n");
	} else {
		printf("  absent, which is a supported build\n");
		ck(!ss_signed_in(), "nothing is signed in without it");
	}

	scrub();
	if (!db_init(DEV, LIB, NULL)) {
		printf("  (no sqlite here, so storage is not checked)\n");
		printf("\nok: ScreenScraper reports what it can do\n");
		return fails ? 1 : 0;
	}

	printf("with no account stored:\n");
	ss_creds_clear();
	ck(!ss_creds_load(), "loading finds nothing");
	ck(!ss_signed_in(), "and nothing claims to be signed in");
	ck(ss_user()[0] == '\0', "the account name is empty");

	have_account = env_value("SS_USER", user, sizeof user) &&
	               env_value("SS_PASS", pass, sizeof pass);
	if (!ss_have_dev() || !have_account) {
		printf("the live sign-in:\n  skipped - %s\n",
		       !ss_have_dev() ? "this build has no developer pair"
		                      : "no SS_USER and SS_PASS in .screenscraper.env");
		db_shutdown();
		scrub();
		printf("\nok: ScreenScraper reports what it can do\n");
		return fails ? 1 : 0;
	}

	printf("the live sign-in:\n");
	if (!ss_sign_in(user, pass, err, sizeof err)) {
		/* A network that is down is not a broken launcher, so this reports
		 * rather than fails. What it must never do is report success. */
		printf("  did not sign in: %s\n", err);
		ck(!ss_signed_in(), "and it did not claim to have");
	} else {
		ck(ss_signed_in(), "signed in");
		ck(strcmp(ss_user(), user) == 0, "under the account that was given");

		/* The point of storing it: a handheld that has been off still knows
		 * the account, because there is no token to re-ask for. */
		ss_creds_clear();
		ck(!ss_signed_in(), "clearing really clears it");
		ck(ss_creds_load(), "and it comes back from the database");
		ck(strcmp(ss_user(), user) == 0, "still the same account");

		/* And the whole round trip, against a game whose checksum this card
		 * has had an answer for since 2026-09-15. */
		{
			ss_result r;

			if (!ss_lookup("Genesis", "Gunstar Heroes (USA).zip",
			               0x1F3C05A1u, &r)) {
				printf("  the live lookup did not answer\n");
			} else {
				ck(r.found, "a real lookup finds a real game");
				ck(r.name[0] != '\0', "and it has a name");
				ck(r.meta.year[0] != '\0', "and a year");
				ck(r.meta.synopsis[0] != '\0', "and prose");
				ck(r.art[0] != '\0', "and a cover to fetch");
				printf("  looked up %s (%s), %d characters, cover %s\n",
				       r.name, r.meta.year, (int)strlen(r.meta.synopsis),
				       r.art_region);
			}
			/* The retry, against the checksum that started it: this one is
			 * filed upstream under the first Kid Niki, and asking by name
			 * gets the right game. Three of this card's four wrong answers
			 * come back this way. */
			if (ss_lookup("NES", "Kid Niki 2 (Japan) (Translated).zip",
			              0xEFAD8ECEu, &r) && r.found) {
				ck(r.name_ok, "a flagged checksum answer is retried by name");
				ck(strstr(r.name, "Yancha Maru 2") != NULL,
				   "and the retry is the right game");
				printf("  retried by name: %s\n", r.name);
			}
		}
		/* Said out loud, because every assertion above is silent when it
		 * passes and a silent section reads exactly like a skipped one. The
		 * account name is not printed: it is half of a credential. */
		printf("  signed in, stored, and read back after a clear\n");
	}
	memset(pass, 0, sizeof pass);

	db_shutdown();
	scrub();
	if (fails) { printf("\n%d ScreenScraper check(s) failed\n", fails); return 1; }
	printf("\nok: ScreenScraper reports what it can do\n");
	return 0;
}
