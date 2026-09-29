/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* Does the server read what a client actually sends?
 *
 *     make check-httpd
 *
 * An HTTP parser is where "looks right" and "is right" part company, because
 * every browser on earth sends the well-formed case and only the interesting
 * failures send anything else. So this drives the real server over a real
 * socket with bytes it chooses, in one process: the server is non-blocking, so
 * the client and the poll loop take turns without threads.
 *
 * What is worth checking here is the seams, not the happy path:
 *
 *   - a request split across two packets, which is the normal case on a slow
 *     network and the one a parser that assumes "one read, one request" fails
 *   - a body arriving in the SAME read as the headers, which is what every
 *     upload does and what a parser that discards the tail of its buffer loses
 *   - two requests pipelined into one packet, where the second must not be
 *     read as the tail of the first
 *   - a short write on the way out, so a download resumes where it stopped
 *   - headers bigger than the buffer, which must be refused rather than
 *     overflow it
 *   - the end of a request, reported once and as what it was, whole or cut
 *     off, so a screen showing a transfer knows when to stop showing it
 *
 * No device, no browser. One process, two sockets, and bytes chosen by hand.
 */
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "../src/httpd.h"

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

/* ---- a handler with just enough behavior to check the transport ------ */

static char g_sink[256];        /* where the last upload was told to go */
static int  g_calls;            /* how many times the handler was entered */

static void handler(httpd_req *r, bool done, void *ctx)
{
	const char *m = httpd_method(r);
	const char *p = httpd_path(r);

	(void)ctx;
	g_calls++;

	if (!done) {
		/* First call: decide what happens to the body. */
		if (!strcmp(m, "PUT")) {
			httpd_want_body(r, HTTPD_BODY_FILE, g_sink);
		} else if (!strcmp(m, "POST")) {
			if (httpd_content_len(r) > HTTPD_MEM_MAX) {
				httpd_reply_status(r, 413, "too big");
				return;
			}
			httpd_want_body(r, HTTPD_BODY_MEM, NULL);
		} else {
			httpd_want_body(r, HTTPD_BODY_NONE, NULL);
		}
		if (httpd_content_len(r) > 0) return;   /* wait for the body */
	}

	if (!strcmp(p, "/echo")) {
		char q[128];
		httpd_query(r, "v", q, sizeof q);
		httpd_reply(r, 200, "text/plain", q, strlen(q), NULL);
	} else if (!strcmp(p, "/body")) {
		size_t n;
		const char *b = httpd_body(r, &n);
		httpd_reply(r, 200, "text/plain", b, n, NULL);
	} else if (!strcmp(p, "/hdr")) {
		const char *h = httpd_header(r, "X-Thing");
		httpd_reply(r, 200, "text/plain", h ? h : "(none)",
		            strlen(h ? h : "(none)"), NULL);
	} else if (!strcmp(p, "/put")) {
		char n[32];
		snprintf(n, sizeof n, "%ld", httpd_content_len(r));
		httpd_reply(r, 200, "text/plain", n, strlen(n), NULL);
	} else if (!strcmp(p, "/file")) {
		httpd_reply_file(r, (const char *)ctx, "application/octet-stream", NULL);
	} else {
		httpd_reply_status(r, 404, "no");
	}
}

static int  g_ends;             /* how many times a request's end was reported */
static int  g_end_whole = -1;   /* what the last report said */

static void on_end(httpd_req *r, bool whole, void *ctx)
{
	(void)r;
	(void)ctx;
	g_ends++;
	g_end_whole = whole;
}

/* ---- a server thread, and an ordinary client ------------------------- */

/* The first version of this file ran the server and the client in one thread,
 * taking turns: the client called httpd_poll between its own reads and writes.
 * It was flaky at about five per cent, in a different place each run, and the
 * design was why. Taking turns means the server only advances where the client
 * remembered to let it - so every path the client did not think to spin was a
 * stall, and every stall looked like the server losing a request.
 *
 * A thread instead. The server runs the loop it actually runs on the device,
 * the client does blocking socket I/O like a browser, and neither has to know
 * anything about the other's scheduling. */

static pthread_t     g_thread;
static volatile int  g_stop;
static void         *g_ctx;

static void *server_loop(void *unused)
{
	(void)unused;
	while (!g_stop) {
		httpd_poll(handler, on_end, g_ctx);
		usleep(200);
	}
	return NULL;
}

/* A fresh server for each block, so nothing depends on what the last one left
 * open. HTTP/1.1 keeps connections alive and the client closing its end only
 * frees the slot once the FIN is read, so shared state here means a block can
 * start against a server with slots already taken. */
static void fresh(void *ctx)
{
	int p0 = 0;

	if (g_thread) { g_stop = 1; pthread_join(g_thread, NULL); g_thread = 0; }
	httpd_stop();
	g_ctx = ctx;
	g_stop = 0;
	g_ends = 0;
	g_end_whole = -1;
	if (!httpd_start(&p0, 1)) {
		printf("  FAIL: could not rebind\n");
		failures++;
		return;
	}
	pthread_create(&g_thread, NULL, server_loop, NULL);
}

static void shutdown_server(void)
{
	if (g_thread) { g_stop = 1; pthread_join(g_thread, NULL); g_thread = 0; }
	httpd_stop();
}

static int cl_connect(void)
{
	struct sockaddr_in a;
	struct timeval tv = { 3, 0 };
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons((unsigned short)httpd_port());
	if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
		printf("  FAIL: could not connect: %s\n", strerror(errno));
		failures++;
		close(fd);
		return -1;
	}
	/* Bounded, so a bug here is a failing check rather than a hung one. */
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	return fd;
}

/* Every byte, or false. A short write silently dropping the tail is how a
 * 3000-byte upload arrives as 1400 and reads as the server losing a body. */
static bool cl_write(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	size_t sent = 0;

	while (sent < len) {
		ssize_t n = write(fd, p + sent, len - sent);

		if (n > 0) { sent += (size_t)n; continue; }
		if (n < 0 && (errno == EINTR)) continue;
		return false;
	}
	return true;
}

/* Is `out` `want` whole responses yet? Headers, then exactly Content-Length
 * bytes, for each. "Some bytes have arrived" and "the reply has arrived" are
 * different questions and only one of them is the one being asked. */
static bool complete(const char *out, size_t got, int want_bodies)
{
	const char *p = out;
	int seen = 0;

	for (;;) {
		const char *blank = strstr(p, "\r\n\r\n");
		const char *cl;
		long len;

		if (!blank) return false;
		cl = strstr(p, "Content-Length: ");
		if (!cl || cl > blank) return false;
		len = strtol(cl + 16, NULL, 10);
		if ((size_t)(blank + 4 - out) + (size_t)len > got) return false;
		if (++seen >= want_bodies) return true;
		p = blank + 4 + len;
	}
}

static size_t talk_n(int fd, const char *req, size_t reqlen,
                     char *out, size_t outn, int want)
{
	size_t got = 0;

	if (reqlen && !cl_write(fd, req, reqlen)) return 0;
	while (got + 1 < outn) {
		ssize_t n = recv(fd, out + got, outn - got - 1, 0);

		if (n > 0) {
			got += (size_t)n;
			out[got] = '\0';
			if (complete(out, got, want)) break;
			continue;
		}
		break;                        /* closed, or the read timed out */
	}
	out[got] = '\0';
	return got;
}

static size_t talk(int fd, const char *req, size_t reqlen, char *out, size_t outn)
{
	return talk_n(fd, req, reqlen, out, outn, 1);
}

/* connect() returning does not mean the server has accepted - that happens in
 * httpd_poll, on the server thread's schedule. */
static void wait_conns(int n)
{
	int i;

	for (i = 0; i < 600 && httpd_conn_count() < n; i++) usleep(500);
}

static void settle(void)
{
	int i;

	for (i = 0; i < 600 && httpd_conn_count() > 0; i++) usleep(500);
}

/* The body of a response, after the blank line. */
static const char *body_of(const char *resp)
{
	const char *b = strstr(resp, "\r\n\r\n");
	return b ? b + 4 : "";
}

static int status_of(const char *resp)
{
	int s = 0;
	if (sscanf(resp, "HTTP/1.1 %d", &s) != 1) return 0;
	return s;
}

int main(void)
{
	char resp[16384];
	char tmpf[256], tmpsink[256];
	FILE *f;

	snprintf(tmpf,    sizeof tmpf,    "/tmp/tortos-httpd-%ld.bin", (long)getpid());
	snprintf(tmpsink, sizeof tmpsink, "/tmp/tortos-httpd-%ld.up",  (long)getpid());
	snprintf(g_sink,  sizeof g_sink,  "%s", tmpsink);

	printf("httpd: what a client actually sends\n");

	fresh(tmpf);
	if (!httpd_running()) { printf("\ncannot continue\n"); return 1; }
	CHECK(httpd_port() > 0, "port 0 was reported back instead of the real one");

	printf("  a plain request, in one packet:\n");
	fresh(tmpf);
	{
		int fd = cl_connect();
		const char *req = "GET /echo?v=hello HTTP/1.1\r\nHost: x\r\n\r\n";

		talk(fd, req, strlen(req), resp, sizeof resp);
		CHECK(status_of(resp) == 200, "one packet: status was %d", status_of(resp));
		CHECK(!strcmp(body_of(resp), "hello"), "one packet: body was \"%s\"",
		      body_of(resp));
		CHECK(strstr(resp, "Content-Length: 5") != NULL,
		      "no or wrong Content-Length");
		close(fd);
		settle();
	}

	printf("  a request split across two packets:\n");
	fresh(tmpf);
	{
		int fd = cl_connect();
		/* The split lands INSIDE the header block, which is what a parser
		 * that treats one read as one request gets wrong. */
		const char *a = "GET /echo?v=split HTTP/1.1\r\nHo";
		const char *b = "st: x\r\n\r\n";

		cl_write(fd, a, strlen(a));
				talk(fd, b, strlen(b), resp, sizeof resp);
		CHECK(status_of(resp) == 200, "split headers: status was %d",
		      status_of(resp));
		CHECK(!strcmp(body_of(resp), "split"), "split headers: body was \"%s\"",
		      body_of(resp));
		close(fd);
		settle();
	}

	printf("  a body in the same packet as its headers:\n");
	fresh(tmpf);
	{
		int fd = cl_connect();
		const char *req = "POST /body HTTP/1.1\r\nHost: x\r\n"
		                  "Content-Length: 11\r\n\r\nhello world";

		talk(fd, req, strlen(req), resp, sizeof resp);
		CHECK(status_of(resp) == 200, "body with headers: status was %d",
		      status_of(resp));
		CHECK(!strcmp(body_of(resp), "hello world"),
		      "the body that arrived with the headers was lost: \"%s\"",
		      body_of(resp));
		close(fd);
		settle();
	}

	printf("  a body split away from its headers:\n");
	fresh(tmpf);
	{
		int fd = cl_connect();
		const char *h = "POST /body HTTP/1.1\r\nHost: x\r\nContent-Length: 9\r\n\r\n";

		cl_write(fd, h, strlen(h));
				talk(fd, "sometext!", 9, resp, sizeof resp);
		CHECK(!strcmp(body_of(resp), "sometext!"), "body was \"%s\"", body_of(resp));
		close(fd);
		settle();
	}

	printf("  two requests pipelined into one packet:\n");
	fresh(tmpf);
	{
		int fd = cl_connect();
		const char *req =
			"GET /echo?v=one HTTP/1.1\r\nHost: x\r\n\r\n"
			"GET /echo?v=two HTTP/1.1\r\nHost: x\r\n\r\n";

		talk_n(fd, req, strlen(req), resp, sizeof resp, 2);
		/* Both answers, in order, on one connection. A server that throws away
		 * the tail of its read buffer answers the first and hangs. */
		CHECK(strstr(resp, "one") != NULL, "the first answer is missing");
		CHECK(strstr(resp, "two") != NULL,
		      "the second request in the packet was dropped");
		close(fd);
		settle();
	}

	printf("  an upload, streamed to the card:\n");
	fresh(tmpf);
	{
		int fd = cl_connect();
		char req[1024];
		char payload[3000];
		size_t i;

		for (i = 0; i < sizeof payload; i++) payload[i] = (char)('a' + i % 26);
		snprintf(req, sizeof req,
		         "PUT /put HTTP/1.1\r\nHost: x\r\nContent-Length: %zu\r\n\r\n",
		         sizeof payload);
		cl_write(fd, req, strlen(req));
				talk(fd, payload, sizeof payload, resp, sizeof resp);
		CHECK(status_of(resp) == 200, "upload: status was %d", status_of(resp));
		{
			struct stat st;
			CHECK(stat(tmpsink, &st) == 0 && st.st_size == (off_t)sizeof payload,
			      "the upload landed as %ld bytes, wanted %zu",
			      (long)(stat(tmpsink, &st) == 0 ? st.st_size : -1),
			      sizeof payload);
		}
		close(fd);
		settle();
	}

	printf("  a download, streamed from the card:\n");
	fresh(tmpf);
	{
		int fd;
		/* Deliberately several times POLL_BUDGET, which is 256 KB.
		 *
		 * This was 200 KB, which is under it - so the download never crossed
		 * a budget boundary and the check passed over a server that corrupted
		 * every file bigger than 256 KB. A fixture smaller than the thing it
		 * is meant to stress is not a fixture. */
		size_t i, n = 900000;
		char *big = malloc(n);

		for (i = 0; i < n; i++) big[i] = (char)(i & 0xff);
		f = fopen(tmpf, "wb");
		CHECK(f != NULL, "could not write the fixture");
		if (f) { fwrite(big, 1, n, f); fclose(f); }

		fd = cl_connect();
		{
			char *buf = malloc(n + 4096);
			const char *req = "GET /file HTTP/1.1\r\nHost: x\r\n\r\n";
			size_t got = talk(fd, req, strlen(req), buf, n + 4096);
			const char *b = body_of(buf);

			CHECK(status_of(buf) == 200, "status was %d", status_of(buf));
			CHECK(got > n, "only %zu bytes came back, wanted more than %zu", got, n);
			CHECK(memcmp(b, big, n) == 0,
			      "the file came back different - a short write lost or "
			      "repeated a chunk");
			free(buf);
		}
		close(fd);
		settle();
		/* The server reports the end once the last byte is out, before it
		 * notices the hang-up, so a finished download never reads as cut. */
		CHECK(g_ends == 1 && g_end_whole == 1,
		      "a finished download reported its end %d time(s), whole=%d",
		      g_ends, g_end_whole);
		free(big);
	}

	printf("  headers larger than the buffer are refused, not overflowed:\n");
	fresh(tmpf);
	{
		int fd = cl_connect();
		char *req = malloc(32768);
		size_t used = (size_t)snprintf(req, 32768, "GET /echo HTTP/1.1\r\n");

		while (used < 30000)
			used += (size_t)snprintf(req + used, 32768 - used,
			                         "X-Pad-%04zu: aaaaaaaaaaaaaaaaaaaaaaaa\r\n",
			                         used);
		snprintf(req + used, 32768 - used, "\r\n");
		talk(fd, req, strlen(req), resp, sizeof resp);
		/* Refused or dropped, either is fine; answering 200 is not, and
		 * neither is this process still being alive by luck. */
		CHECK(status_of(resp) != 200,
		      "an oversized header block was accepted");
		free(req);
		close(fd);
		settle();
	}

	printf("  malformed requests get an answer, not a crash:\n");
	fresh(tmpf);
	{
		const char *bad[] = {
			"GET\r\n\r\n",                       /* no path, no version */
			"GET /echo\r\n\r\n",                 /* no version */
			"\r\n\r\n",                          /* nothing at all */
			"GET /echo HTTP/1.1\r\nContent-Length: -5\r\n\r\n",
			"GET /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
		};
		size_t i;

		for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
			int fd = cl_connect();
			int s;

			talk(fd, bad[i], strlen(bad[i]), resp, sizeof resp);
			s = status_of(resp);
			CHECK(s == 0 || s >= 400, "request %zu got %d, wanted a refusal",
			      i, s);
			close(fd);
			settle();
		}
	}

	printf("  a header the handler asks for by name:\n");
	fresh(tmpf);
	{
		int fd = cl_connect();
		const char *req = "GET /hdr HTTP/1.1\r\nHost: x\r\n"
		                  "X-Other: no\r\nX-Thing: yes please\r\n\r\n";

		talk(fd, req, strlen(req), resp, sizeof resp);
		CHECK(!strcmp(body_of(resp), "yes please"),
		      "header lookup gave \"%s\"", body_of(resp));
		close(fd);
		settle();
	}

	printf("  a client that vanishes mid-request frees its slot:\n");
	fresh(tmpf);
	settle();
	{
		int i, fd[4];

		for (i = 0; i < 4; i++) {
			fd[i] = cl_connect();
			cl_write(fd[i], "GET /echo HTTP/1.1\r\n", 20);
		}
		wait_conns(4);
		CHECK(httpd_conn_count() == 4, "expected 4 open, got %d",
		      httpd_conn_count());
		for (i = 0; i < 4; i++) close(fd[i]);
		/* settle, not a fixed spin count: the FIN arrives when the kernel
		 * says so, and a spin budget that is usually enough is exactly the
		 * kind of check that fails once in forty runs. */
		settle();
		CHECK(httpd_conn_count() == 0,
		      "%d connection(s) survived the client hanging up",
		      httpd_conn_count());
	}

	/* The half-written file, and why it is not merely untidy: a part-file is
	 * filtered out of listings, so its folder reads as empty while rmdir keeps
	 * answering "that folder is not empty" and nothing in the interface names
	 * the thing in the way. Dropping a lid mid-transfer is the ordinary way to
	 * get one. */
	printf("  an upload that dies halfway leaves no part-file:\n");
	remove(tmpsink);
	{
		int fd = cl_connect();
		char req[256], sent[1500];
		struct stat st;
		size_t i;

		for (i = 0; i < sizeof sent; i++) sent[i] = (char)('a' + i % 26);
		snprintf(req, sizeof req,
		         "PUT /put HTTP/1.1\r\nHost: x\r\nContent-Length: 100000\r\n\r\n");
		cl_write(fd, req, strlen(req));
		cl_write(fd, sent, sizeof sent);
		/* Give the server thread time to open the sink and write what came,
		 * with the connection still up. */
		usleep(150000);
		/* Without this the rest is vacuous: a check that the file is gone
		 * passes just as well when it was never created. */
		CHECK(stat(tmpsink, &st) == 0,
		      "the part-file was never created, so the next check proves nothing");
		close(fd);
		settle();
		CHECK(stat(tmpsink, &st) != 0,
		      "a part-file survived a client that hung up mid-upload");
		/* Without this the transfer screen went on reading "receiving". */
		CHECK(g_ends == 1 && g_end_whole == 0,
		      "an upload cut off halfway reported its end %d time(s), whole=%d",
		      g_ends, g_end_whole);
	}

	shutdown_server();
	CHECK(!httpd_running(), "the server is still listening after stop");
	remove(tmpf);
	remove(tmpsink);

	if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
	printf("\nok: every check passed\n");
	return 0;
}
