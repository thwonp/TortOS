/* SPDX-License-Identifier: MIT */
/* See hare.h for what this is and what the PIN is worth. */
#include <dirent.h>
#include <stdarg.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>

#include "hare.h"
#include "xfer.h"

#define PIN_LEN        4
#define TOKEN_LEN     32           /* hex characters */
#define MAX_SESSIONS   4           /* a phone and a laptop, with room spare */
#define MAX_TRIES      5
#define LOCKOUT_MS 30000u

/* One page of listing. A ROM folder of 500 entries is ordinary and the whole
 * thing has to fit in one reply, because there is no paging in the client. */
#define JSON_MAX (256 * 1024)

/* Its own clock, for the same reason httpd.c has one: the lockout is about
 * wall time and has nothing to do with frames. */
static unsigned now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned)(ts.tv_sec * 1000u + (unsigned)(ts.tv_nsec / 1000000));
}

static char     g_web[XFER_PATH_MAX];
static char     g_pin[PIN_LEN + 1];
static char     g_token[MAX_SESSIONS][TOKEN_LEN + 1];
static int      g_ntoken;
static int      g_tries;
static unsigned g_locked_until;
static int      g_uploads;
static bool     g_shelf_changed;  /* something the shelf would show has moved */
static char     g_last[128];
static bool   (*g_pack_logs)(char *path, size_t pn, char *name, size_t nn);
static void   (*g_before_delete)(const char *abs);
static unsigned long g_in, g_out;

/* ---- randomness ------------------------------------------------------ */

/* /dev/urandom or nothing.
 *
 * Not rand(): the PIN is the only thing standing between a stranger on the
 * same network and this device's filesystem, and a PIN seeded from the clock
 * is one a person can work out from when the screen was opened. If urandom
 * cannot be read, the feature refuses to start rather than falling back to
 * something weaker - a lock that is sometimes not a lock is worse than an
 * obvious failure to open the door. */
static bool rand_bytes(void *out, size_t n)
{
	FILE *f = fopen("/dev/urandom", "rb");
	size_t got;

	if (!f) return false;
	got = fread(out, 1, n, f);
	fclose(f);
	return got == n;
}

static bool make_pin(void)
{
	unsigned char b[PIN_LEN];
	int i;

	if (!rand_bytes(b, sizeof b)) return false;
	/* Modulo 10 on a uniform byte is very slightly biased toward the low
	 * digits - 256 is not a multiple of 10. At four digits against a lockout
	 * of five tries that is not a distinction anybody can use, and the
	 * rejection loop it would take to remove costs more clarity than it buys.
	 * Written down so the next reader does not have to work out whether it
	 * was noticed. */
	for (i = 0; i < PIN_LEN; i++) g_pin[i] = (char)('0' + b[i] % 10);
	g_pin[PIN_LEN] = '\0';
	return true;
}

static bool make_token(char *out)
{
	static const char hex[] = "0123456789abcdef";
	unsigned char b[TOKEN_LEN / 2];
	int i;

	if (!rand_bytes(b, sizeof b)) return false;
	for (i = 0; i < (int)sizeof b; i++) {
		out[i * 2]     = hex[b[i] >> 4];
		out[i * 2 + 1] = hex[b[i] & 15];
	}
	out[TOKEN_LEN] = '\0';
	return true;
}

/* Length-independent compare, so the time this takes says nothing about how
 * much of the secret was right. Four digits and a lockout make that close to
 * theater, but it is four lines and the token it also guards is not four
 * digits. */
static bool secret_eq(const char *a, const char *b, size_t n)
{
	unsigned char diff = 0;
	size_t i;

	for (i = 0; i < n; i++) diff |= (unsigned char)(a[i] ^ b[i]);
	return diff == 0;
}

/* ---- sessions --------------------------------------------------------- */

static bool session_valid(const httpd_req *r)
{
	const char *c = httpd_header(r, "Cookie");
	const char *p;
	int i;

	if (!c) return false;
	p = strstr(c, "hare=");
	if (!p) return false;
	p += 5;
	if (strlen(p) < TOKEN_LEN) return false;
	for (i = 0; i < g_ntoken; i++)
		if (secret_eq(p, g_token[i], TOKEN_LEN)) return true;
	return false;
}

/* ---- small writers ---------------------------------------------------- */

typedef struct { char *p; size_t used, cap; bool over; } jbuf;

/* Literals go through this, so the LENGTH IS THE COMPILER'S PROBLEM.
 *
 * Every one of these was hand-counted first, and one of them was wrong:
 * `{"path":"","entries":[` is 22 characters and was written as 21, which cut
 * off the opening bracket and emitted `"entries":{`. The browser's parser said
 * "Expected double-quoted property name at position 71", which is a true and
 * completely unhelpful description of a missing bracket sixty characters
 * earlier. Nothing in C requires a human to count a string literal. */
/* The "" in front makes anything but a literal a compile error: given an
 * expression, sizeof measures a pointer, which is how an artist's covers went
 * out as seven bytes of garbage on 2026-10-05. */
#define JLIT(j, s) jput((j), "" s, sizeof (s) - 1)

static void jput(jbuf *j, const char *s, size_t n)
{
	if (j->over || j->used + n >= j->cap) { j->over = true; return; }
	memcpy(j->p + j->used, s, n);
	j->used += n;
}

static void jstr(jbuf *j, const char *s)
{
	JLIT(j, "\"");
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		/* Only what JSON requires. Everything else, including the UTF-8 that
		 * a filename on this card is, goes through untouched - re-encoding it
		 * would be inventing an opinion about text that nothing here has. */
		if (c == '"' || c == '\\') { JLIT(j, "\\"); jput(j, (char *)&c, 1); }
		else if (c == '\n') JLIT(j, "\\n");
		else if (c == '\r') JLIT(j, "\\r");
		else if (c == '\t') JLIT(j, "\\t");
		else if (c < 0x20) {
			char esc[7];
			snprintf(esc, sizeof esc, "\\u%04x", c);
			jput(j, esc, 6);
		} else jput(j, (char *)&c, 1);
	}
	JLIT(j, "\"");
}

static void jfmt(jbuf *j, const char *fmt, ...)
{
	char tmp[256];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(tmp, sizeof tmp, fmt, ap);
	va_end(ap);
	if (n > 0) jput(j, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

static const char *mime_for(const char *path)
{
	const char *dot = strrchr(path, '.');

	if (!dot) return "application/octet-stream";
	if (!strcasecmp(dot, ".html")) return "text/html; charset=utf-8";
	if (!strcasecmp(dot, ".css"))  return "text/css; charset=utf-8";
	if (!strcasecmp(dot, ".js"))   return "text/javascript; charset=utf-8";
	if (!strcasecmp(dot, ".json")) return "application/json";
	if (!strcasecmp(dot, ".png"))  return "image/png";
	/* The mark is an SVG, and X-Content-Type-Options: nosniff means a browser
	 * will NOT guess when this table does not know an extension - it refuses
	 * to render it at all. Which is the right behavior and exactly why the
	 * header is set; it also means a missing row here is a broken image
	 * rather than a slightly wrong one. */
	if (!strcasecmp(dot, ".svg"))  return "image/svg+xml";
	if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")) return "image/jpeg";
	if (!strcasecmp(dot, ".ttf"))  return "font/ttf";
	/* A ROM is a download, not something a browser should try to render. */
	return "application/octet-stream";
}

/* ---- the routes ------------------------------------------------------- */

static void note(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(g_last, sizeof g_last, fmt, ap);
	va_end(ap);
}

/* Did that write land somewhere the shelf reads?
 *
 * Only the ROM, Music and Audiobooks roots count; Muse is a shelf too. A save state, a
 * BIOS image or a renamed folder under Saves changes nothing the launcher
 * displays, and making those trigger a rescan would mean pulling a hundred
 * games off the card to react to a file nobody is looking at. Prefix match on
 * the resolved absolute path, so it is asking about where the bytes actually
 * went rather than what the URL said.
 */
static void note_write(const char *abs)
{
	int i;

	for (i = 0; i < xfer_root_count(); i++) {
		const xfer_root *rt = xfer_root_at(i);
		size_t n;

		if (strcmp(rt->name, "roms") != 0 && strcmp(rt->name, "music") != 0 &&
		    strcmp(rt->name, "books") != 0)
			continue;
		n = strlen(rt->path);
		if (!strncmp(abs, rt->path, n) && (abs[n] == '/' || abs[n] == '\0'))
			g_shelf_changed = true;
	}
}

bool hare_shelf_changed(void) { return g_shelf_changed; }

void hare_set_logs(bool (*pack)(char *path, size_t pn, char *name, size_t nn))
{
	g_pack_logs = pack;
}

void hare_set_before_delete(void (*fn)(const char *abs)) { g_before_delete = fn; }

/* ---- a folder and everything in it ----------------------------------------
 *
 * lstat, so a link is removed and never followed. The card is vfat and has no
 * links (see xfer.h), and this would still not walk out of a root if it ever
 * had one. Deep enough for any album or disc game; a folder deeper than this
 * is counted as too big rather than half deleted. */
#define TREE_DEPTH 12

/* Files and folders under `dir`, stopping once past `cap`. */
static int tree_count(const char *dir, int depth, int cap, int *files, int *folders)
{
	DIR *d;
	struct dirent *e;
	int n = 0;

	if (depth > TREE_DEPTH) return cap + 1;
	if (!(d = opendir(dir))) return 0;
	while ((e = readdir(d)) && n <= cap) {
		char p[XFER_PATH_MAX];
		struct stat st;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p) continue;
		if (lstat(p, &st) != 0) continue;
		n++;
		if (S_ISDIR(st.st_mode)) {
			(*folders)++;
			n += tree_count(p, depth + 1, cap - n, files, folders);
		} else {
			(*files)++;
		}
	}
	closedir(d);
	return n;
}

static bool tree_delete(const char *dir, int depth)
{
	DIR *d;
	struct dirent *e;
	bool ok = true;

	if (depth > TREE_DEPTH || !(d = opendir(dir))) return false;
	while ((e = readdir(d))) {
		char p[XFER_PATH_MAX];
		struct stat st;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p) { ok = false; continue; }
		if (lstat(p, &st) != 0) continue;
		if (S_ISDIR(st.st_mode)) ok = tree_delete(p, depth + 1) && ok;
		else if (remove(p) != 0) ok = false;
	}
	closedir(d);
	return rmdir(dir) == 0 && ok;
}

/* The name a listing shows for a path, or "" for a root. */
static const char *base_of(const char *p)
{
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

static void route_list(httpd_req *r)
{
	/* Two forms of the same path, and the difference matters.
	 *
	 * `raw` is what the browser sent, still percent-encoded, and it is what
	 * xfer_resolve must be given: that function decodes and validates in one
	 * step precisely so nobody can do those in the wrong order.
	 *
	 * `req` is the decoded form, and it is what goes into the JSON. The API
	 * speaks decoded paths in its bodies and encoded ones in its URLs, which
	 * is the only combination that survives a round trip: echoing the encoded
	 * form back put "roms%2FNES" in the breadcrumb and then encoded it again
	 * on the next click. */
	char  raw[XFER_PATH_MAX], req[XFER_PATH_MAX], abs[XFER_PATH_MAX];
	jbuf  j;
	DIR  *d;
	struct dirent *e;
	bool  first = true;

	httpd_query(r, "p", raw, sizeof raw);
	if (raw[0] && !xfer_decode(raw, req, sizeof req)) {
		httpd_reply_status(r, 400, "that is not a path");
		return;
	}
	if (!raw[0]) req[0] = '\0';

	/* No path means the roots themselves, which are not a directory anywhere
	 * on the card and so cannot be listed by reading one. */
	if (!raw[0]) {
		int i;

		j.p = malloc(4096);
		if (!j.p) { httpd_reply_status(r, 500, "out of memory"); return; }
		j.used = 0; j.cap = 4096; j.over = false;
		JLIT(&j, "{\"path\":\"\",\"entries\":[");
		for (i = 0; i < xfer_root_count(); i++) {
			const xfer_root *rt = xfer_root_at(i);

			if (!first) JLIT(&j, ",");
			first = false;
			JLIT(&j, "{\"name\":");
			jstr(&j, rt->label);
			JLIT(&j, ",\"path\":");
			jstr(&j, rt->name);
			JLIT(&j, ",\"dir\":true,\"size\":0}");
		}
		JLIT(&j, "]}");
		if (j.over) httpd_reply_status(r, 500, "listing did not fit");
		else httpd_reply(r, 200, "application/json", j.p, j.used, NULL);
		free(j.p);
		return;
	}

	if (!xfer_resolve(raw, abs, sizeof abs)) {
		httpd_reply_status(r, 403, "not somewhere you can look");
		return;
	}
	d = opendir(abs);
	if (!d) { httpd_reply_status(r, 404, "no such folder"); return; }

	j.p = malloc(JSON_MAX);
	if (!j.p) { closedir(d); httpd_reply_status(r, 500, "out of memory"); return; }
	j.used = 0; j.cap = JSON_MAX; j.over = false;
	JLIT(&j, "{\"path\":");
	jstr(&j, req);
	JLIT(&j, ",\"entries\":[");

	while ((e = readdir(d))) {
		char full[XFER_PATH_MAX];
		struct stat st;
		size_t n = strlen(e->d_name);

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		/* A transfer that died left this behind; it is not a file anyone
		 * asked for and showing it invites someone to try to use it. */
		if (n > 5 && !strcmp(e->d_name + n - 5, ".part")) continue;
		if (snprintf(full, sizeof full, "%s/%s", abs, e->d_name) >= (int)sizeof full)
			continue;
		if (stat(full, &st) != 0) continue;

		{
			char child[XFER_PATH_MAX];

			/* Skipped rather than truncated. A cut path is not this file's
			 * path, it is some other file's - and the browser would send it
			 * back as though it meant this one. Better to be absent from the
			 * listing than to be a name for the wrong thing. */
			if (snprintf(child, sizeof child, "%s/%s", req, e->d_name)
			    >= (int)sizeof child)
				continue;
			if (!first) JLIT(&j, ",");
			first = false;
			JLIT(&j, "{\"name\":");
			jstr(&j, e->d_name);
			JLIT(&j, ",\"path\":");
			jstr(&j, child);
		}
		jfmt(&j, ",\"dir\":%s,\"size\":%lld}",
		     S_ISDIR(st.st_mode) ? "true" : "false", (long long)st.st_size);
	}
	closedir(d);
	JLIT(&j, "]}");

	if (j.over) httpd_reply_status(r, 500, "that folder has more in it than fits");
	else httpd_reply(r, 200, "application/json", j.p, j.used, NULL);
	free(j.p);
}

/* Is there room for `want` more bytes where `path` is going? */
static bool room_for(const char *path, long want)
{
	struct statvfs vfs;
	char dir[XFER_PATH_MAX];
	char *slash;

	snprintf(dir, sizeof dir, "%s", path);
	slash = strrchr(dir, '/');
	if (slash) *slash = '\0';
	if (statvfs(dir, &vfs) != 0) return true;      /* cannot tell: allow it */
	return (double)vfs.f_bavail * (double)vfs.f_frsize >= (double)want;
}

/* ---- the entry points httpd calls ------------------------------------ */

/* What a request has put on the screen, so on_end knows it has more to say. */
enum { TAG_SENDING = 1, TAG_RECEIVING = 2 };

/* A request is over. Only the two that put something on the screen have
 * anything to add to it.
 *
 * An upload still tagged never reached its second call - its body did not
 * arrive whole - so it failed however the connection ended, and its part-file
 * goes when httpd releases the request. A download says whether it went out
 * whole. */
static void on_end(httpd_req *r, bool whole, void *ctx)
{
	char req[XFER_PATH_MAX], abs[XFER_PATH_MAX];
	int tag = httpd_tag(r);

	(void)ctx;
	if (!tag) return;
	httpd_query(r, "p", req, sizeof req);
	if (!xfer_resolve(req, abs, sizeof abs)) return;
	if (tag == TAG_RECEIVING) {
		if (g_uploads > 0) g_uploads--;
		note("failed: %s", base_of(abs));
	} else if (whole) {
		note("sent %s", base_of(abs));
	} else {
		note("failed: %s", base_of(abs));
	}
}

static void on_request(httpd_req *r, bool done, void *ctx)
{
	const char *m = httpd_method(r);
	const char *path = httpd_path(r);

	(void)ctx;

	/* ---- the parts that need no session ----------------------------- */
	if (!done && !strcmp(m, "POST") && !strcmp(path, "/api/auth")) {
		if (httpd_content_len(r) > 16) {
			httpd_reply_status(r, 400, "that is not a PIN");
			return;
		}
		httpd_want_body(r, HTTPD_BODY_MEM, NULL);
		return;
	}
	if (done && !strcmp(path, "/api/auth")) {
		size_t n;
		const char *body = httpd_body(r, &n);
		char cookie[128];

		if (now_ms() < g_locked_until) {
			note("PIN locked out");
			httpd_reply_status(r, 429, "too many wrong PINs - wait a moment");
			return;
		}
		if (n != PIN_LEN || !secret_eq(body, g_pin, PIN_LEN)) {
			if (++g_tries >= MAX_TRIES) {
				g_locked_until = now_ms() + LOCKOUT_MS;
				g_tries = 0;
				note("PIN locked out");
			} else {
				note("wrong PIN (%d of %d)", g_tries, MAX_TRIES);
			}
			httpd_reply_status(r, 401, "wrong PIN");
			return;
		}
		g_tries = 0;
		if (g_ntoken >= MAX_SESSIONS) g_ntoken = 0;    /* oldest falls off */
		if (!make_token(g_token[g_ntoken])) {
			httpd_reply_status(r, 500, "no randomness available");
			return;
		}
		snprintf(cookie, sizeof cookie,
		         "Set-Cookie: hare=%s; Path=/; HttpOnly; SameSite=Strict\r\n",
		         g_token[g_ntoken]);
		g_ntoken++;
		note("a browser signed in");
		httpd_reply(r, 200, "text/plain", "ok", 2, cookie);
		return;
	}

	/* The page and its assets. Served before the session check, because the
	 * page IS the PIN prompt - requiring a session to reach it would be a
	 * lock on the door to the lock. */
	if (!done && !strcmp(m, "GET") &&
	    (!strcmp(path, "/") || !strncmp(path, "/web/", 5))) {
		char file[XFER_PATH_MAX];
		const char *name = !strcmp(path, "/") ? "index.html" : path + 5;

		/* From res/web on the card, so the page can be restyled without a
		 * rebuild. The name is checked the same way a rename target is:
		 * these are assets, not a directory to browse. */
		if (!xfer_name_ok(name)) { httpd_reply_status(r, 404, "no"); return; }
		if (snprintf(file, sizeof file, "%s/%s", g_web, name) >= (int)sizeof file) {
			httpd_reply_status(r, 404, "no");
			return;
		}
		httpd_reply_file(r, file, mime_for(name), NULL);
		return;
	}

	/* ---- everything below needs one --------------------------------- */
	if (!session_valid(r)) {
		if (!done) httpd_want_body(r, HTTPD_BODY_NONE, NULL);
		httpd_reply_status(r, 401, "sign in first");
		return;
	}

	if (!strcmp(path, "/api/list")) {
		if (!done) { httpd_want_body(r, HTTPD_BODY_NONE, NULL); }
		if (done || httpd_content_len(r) == 0) route_list(r);
		return;
	}

	/* Download logs: packed when asked, named for the day, and sent. The
	 * pack is removed as soon as it is open - the stream keeps it alive - so
	 * nothing is left in /tmp however the download ends. */
	if (!strcmp(path, "/api/logs") && !strcmp(m, "GET")) {
		char pack[XFER_PATH_MAX], name[128], extra[256];

		if (!done) httpd_want_body(r, HTTPD_BODY_NONE, NULL);
		if (!g_pack_logs || !g_pack_logs(pack, sizeof pack, name, sizeof name)) {
			httpd_reply_status(r, 500, "the logs could not be packed");
			return;
		}
		snprintf(extra, sizeof extra,
		         "Content-Disposition: attachment; filename=\"%s\"\r\n", name);
		if (httpd_reply_file(r, pack, "application/gzip", extra)) {
			note("sending the logs");
			httpd_set_tag(r, TAG_SENDING);
		}
		remove(pack);
		return;
	}

	if (!strcmp(path, "/api/file") && !strcmp(m, "GET")) {
		char req[XFER_PATH_MAX], abs[XFER_PATH_MAX];
		char extra[XFER_PATH_MAX + 64];

		if (!done) httpd_want_body(r, HTTPD_BODY_NONE, NULL);
		httpd_query(r, "p", req, sizeof req);
		if (!xfer_resolve(req, abs, sizeof abs)) {
			httpd_reply_status(r, 403, "not somewhere you can look");
			return;
		}
		/* Named, so a browser saves it as itself rather than as "file". The
		 * name is quoted and cannot contain a quote - xfer_name_ok refused
		 * control bytes and the filesystem refuses the rest. */
		if (snprintf(extra, sizeof extra,
		             "Content-Disposition: attachment; filename=\"%s\"\r\n",
		             base_of(abs)) >= (int)sizeof extra) {
			/* Dropped whole rather than cut. A truncated header has no
			 * closing quote and no CRLF, so it does not become a shorter
			 * header - it becomes a broken response, with the next header
			 * running into this one. The download still works; the browser
			 * just names the file from the URL. */
			extra[0] = '\0';
		}
		/* Only once the file is really going out: a 404 said "sending" too. */
		if (httpd_reply_file(r, abs, mime_for(abs), extra[0] ? extra : NULL)) {
			note("sending %s", base_of(abs));
			httpd_set_tag(r, TAG_SENDING);
		}
		return;
	}

	if (!strcmp(path, "/api/file") && !strcmp(m, "PUT")) {
		char req[XFER_PATH_MAX], abs[XFER_PATH_MAX];

		httpd_query(r, "p", req, sizeof req);
		if (!xfer_resolve(req, abs, sizeof abs)) {
			if (!done) httpd_want_body(r, HTTPD_BODY_NONE, NULL);
			httpd_reply_status(r, 403, "not somewhere you can write");
			return;
		}
		if (!done) {
			char part[XFER_PATH_MAX];

			/* Before room_for, because a name the card cannot store is not a
			 * space problem and must not be reported as one. xfer_resolve
			 * checks where a path goes, not whether its last component is a
			 * legal filename here - that is this call. */
			if (!xfer_name_ok(base_of(abs))) {
				httpd_want_body(r, HTTPD_BODY_NONE, NULL);
				httpd_reply_status(r, 400, "that is not a usable name");
				return;
			}
			if (!room_for(abs, httpd_content_len(r))) {
				httpd_want_body(r, HTTPD_BODY_NONE, NULL);
				httpd_reply_status(r, 507, "not enough room on the card");
				return;
			}
			/* Into a part-file and renamed at the end, so a transfer that
			 * dies halfway leaves the original alone rather than replacing it
			 * with the front half of something else. The same argument as
			 * atomic.c makes for every config this launcher writes, and it
			 * matters more here: the thing being replaced may be the only
			 * copy of a save. */
			if (snprintf(part, sizeof part, "%s.part", abs) >= (int)sizeof part) {
				httpd_want_body(r, HTTPD_BODY_NONE, NULL);
				httpd_reply_status(r, 400, "that name is too long");
				return;
			}
			httpd_want_body(r, HTTPD_BODY_FILE, part);
			g_uploads++;
			note("receiving %s", base_of(abs));
			httpd_set_tag(r, TAG_RECEIVING);
			return;
		}
		{
			char part[XFER_PATH_MAX];

			/* Checked again, though the first call already refused a name
			 * this long: the two have to agree about where the bytes went,
			 * and "it cannot happen here because of something forty lines
			 * up" is a thing that stops being true when one of them moves. */
			if (g_uploads > 0) g_uploads--;
			/* The body is here. What happens to it is said below, not by
			 * on_end. */
			httpd_set_tag(r, 0);
			if (snprintf(part, sizeof part, "%s.part", abs) >= (int)sizeof part) {
				httpd_reply_status(r, 500, "that name is too long");
				return;
			}
			if (rename(part, abs) != 0) {
				remove(part);
				httpd_reply_status(r, 500, "could not put that in place");
				note("failed: %s", base_of(abs));
				return;
			}
			note_write(abs);
			note("received %s", base_of(abs));
			httpd_reply(r, 200, "text/plain", "ok", 2, NULL);
		}
		return;
	}

	if (!strcmp(path, "/api/rename") && !strcmp(m, "POST")) {
		char req[XFER_PATH_MAX], to[XFER_NAME_MAX];
		char abs[XFER_PATH_MAX], dst[XFER_PATH_MAX];
		char *slash;

		if (!done) { httpd_want_body(r, HTTPD_BODY_NONE, NULL); return; }
		httpd_query(r, "p", req, sizeof req);
		httpd_query(r, "to", to, sizeof to);
		if (!xfer_resolve(req, abs, sizeof abs)) {
			httpd_reply_status(r, 403, "not somewhere you can write");
			return;
		}
		/* The destination is decoded HERE and checked as one name. It is not
		 * run through xfer_resolve, because a rename names a sibling: letting
		 * it be a path would let a rename move a file to another root. */
		{
			char dec[XFER_NAME_MAX];

			if (!xfer_decode(to, dec, sizeof dec) || !xfer_name_ok(dec)) {
				httpd_reply_status(r, 400, "that is not a usable name");
				return;
			}
			snprintf(dst, sizeof dst, "%s", abs);
			slash = strrchr(dst, '/');
			if (!slash) { httpd_reply_status(r, 400, "cannot rename that"); return; }
			if ((size_t)(slash - dst) + 1 + strlen(dec) >= sizeof dst) {
				httpd_reply_status(r, 400, "that name is too long");
				return;
			}
			strcpy(slash + 1, dec);
		}
		if (rename(abs, dst) != 0) {
			httpd_reply_status(r, 500, strerror(errno));
			return;
		}
		note_write(dst);
		note_write(abs);   /* it left one name and arrived at another */
		note("renamed %s", base_of(dst));
		httpd_reply(r, 200, "text/plain", "ok", 2, NULL);
		return;
	}

	if (!strcmp(path, "/api/delete") && !strcmp(m, "POST")) {
		char req[XFER_PATH_MAX], abs[XFER_PATH_MAX];
		struct stat st;

		if (!done) { httpd_want_body(r, HTTPD_BODY_NONE, NULL); return; }
		httpd_query(r, "p", req, sizeof req);
		if (!xfer_resolve(req, abs, sizeof abs)) {
			httpd_reply_status(r, 403, "not somewhere you can write");
			return;
		}
		if (stat(abs, &st) != 0) { httpd_reply_status(r, 404, "not there"); return; }
		/* A folder goes as xfer_delete_rule says, and only with everything
		 * in it when the page asked for that by name (all=1), having shown
		 * what is inside first - see /api/count. It was rmdir alone once:
		 * one click should not take a whole system's ROMs, and a folder
		 * that had to be emptied first was a confirmation nobody had to
		 * design. That still holds where it matters, and an album or a
		 * book no longer has to be emptied a file at a time. */
		if (S_ISDIR(st.st_mode)) {
			const char *why;
			xfer_del rule = xfer_delete_rule(abs, &why);
			char all[4];

			httpd_query(r, "all", all, sizeof all);
			if (rule == XFER_DEL_NO) { httpd_reply_status(r, 403, why); return; }
			if (rmdir(abs) != 0) {
				int files = 0, folders = 0, n;

				if (rule != XFER_DEL_ALL) { httpd_reply_status(r, 409, why); return; }
				if (strcmp(all, "1") != 0) {
					httpd_reply_status(r, 409, "that folder is not empty");
					return;
				}
				n = tree_count(abs, 0, HARE_DELETE_MAX, &files, &folders);
				if (n > HARE_DELETE_MAX) {
					httpd_reply_status(r, 413, "too much in one go; delete it in parts");
					return;
				}
				if (g_before_delete) g_before_delete(abs);
				if (!tree_delete(abs, 0)) {
					httpd_reply_status(r, 500, "some of it could not be deleted");
					note_write(abs);
					return;
				}
			}
		} else if (remove(abs) != 0) {
			httpd_reply_status(r, 500, strerror(errno));
			return;
		}
		note_write(abs);
		note("deleted %s", base_of(abs));
		httpd_reply(r, 200, "text/plain", "ok", 2, NULL);
		return;
	}

	/* What deleting a folder would take, asked before the page asks you:
	 * how much is in it, and whether it may go whole, only empty, or not at
	 * all - xfer_delete_rule, with the reason when it will not. */
	if (!strcmp(path, "/api/count") && !strcmp(m, "GET")) {
		char req[XFER_PATH_MAX], abs[XFER_PATH_MAX];
		const char *why, *rule;
		int files = 0, folders = 0, n;
		struct stat st;
		jbuf j = { 0 };

		if (!done) httpd_want_body(r, HTTPD_BODY_NONE, NULL);
		httpd_query(r, "p", req, sizeof req);
		if (!xfer_resolve(req, abs, sizeof abs)) {
			httpd_reply_status(r, 403, "not somewhere you can look");
			return;
		}
		if (stat(abs, &st) != 0 || !S_ISDIR(st.st_mode)) {
			httpd_reply_status(r, 404, "not a folder");
			return;
		}
		switch (xfer_delete_rule(abs, &why)) {
		case XFER_DEL_ALL:   rule = "all";   break;
		case XFER_DEL_EMPTY: rule = "empty"; break;
		default:             rule = "no";    break;
		}
		n = tree_count(abs, 0, HARE_DELETE_MAX, &files, &folders);
		j.p = malloc(512);
		if (!j.p) { httpd_reply_status(r, 500, "out of memory"); return; }
		j.cap = 512;
		jfmt(&j, "{\"files\":%d,\"folders\":%d,\"too_many\":%s,\"rule\":\"%s\",\"why\":",
		     files, folders, n > HARE_DELETE_MAX ? "true" : "false", rule);
		jstr(&j, why);
		jfmt(&j, "}");
		if (j.over) httpd_reply_status(r, 500, "reply too long");
		else        httpd_reply(r, 200, "application/json", j.p, j.used, NULL);
		free(j.p);
		return;
	}

	if (!strcmp(path, "/api/mkdir") && !strcmp(m, "POST")) {
		char req[XFER_PATH_MAX], abs[XFER_PATH_MAX];

		if (!done) { httpd_want_body(r, HTTPD_BODY_NONE, NULL); return; }
		httpd_query(r, "p", req, sizeof req);
		if (!xfer_resolve(req, abs, sizeof abs)) {
			httpd_reply_status(r, 403, "not somewhere you can write");
			return;
		}
		if (!xfer_name_ok(base_of(abs))) {
			httpd_reply_status(r, 400, "that is not a usable name");
			return;
		}
		if (mkdir(abs, 0777) != 0) {
			httpd_reply_status(r, 409, strerror(errno));
			return;
		}
		note("made %s", base_of(abs));
		httpd_reply(r, 200, "text/plain", "ok", 2, NULL);
		return;
	}

	if (!done) httpd_want_body(r, HTTPD_BODY_NONE, NULL);
	httpd_reply_status(r, 404, "no such thing here");
}

/* ---- opening and closing --------------------------------------------- */

/* Part-files from a transfer that died. Swept at open rather than at close,
 * because the death that leaves one is usually the death that stops close
 * from running. */
static void sweep_parts(const char *dir, int depth)
{
	DIR *d;
	struct dirent *e;

	if (depth > 3) return;
	d = opendir(dir);
	if (!d) return;
	while ((e = readdir(d))) {
		char full[XFER_PATH_MAX];
		struct stat st;
		size_t n = strlen(e->d_name);

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		if (snprintf(full, sizeof full, "%s/%s", dir, e->d_name) >= (int)sizeof full)
			continue;
		if (stat(full, &st) != 0) continue;
		if (S_ISDIR(st.st_mode)) sweep_parts(full, depth + 1);
		else if (n > 5 && !strcmp(e->d_name + n - 5, ".part")) remove(full);
	}
	closedir(d);
}

bool hare_start(const char *roms_dir, const char *card_dir,
                const char *shared_dir, const char *web_dir)
{
	/* 80 first, so the address on the screen is one somebody can type without
	 * a colon in it. It needs root, which this process has; 8080 is there for
	 * anything that has taken 80 already. */
	static const int ports[] = { 80, 8080, 8081 };
	int i;

	xfer_init(roms_dir, card_dir, shared_dir);
	snprintf(g_web, sizeof g_web, "%s", web_dir);
	if (!make_pin()) return false;
	g_ntoken = 0;
	g_tries = 0;
	g_locked_until = 0;
	g_uploads = 0;
	g_shelf_changed = false;
	g_in = g_out = 0;
	g_last[0] = '\0';

	/* A root that is missing is one nobody can make: the page does not edit
	 * the roots themselves, and the card's top level is not a root. v1.0
	 * shipped no Music/, and updating only TortOS/ does not bring one. EEXIST
	 * is the usual answer and is ignored with the rest. */
	for (i = 0; i < xfer_root_count(); i++) {
		mkdir(xfer_root_at(i)->path, 0777);
		sweep_parts(xfer_root_at(i)->path, 0);
	}

	if (!httpd_start(ports, (int)(sizeof ports / sizeof ports[0]))) return false;
	note("waiting for a browser");
	return true;
}

void hare_stop(void)
{
	httpd_stop();
	/* The PIN and every session die with the screen. There is nothing left
	 * listening and nothing left to guess at. */
	memset(g_pin, 0, sizeof g_pin);
	memset(g_token, 0, sizeof g_token);
	g_ntoken = 0;
	g_uploads = 0;
}

const char *hare_pin(void) { return g_pin; }
int hare_port(void) { return httpd_port(); }

int hare_poll(void)
{
	unsigned long in = 0, out = 0;
	int did = httpd_poll(on_request, on_end, NULL);

	httpd_traffic(&in, &out);
	g_in += in;
	g_out += out;
	return did;
}

void hare_status(hare_stats *out)
{
	if (!out) return;
	out->clients = httpd_conn_count();
	out->in = g_in;
	out->out = g_out;
	out->uploads = g_uploads;
	snprintf(out->last, sizeof out->last, "%s", g_last);
	g_in = g_out = 0;
}
