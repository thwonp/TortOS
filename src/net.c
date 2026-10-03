/* SPDX-License-Identifier: MIT */
/* See net.h for why this shells out to curl and why the request is a file. */
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "net.h"

#define RA_URL "https://retroachievements.org/dorequest.php"

static char g_ca[512];

void net_set_ca_path(const char *path)
{
	snprintf(g_ca, sizeof g_ca, "%s", path ? path : "");
}

static const char *curl_bin(void)
{
	static const char *tried[] = { "/usr/bin/curl", "/bin/curl", "curl" };
	static const char *found;
	size_t i;

	if (found) return found;
	for (i = 0; i < sizeof tried / sizeof tried[0]; i++) {
		if (access(tried[i], X_OK) == 0) { found = tried[i]; return found; }
	}
	return NULL;
}

/* Quote for curl's config-file syntax: values are double quoted, and inside
 * them only backslash and quote need escaping. Nothing here is attacker
 * controlled today - hashes, ids and a token - but a ROM title will end up in
 * one of these eventually and this is the cheap moment to be right. */
static void cfg_quote(FILE *f, const char *v)
{
	fputc('"', f);
	for (; *v; v++) {
		if (*v == '"' || *v == '\\') fputc('\\', f);
		fputc(*v, f);
	}
	fputc('"', f);
}

/* The same config, for a GET of an arbitrary URL. No fields, no token, so
 * nothing here needs hiding from a process list - but it goes through a file
 * anyway, because two ways of invoking curl is two things to keep right. */
/* Who is asking, on every request. MusicBrainz asks each application to name
 * itself and a way to reach whoever runs it, and to throttle or refuse what
 * does not; the project's address is that way, and nobody's email is. The
 * other services are sent the same line, since one client should say one
 * thing about itself. */
#define NET_UA "TortOS/" TORTOS_VERSION " ( https://github.com/ericreinsmidt/TortOS )"

static bool write_get_config(const char *path, const char *url,
                            const char *body_path, int timeout_s)
{
	FILE *f;
	int fd;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) return false;
	f = fdopen(fd, "w");
	if (!f) { close(fd); return false; }

	/* No show-error. A 404 is a normal answer to a GET here - box art asks
	 * for the name the card uses and expects to be told no about one in six -
	 * and curl printing "curl: (22)" for each put thirty-two of them in the
	 * device log per run. The caller knows what it asked for and says so in
	 * words; curl's exit code is enough for it to know. */
	fprintf(f, "silent\nfail\nlocation\n");
	/* THE STATUS, ON STDOUT, WITH THE BODY SENT TO ITS OWN FILE.
	 *
	 * curl runs with `fail`, so an HTTP error is an exit code and nothing
	 * else - and the callers now need to tell one refusal from another.
	 * ScreenScraper answers "we do not know this game" and "you have spent
	 * today's quota" and "too many at once" all as 4xx, and a run that treats
	 * those alike either gives up on a library it could have scraped or keeps
	 * asking a server that has already said no.
	 *
	 * write-out prints whatever happened, error or not, so the code arrives
	 * even when the body does not. It goes to stdout, which means the body
	 * cannot: `output` names the file instead. The device carries curl 7.54,
	 * which has neither --fail-with-body nor write-out's %%output{} - this
	 * works on any of them. */
	fprintf(f, "write-out = "); cfg_quote(f, "%{http_code}"); fputc('\n', f);
	if (body_path) { fprintf(f, "output = "); cfg_quote(f, body_path); fputc('\n', f); }
	fprintf(f, "max-time = %d\n", timeout_s);
	if (g_ca[0]) { fprintf(f, "cacert = "); cfg_quote(f, g_ca); fputc('\n', f); }
	fprintf(f, "user-agent = "); cfg_quote(f, NET_UA); fputc('\n', f);
	fprintf(f, "url = "); cfg_quote(f, url); fputc('\n', f);
	fclose(f);
	return true;
}

/* `url` is where it goes and `get` says how the fields travel: as a body, or
 * urlencoded onto the query string. Both were once RA_URL and a POST, which is
 * all RetroAchievements needs; ScreenScraper authenticates every call with
 * query parameters instead, and it carries the account password in them.
 *
 * Which is exactly why it goes through here rather than through a URL built by
 * a caller: curl's --data-urlencode with --get puts those parameters in THIS
 * file, mode 0600, and never in argv where anything that can list processes
 * would read them. One place knows about the trust store, the agent and the
 * quoting, and it stays one place. */
static bool write_config(const char *path, const char *url, bool get,
                         const net_field *fl, int n, int timeout_s)
{
	FILE *f;
	int fd, i;

	/* 0600 from the moment it exists: it carries the account token, and a
	 * file created readable and chmod'ed after has a window. */
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) return false;
	f = fdopen(fd, "w");
	if (!f) { close(fd); return false; }

	fprintf(f, "silent\nshow-error\nfail\n");
	fprintf(f, "max-time = %d\n", timeout_s);
	/* Verification stays ON. The device has no trust store of its own, which
	 * is the whole reason this file ships; -k would be the other way to make
	 * the handshake succeed and is not on the table for a request carrying a
	 * token. */
	if (g_ca[0]) { fprintf(f, "cacert = "); cfg_quote(f, g_ca); fputc('\n', f); }
	/* RA refuses curl's default agent with `unsupported_client`, which reads
	 * like a permissions problem and is not one. */
	fprintf(f, "user-agent = "); cfg_quote(f, NET_UA); fputc('\n', f);

	for (i = 0; i < n; i++) {
		char kv[1024];
		snprintf(kv, sizeof kv, "%s=%s", fl[i].k, fl[i].v ? fl[i].v : "");
		fprintf(f, "data-urlencode = ");
		cfg_quote(f, kv);
		fputc('\n', f);
	}
	if (get) fprintf(f, "get\n");
	fprintf(f, "url = "); cfg_quote(f, url); fputc('\n', f);
	fclose(f);
	return true;
}

/* Run curl with stdout on `out_fd`. Returns its exit status, or -1. */
static int run_curl(const char *cfg, int out_fd)
{
	const char *bin = curl_bin();
	pid_t pid;
	int st = -1;

	if (!bin) {
		fprintf(stderr, "ra: no curl on this device\n");
		return -1;
	}

	pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		char *argv[4];
		int devnull = open("/dev/null", O_RDONLY);

		if (devnull >= 0) { dup2(devnull, 0); close(devnull); }
		dup2(out_fd, 1);
		argv[0] = (char *)bin;
		argv[1] = (char *)"-K";
		argv[2] = (char *)cfg;
		argv[3] = NULL;
		execv(bin, argv);
		_exit(127);
	}
	if (waitpid(pid, &st, 0) != pid) return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* A path no other call can be using: per process AND per call.
 *
 * It was per process alone - "/tmp/tortos-ra-<pid>.curl" - which every thread
 * in the launcher shares, and the response goes beside it as ".body". That held
 * only while every synchronous request ran on the main thread, one after
 * another. The moment two overlap, a sign-in on the main thread and an unlock
 * sent from a worker, both write the one config and read the one body, and each
 * gets the other's credentials, or the other's answer, or half of both. Nothing
 * would crash; a request would simply be refused or misread. The counter makes
 * every call its own pair of files. */
static void tmp_config(char *out, size_t n)
{
	static atomic_uint seq;

	snprintf(out, n, "/tmp/tortos-ra-%ld-%u.curl", (long)getpid(),
	         (unsigned)atomic_fetch_add(&seq, 1));
}

/* The two blocking calls differ only in where they go and how the fields
 * travel, so they are one body with the config written two ways. */
static long request_buf(const char *url, bool get, const net_field *f, int n,
                        char *out, size_t outn, int timeout_s)
{
	char cfg[128], tmp[160];
	int fd, rc;
	long got = -1;

	if (!out || outn == 0) return -1;
	out[0] = '\0';
	tmp_config(cfg, sizeof cfg);
	snprintf(tmp, sizeof tmp, "%s.body", cfg);
	if (!write_config(cfg, url, get, f, n, timeout_s)) return -1;

	fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) { unlink(cfg); return -1; }

	rc = run_curl(cfg, fd);
	unlink(cfg);
	if (rc == 0) {
		ssize_t r;
		lseek(fd, 0, SEEK_SET);
		r = read(fd, out, outn - 1);
		if (r >= 0) { out[r] = '\0'; got = (long)r; }
	} else {
		fprintf(stderr, "net: request failed (curl exit %d)\n", rc);
	}
	close(fd);
	unlink(tmp);
	return got;
}

long net_get_buf(const char *url, const net_field *f, int n, char *out,
                 size_t outn, int timeout_s)
{
	return request_buf(url, true, f, n, out, outn, timeout_s);
}

long net_post_buf(const net_field *f, int n, char *out, size_t outn, int timeout_s)
{
	return request_buf(RA_URL, false, f, n, out, outn, timeout_s);
}

/* ---- the same thing, without waiting ------------------------------------ */

static pid_t g_async = -1;
static char  g_async_cfg[160];
/* When the request in flight started, and what the last one to finish took
 * and exited with. See net_async_ms. */
static long  g_async_t0;
static int   g_async_ms, g_async_exit;

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

bool net_post_async(const net_field *f, int n, const char *path, int timeout_s)
{
	const char *bin = curl_bin();
	int fd;

	if (g_async > 0 && net_async_poll() == 0) return false;
	if (!bin) return false;

	snprintf(g_async_cfg, sizeof g_async_cfg, "/tmp/tortos-ra-async-%ld.curl",
	         (long)getpid());
	if (!write_config(g_async_cfg, RA_URL, false, f, n, timeout_s)) return false;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) { unlink(g_async_cfg); return false; }

	g_async_t0 = now_ms();
	g_async = fork();
	if (g_async < 0) { close(fd); unlink(g_async_cfg); g_async = -1; return false; }
	if (g_async == 0) {
		char *argv[4];
		int devnull = open("/dev/null", O_RDONLY);

		if (devnull >= 0) { dup2(devnull, 0); close(devnull); }
		dup2(fd, 1);
		argv[0] = (char *)bin;
		argv[1] = (char *)"-K";
		argv[2] = g_async_cfg;
		argv[3] = NULL;
		execv(bin, argv);
		_exit(127);
	}
	close(fd);
	return true;
}

/* Where an async GET is being written, so the poll can put it in place. Empty
 * when the request in flight is a POST, which manages its own destination. */
static char g_async_dest[512];
static char g_async_part[520];
/* Where curl prints the status, and the last one it printed. Kept apart from
 * the body so that a caller can ask what happened even when there is no body
 * to read - which is every refusal, since curl runs with `fail`. */
static char g_async_st[560];
static int  g_async_http;

void net_async_abort(void)
{
	if (g_async > 0) {
		kill(g_async, SIGKILL);
		waitpid(g_async, NULL, 0);
	}
	g_async = -1;
	if (g_async_cfg[0]) unlink(g_async_cfg);
	if (g_async_part[0]) unlink(g_async_part);
	if (g_async_st[0]) { unlink(g_async_st); g_async_st[0] = '\0'; }
	g_async_dest[0] = '\0';
	g_async_part[0] = '\0';
}

bool net_get_async(const char *url, const char *path, int timeout_s)
{
	const char *bin = curl_bin();
	int fd;

	/* A slot left set by a caller that walked away is cleared here rather
	 * than refused forever.
	 *
	 * This deadlocked once, and silently: a screen closed mid-fetch left
	 * g_async pointing at a pid nobody would ever wait for. Every later
	 * request then returned false - so no WAIT phase was entered, so
	 * net_async_poll was never called, so the slot was never cleared. Box art
	 * reported "could not start curl" on every system for the rest of the
	 * launcher's life, with curl sitting right there working. Only a restart
	 * fixed it, which is the shape of a bug nobody reports accurately. */
	if (g_async > 0 && net_async_poll() == 0) return false;   /* genuinely busy */
	if (!bin) return false;

	snprintf(g_async_cfg, sizeof g_async_cfg, "/tmp/tortos-get-async-%ld.curl",
	         (long)getpid());
	snprintf(g_async_dest, sizeof g_async_dest, "%s", path);
	if (snprintf(g_async_part, sizeof g_async_part, "%s.part", path)
	    >= (int)sizeof g_async_part) {
		g_async_dest[0] = '\0';
		return false;
	}
	if (!write_get_config(g_async_cfg, url, g_async_part, timeout_s)) {
		g_async_dest[0] = '\0';
		return false;
	}
	snprintf(g_async_st, sizeof g_async_st, "%s.http", g_async_part);

	g_async_http = 0;
	fd = open(g_async_st, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) { unlink(g_async_cfg); g_async_dest[0] = '\0'; return false; }

	g_async_t0 = now_ms();
	g_async = fork();
	if (g_async < 0) {
		close(fd);
		unlink(g_async_cfg);
		unlink(g_async_part);
		g_async = -1;
		g_async_dest[0] = '\0';
		return false;
	}
	if (g_async == 0) {
		char *argv[4];
		int devnull = open("/dev/null", O_RDONLY);

		if (devnull >= 0) { dup2(devnull, 0); close(devnull); }
		dup2(fd, 1);
		argv[0] = (char *)bin;
		argv[1] = (char *)"-K";
		argv[2] = g_async_cfg;
		argv[3] = NULL;
		execv(bin, argv);
		_exit(127);
	}
	close(fd);
	return true;
}

int net_async_poll(void)
{
	int st;
	pid_t r;
	bool ok;

	if (g_async <= 0) return -1;
	r = waitpid(g_async, &st, WNOHANG);
	if (r == 0) return 0;                      /* still going */

	g_async = -1;
	unlink(g_async_cfg);
	ok = (r > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0);
	g_async_ms = (int)(now_ms() - g_async_t0);
	g_async_exit = (r > 0 && WIFEXITED(st)) ? WEXITSTATUS(st) : -1;

	g_async_http = 0;
	if (g_async_st[0]) {
		FILE *h = fopen(g_async_st, "r");

		if (h) {
			if (fscanf(h, "%d", &g_async_http) != 1) g_async_http = 0;
			fclose(h);
		}
		unlink(g_async_st);
		g_async_st[0] = '\0';
	}

	/* A GET renames into place here rather than in the child, because only
	 * the parent knows the request succeeded. curl -f exits non-zero on an
	 * HTTP error but has usually already written the error body. */
	if (g_async_dest[0]) {
		if (ok && rename(g_async_part, g_async_dest) != 0) ok = false;
		if (!ok) unlink(g_async_part);
		g_async_dest[0] = '\0';
	}
	return ok ? 1 : -1;
}

int net_async_http(void)
{
	return g_async_http;
}

int net_async_ms(void)   { return g_async_ms; }
int net_async_exit(void) { return g_async_exit; }

bool net_online(void)
{
	struct ifaddrs *ifa, *p;
	bool up = false;

	if (!curl_bin()) return false;
	if (getifaddrs(&ifa) != 0) return false;
	for (p = ifa; p; p = p->ifa_next) {
		if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
		if (!(p->ifa_flags & IFF_UP)) continue;
		if (p->ifa_flags & IFF_LOOPBACK) continue;
		up = true;
		break;
	}
	freeifaddrs(ifa);
	return up;
}
