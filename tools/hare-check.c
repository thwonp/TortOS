/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* Can a browser get at the card without the PIN?
 *
 *     make check-hare
 *
 * check-xfer proves a path cannot climb out of a root. This proves the routes
 * that use it are actually behind the lock - which is a separate claim, and the
 * one that fails when somebody adds a convenient endpoint and forgets. Every
 * route is tried twice: once with no session at all, and once with a session,
 * so a route that is accidentally public shows up as a pass where a 401 was
 * wanted.
 *
 * Also here: the lockout, which is the entire reason a four-digit PIN is a lock
 * rather than a doorbell. Ten thousand guesses is seconds of scripting; five
 * tries and a wait is not.
 *
 * A real server on a real socket, against a real directory tree in /tmp.
 */
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "../src/hare.h"
#include "../src/xfer.h"

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

static char g_root[128], g_card[160], g_roms[160], g_web[160], g_shared[160];
static char g_cookie[128];

/* ---- the server on its own thread ------------------------------------ */

static pthread_t    g_thread;
static volatile int g_stop;

static void *server_loop(void *u)
{
	(void)u;
	while (!g_stop) { hare_poll(); usleep(200); }
	return NULL;
}

static bool serve_up(void)
{
	g_stop = 0;
	if (!hare_start(g_roms, g_card, g_shared, g_web)) return false;
	pthread_create(&g_thread, NULL, server_loop, NULL);
	return true;
}

static void serve_down(void)
{
	if (g_thread) { g_stop = 1; pthread_join(g_thread, NULL); g_thread = 0; }
	hare_stop();
}

/* ---- an ordinary client ---------------------------------------------- */

static bool complete(const char *out, size_t got)
{
	const char *blank = strstr(out, "\r\n\r\n");
	const char *cl;
	long len;

	if (!blank) return false;
	cl = strstr(out, "Content-Length: ");
	if (!cl || cl > blank) return false;
	len = strtol(cl + 16, NULL, 10);
	return (size_t)(blank + 4 - out) + (size_t)len <= got;
}

/* One request, one answer. Returns the status, and copies the body out. */
static int req(const char *method, const char *path, const char *cookie,
               const void *body, size_t blen, char *outbody, size_t outn)
{
	struct sockaddr_in a;
	struct timeval tv = { 3, 0 };
	char head[1024], resp[65536];
	size_t got = 0, sent = 0;
	int fd, n, status = 0;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons((unsigned short)hare_port());
	if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
		printf("  FAIL: connect: %s\n", strerror(errno));
		failures++;
		close(fd);
		return -1;
	}
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

	n = snprintf(head, sizeof head,
	             "%s %s HTTP/1.1\r\nHost: x\r\n%s%s%s"
	             "Content-Length: %zu\r\nConnection: close\r\n\r\n",
	             method, path,
	             cookie ? "Cookie: hare=" : "", cookie ? cookie : "",
	             cookie ? "\r\n" : "", blen);
	while (sent < (size_t)n) {
		ssize_t w = write(fd, head + sent, (size_t)n - sent);
		if (w <= 0) break;
		sent += (size_t)w;
	}
	sent = 0;
	while (sent < blen) {
		ssize_t w = write(fd, (const char *)body + sent, blen - sent);
		if (w <= 0) break;
		sent += (size_t)w;
	}
	while (got + 1 < sizeof resp) {
		ssize_t r = recv(fd, resp + got, sizeof resp - got - 1, 0);
		if (r <= 0) break;
		got += (size_t)r;
		resp[got] = '\0';
		if (complete(resp, got)) break;
	}
	resp[got] = '\0';
	close(fd);
	sscanf(resp, "HTTP/1.1 %d", &status);
	if (outbody) {
		const char *b = strstr(resp, "\r\n\r\n");
		snprintf(outbody, outn, "%s", b ? b + 4 : "");
	}
	/* Stash a session cookie if one was handed out. */
	{
		const char *sc = strstr(resp, "Set-Cookie: hare=");
		if (sc) {
			sc += 17;
			snprintf(g_cookie, sizeof g_cookie, "%.*s",
			         (int)strcspn(sc, ";\r\n"), sc);
		}
	}
	return status;
}

/* Is this JSON, structurally?
 *
 * The check used to look for substrings - strstr(body, "\"roms\"") - and
 * passed with flying colors over a reply that no parser would accept: a
 * hand-counted string literal had swallowed the opening bracket, so the
 * listing read "entries":{...},{...}] and the page died on it. A check that
 * looks for words in a document it never parses is checking that the words
 * are there, which is not the claim being made.
 *
 * Not a full parser: brackets and braces balanced, in order, ignoring
 * anything inside a string. That is enough to catch every way a hand-built
 * emitter goes wrong, which is by dropping or doubling a delimiter. */
static bool json_ok(const char *s)
{
	int depth = 0;
	bool instr = false;

	if (!s || *s != '{') return false;
	for (; *s; s++) {
		if (instr) {
			if (*s == '\\' && s[1]) s++;
			else if (*s == '"') instr = false;
			continue;
		}
		if (*s == '"') { instr = true; continue; }
		if (*s == '{' || *s == '[') depth++;
		else if (*s == '}' || *s == ']') { if (--depth < 0) return false; }
	}
	return depth == 0 && !instr;
}

/* An "entries" array, opened with a bracket rather than whatever else. */
static bool has_array(const char *s, const char *key)
{
	char pat[64];

	snprintf(pat, sizeof pat, "\"%s\":[", key);
	return strstr(s, pat) != NULL;
}

static int get(const char *path, const char *cookie)
{
	return req("GET", path, cookie, NULL, 0, NULL, 0);
}
static int post(const char *path, const char *cookie)
{
	return req("POST", path, cookie, NULL, 0, NULL, 0);
}

/* ---- a small tree to serve --------------------------------------------- */

static void mkfile(const char *path, const char *text)
{
	FILE *f = fopen(path, "wb");
	if (f) { fputs(text, f); fclose(f); }
}

static void build_tree(void)
{
	char p[256];

	snprintf(g_root, sizeof g_root, "/tmp/tortos-hare-%ld", (long)getpid());
	snprintf(g_card, sizeof g_card, "%s/card", g_root);
	snprintf(g_roms, sizeof g_roms, "%s/card/Roms", g_root);
	snprintf(g_web,  sizeof g_web,  "%s/web", g_root);
	snprintf(g_shared, sizeof g_shared, "%s/shared", g_root);

	mkdir(g_root, 0777);
	mkdir(g_card, 0777);
	mkdir(g_roms, 0777);
	mkdir(g_web, 0777);
	mkdir(g_shared, 0777);
	snprintf(p, sizeof p, "%s/.tortos", g_shared);      mkdir(p, 0777);
	snprintf(p, sizeof p, "%s/.tortos/NES", g_shared);  mkdir(p, 0777);
	snprintf(p, sizeof p, "%s/.tortos/NES/Contra.state", g_shared);
	mkfile(p, "STATEDATA");
	/* The launcher's own, in the same directory as the states. */
	snprintf(p, sizeof p, "%s/.tortos/cheevos.cfg", g_shared);
	mkfile(p, "1447\t6850\ts\n");
	snprintf(p, sizeof p, "%s/Bios", g_card);   mkdir(p, 0777);
	snprintf(p, sizeof p, "%s/Saves", g_card);  mkdir(p, 0777);
	snprintf(p, sizeof p, "%s/NES", g_roms);    mkdir(p, 0777);
	snprintf(p, sizeof p, "%s/NES/Contra.nes", g_roms); mkfile(p, "ROMDATA");
	snprintf(p, sizeof p, "%s/index.html", g_web);      mkfile(p, "<h1>hare</h1>");
	/* Outside every root, and the thing an escape would be aiming at. */
	snprintf(p, sizeof p, "%s/secret.txt", g_card);     mkfile(p, "TOPSECRET");
}

static void rmtree(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;

	if (!d) return;
	while ((e = readdir(d))) {
		char full[512];
		struct stat st;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
		if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) rmtree(full);
		else remove(full);
	}
	closedir(d);
	rmdir(dir);
}

int main(void)
{
	char body[4096];
	char pin[8];

	build_tree();
	printf("hare: the routes, and who may use them\n");

	if (!serve_up()) {
		printf("  FAIL: the server did not start\n");
		rmtree(g_root);
		return 1;
	}
	snprintf(pin, sizeof pin, "%s", hare_pin());
	CHECK(strlen(pin) == 4, "the PIN is \"%s\", wanted four digits", pin);
	{
		int i, digits = 0;
		for (i = 0; pin[i]; i++) if (pin[i] >= '0' && pin[i] <= '9') digits++;
		CHECK(digits == 4, "the PIN is not four digits");
	}

	printf("  the page is reachable without a PIN, because it IS the prompt:\n");
	CHECK(get("/", NULL) == 200, "the page was not served");

	printf("  nothing else is:\n");
	{
		/* Every route, unauthenticated. A 401 each. This is the check that
		 * fails the day somebody adds an endpoint and forgets the session
		 * test - which is why it lists them rather than sampling. */
		CHECK(get("/api/list", NULL) == 401, "listing the roots needed no PIN");
		CHECK(get("/api/list?p=roms", NULL) == 401, "listing ROMs needed no PIN");
		CHECK(get("/api/file?p=roms/NES/Contra.nes", NULL) == 401,
		      "DOWNLOADING A ROM NEEDED NO PIN");
		CHECK(req("PUT", "/api/file?p=roms/NES/x.nes", NULL, "X", 1, NULL, 0) == 401,
		      "UPLOADING NEEDED NO PIN");
		CHECK(post("/api/delete?p=roms/NES/Contra.nes", NULL) == 401,
		      "DELETING NEEDED NO PIN");
		CHECK(post("/api/rename?p=roms/NES/Contra.nes&to=x", NULL) == 401,
		      "RENAMING NEEDED NO PIN");
		CHECK(post("/api/mkdir?p=roms/NES/sub", NULL) == 401,
		      "MAKING A FOLDER NEEDED NO PIN");
		/* And the file is still there. */
		{
			char p[256];
			struct stat st;
			snprintf(p, sizeof p, "%s/NES/Contra.nes", g_roms);
			CHECK(stat(p, &st) == 0, "an unauthenticated call deleted the ROM");
		}
	}

	printf("  a wrong PIN is refused, and a right one is not:\n");
	CHECK(req("POST", "/api/auth", NULL, "0000", 4, NULL, 0) == 401 ||
	      !strcmp(pin, "0000"), "a wrong PIN was accepted");
	g_cookie[0] = '\0';
	CHECK(req("POST", "/api/auth", NULL, pin, 4, NULL, 0) == 200,
	      "the right PIN was refused");
	CHECK(g_cookie[0] != '\0', "no session cookie came back");
	CHECK(strlen(g_cookie) == 32, "the session token is %zu chars, wanted 32",
	      strlen(g_cookie));

	printf("  a made-up cookie is not a session:\n");
	CHECK(get("/api/list", "00000000000000000000000000000000") == 401,
	      "a guessed token was accepted");
	CHECK(get("/api/list", "short") == 401, "a short token was accepted");

	printf("  with a session, the routes work:\n");
	{
		CHECK(req("GET", "/api/list", g_cookie, NULL, 0, body, sizeof body) == 200,
		      "listing the roots failed");
		CHECK(json_ok(body), "the roots listing is not valid JSON: %s", body);
		CHECK(has_array(body, "entries"),
		      "entries is not an array: %s", body);
		CHECK(strstr(body, "\"roms\"") && strstr(body, "\"bios\"") &&
		      strstr(body, "\"saves\""), "the roots listing is wrong: %s", body);

		CHECK(req("GET", "/api/list?p=roms/NES", g_cookie, NULL, 0,
		          body, sizeof body) == 200, "listing NES failed");
		CHECK(json_ok(body), "the NES listing is not valid JSON: %s", body);
		CHECK(has_array(body, "entries"), "entries is not an array: %s", body);
		CHECK(strstr(body, "Contra.nes") != NULL,
		      "the ROM is missing from the listing: %s", body);

		/* A name with the characters JSON cares about, which is the other way
		 * a hand-built emitter breaks: unescaped, the reply stops parsing. */
		{
			char p[512];
			snprintf(p, sizeof p, "%s/NES/quote\"back\\slash.nes", g_roms);
			mkfile(p, "X");
			CHECK(req("GET", "/api/list?p=roms/NES", g_cookie, NULL, 0,
			          body, sizeof body) == 200, "listing after odd name failed");
			CHECK(json_ok(body),
			      "a name with a quote or backslash broke the JSON: %s", body);
			remove(p);
		}

		CHECK(req("GET", "/api/file?p=roms/NES/Contra.nes", g_cookie, NULL, 0,
		          body, sizeof body) == 200, "download failed");
		CHECK(!strcmp(body, "ROMDATA"), "downloaded \"%s\"", body);

		CHECK(req("PUT", "/api/file?p=roms/NES/New.nes", g_cookie,
		          "NEWROM", 6, NULL, 0) == 200, "upload failed");
		{
			char p[256];
			struct stat st;
			snprintf(p, sizeof p, "%s/NES/New.nes", g_roms);
			CHECK(stat(p, &st) == 0 && st.st_size == 6,
			      "the upload did not land");
			/* The part-file is renamed, not left behind. */
			snprintf(p, sizeof p, "%s/NES/New.nes.part", g_roms);
			CHECK(stat(p, &st) != 0, "a part-file was left behind");
		}

		CHECK(post("/api/rename?p=roms/NES/New.nes&to=Renamed.nes", g_cookie) == 200,
		      "rename failed");
		CHECK(post("/api/delete?p=roms/NES/Renamed.nes", g_cookie) == 200,
		      "delete failed");
	}

	printf("  and a session does not let a path climb out:\n");
	{
		/* xfer_resolve already refuses these; this proves the ROUTES ask it,
		 * which is a different claim and the one that regresses. */
		CHECK(get("/api/file?p=../secret.txt", g_cookie) == 403,
		      "read a file outside every root");
		CHECK(get("/api/file?p=roms/../../secret.txt", g_cookie) == 403,
		      "climbed out of a root");
		CHECK(get("/api/list?p=roms/..", g_cookie) == 403, "listed above a root");
		CHECK(post("/api/delete?p=roms/../../secret.txt", g_cookie) == 403,
		      "DELETED A FILE OUTSIDE EVERY ROOT");
		{
			char p[256];
			struct stat st;
			snprintf(p, sizeof p, "%s/secret.txt", g_card);
			CHECK(stat(p, &st) == 0, "the file outside the roots was deleted");
		}
		/* A rename names a sibling. If it could name a path, it could move a
		 * file into another root - or out of all of them. */
		CHECK(post("/api/rename?p=roms/NES/Contra.nes&to=../../escaped", g_cookie)
		      != 200, "a rename escaped its directory");
		CHECK(post("/api/rename?p=roms/NES/Contra.nes&to=%2e%2e%2fescaped",
		           g_cookie) != 200, "an encoded rename escaped its directory");
		CHECK(post("/api/rename?p=roms/NES/Contra.nes&to=sub%2fx", g_cookie)
		      != 200, "a rename carried a separator");
	}

	printf("  save states are reachable and the config beside them is not:\n");
	{
		CHECK(req("GET", "/api/file?p=states/NES/Contra.state", g_cookie,
		          NULL, 0, body, sizeof body) == 200,
		      "a save state could not be fetched");
		CHECK(!strcmp(body, "STATEDATA"), "the state came back as \"%s\"", body);
		CHECK(get("/api/file?p=states/cheevos.cfg", g_cookie) == 403,
		      "READ THE UNLOCK STORE THAT SITS BESIDE THE SAVE STATES");
		CHECK(post("/api/delete?p=states/cheevos.cfg", g_cookie) == 403,
		      "DELETED THE ONLY RECORD OF UNSUBMITTED UNLOCKS");
		{
			char p[512];
			struct stat st;
			snprintf(p, sizeof p, "%s/.tortos/cheevos.cfg", g_shared);
			CHECK(stat(p, &st) == 0, "cheevos.cfg was deleted");
		}
	}

	printf("  the lockout, which is what makes four digits a lock:\n");
	{
		char wrong[5];
		int i, last = 0;

		/* A PIN that is definitely not the right one. */
		snprintf(wrong, sizeof wrong, "%04d",
		         (atoi(pin) + 1) % 10000);
		for (i = 0; i < 6; i++)
			last = req("POST", "/api/auth", NULL, wrong, 4, NULL, 0);
		CHECK(last == 429, "after six wrong PINs the answer was %d, wanted 429",
		      last);
		/* And the right PIN is refused too while locked out - otherwise the
		 * lockout is advice rather than a lock. */
		CHECK(req("POST", "/api/auth", NULL, pin, 4, NULL, 0) == 429,
		      "the lockout let the right PIN through, so it only slows a "
		      "guesser down between attempts");
	}

	printf("  closing the screen takes the whole thing down:\n");
	serve_down();
	CHECK(hare_port() == 0, "still listening after stop");
	CHECK(hare_pin()[0] == '\0', "the PIN outlived the screen");

	rmtree(g_root);
	if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
	printf("\nok: every check passed\n");
	return 0;
}
