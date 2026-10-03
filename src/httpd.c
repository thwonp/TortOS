/* SPDX-License-Identifier: MIT */
/* See httpd.h for why none of this is allowed to block. */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <time.h>

#include "httpd.h"
#include "xfer.h"

#define MAX_CONN        8
#define HEAD_MAX     8192      /* request line and headers together */
#define RESP_HEAD_MAX 1024
#define CHUNK        (64 * 1024)

/* Per connection per poll. Bounds a frame: the launcher's loop is the power
 * button's watchdog, so a fast client must not be able to hold this function
 * for as long as it can keep the socket readable. 256 KB across eight
 * connections at 120 Hz is far more than the radio will ever deliver. */
#define POLL_BUDGET  (256 * 1024)

/* A connection that has said nothing for this long is gone, whatever it
 * claimed it was going to send. Without it a browser tab left open on a dead
 * network holds a slot until the screen is closed. */
#define IDLE_MS      20000

typedef enum {
	C_FREE, C_HEAD, C_BODY, C_RESP, C_FILE, C_DRAIN
} conn_state;

struct httpd_req {
	/* --- request ---------------------------------------------------- */
	char  method[8];
	char  path[512];
	char  query[512];
	char  head[HEAD_MAX];       /* headers, NUL-separated lines, after parsing */
	long  content_len;

	/* --- what the handler asked for ----------------------------------- */
	httpd_body_mode mode;
	char  sink[XFER_PATH_MAX];
	FILE *sink_f;
	char *mem;                  /* HTTPD_BODY_MEM, malloc'd */
	size_t mem_len;
	long  body_got;

	/* --- response ------------------------------------------------------ */
	int    status;
	char   resp_head[RESP_HEAD_MAX];
	size_t resp_head_len, resp_head_sent;
	char  *resp_body;
	size_t resp_body_len, resp_body_sent;
	FILE  *resp_f;              /* streaming a file out */
	long   resp_f_left;
	bool   replied;

	/* --- bookkeeping --------------------------------------------------- */
	int    tag;                 /* the handler's own, see httpd_set_tag */
	bool   seen;                /* the handler has been called for it */
	bool   ended;               /* httpd_end has been told */
};

typedef struct {
	int         fd;
	conn_state  st;
	/* Unconsumed bytes from the socket. On the CONNECTION, not the request,
	 * and that is the whole of what went wrong first time: two requests
	 * pipelined into one packet put the second one in here behind the first,
	 * and a buffer owned by the request was freed with the request - so the
	 * second was read, discarded, and waited for forever by both ends. */
	char        buf[HEAD_MAX + 1];
	size_t      used;
	unsigned    last_ms;
	bool        close_after;
	httpd_req   r;
} conn;

/* Its own clock rather than the launcher's. The timeouts here are about
 * sockets and have nothing to do with frames, and taking plat_now_ms would
 * drag SDL into any check that wanted to drive this with a real client. */
static unsigned now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned)(ts.tv_sec * 1000u + (unsigned)(ts.tv_nsec / 1000000));
}

static int  g_listen = -1;
static int  g_port;
static conn g_conn[MAX_CONN];
static unsigned long g_in, g_out;

/* ---- small helpers -------------------------------------------------- */

static void set_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	fcntl(fd, F_SETFD, FD_CLOEXEC);
}

static int ci_cmp(const char *a, const char *b, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++) {
		int x = a[i], y = b[i];
		if (x >= 'A' && x <= 'Z') x += 32;
		if (y >= 'A' && y <= 'Z') y += 32;
		if (x != y) return x - y;
		if (!x) break;
	}
	return 0;
}

static const char *status_text(int s)
{
	switch (s) {
	case 200: return "OK";
	case 204: return "No Content";
	case 400: return "Bad Request";
	case 401: return "Unauthorized";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 409: return "Conflict";
	case 413: return "Payload Too Large";
	case 429: return "Too Many Requests";
	case 500: return "Internal Server Error";
	case 507: return "Insufficient Storage";
	default:  return "Error";
	}
}

/* ---- reading a request ---------------------------------------------- */

const char *httpd_method(const httpd_req *r) { return r->method; }
const char *httpd_path(const httpd_req *r)   { return r->path; }
long httpd_content_len(const httpd_req *r)   { return r->content_len; }
void httpd_set_tag(httpd_req *r, int tag)    { r->tag = tag; }
int  httpd_tag(const httpd_req *r)           { return r->tag; }

const char *httpd_body(const httpd_req *r, size_t *len)
{
	if (len) *len = r->mem_len;
	return r->mem ? r->mem : "";
}

const char *httpd_header(const httpd_req *r, const char *name)
{
	size_t n = strlen(name);
	const char *p = r->head;

	/* The stored block starts at the first header line, so every line is a
	 * candidate and none of them is the request line. */
	while (*p) {
		if (!ci_cmp(p, name, n) && p[n] == ':') {
			p += n + 1;
			while (*p == ' ' || *p == '\t') p++;
			return p;
		}
		while (*p) p++;
		p++;                       /* lines are NUL-separated in place */
	}
	return NULL;
}

/* One key out of the query string, percent-decoding left to the caller: this
 * returns raw bytes and xfer_resolve is the only thing allowed to decode a
 * path. Values that are not paths - a PIN, a rename target - are decoded by
 * their own route. */
const char *httpd_query(const httpd_req *r, const char *key, char *out,
                        size_t outn)
{
	size_t n = strlen(key);
	const char *p = r->query;

	out[0] = '\0';
	while (*p) {
		const char *amp = strchr(p, '&');
		size_t seglen = amp ? (size_t)(amp - p) : strlen(p);

		if (seglen > n && !strncmp(p, key, n) && p[n] == '=') {
			size_t vlen = seglen - n - 1;
			if (vlen >= outn) vlen = outn - 1;
			memcpy(out, p + n + 1, vlen);
			out[vlen] = '\0';
			return out;
		}
		if (!amp) break;
		p = amp + 1;
	}
	return out;
}

void httpd_want_body(httpd_req *r, httpd_body_mode mode, const char *sink_path)
{
	r->mode = mode;
	if (mode == HTTPD_BODY_FILE && sink_path)
		snprintf(r->sink, sizeof r->sink, "%s", sink_path);
}

/* ---- writing a response --------------------------------------------- */

static void build_head(httpd_req *r, int status, const char *type,
                       long len, const char *extra)
{
	r->status = status;
	r->resp_head_len = (size_t)snprintf(r->resp_head, sizeof r->resp_head,
		"HTTP/1.1 %d %s\r\n"
		"Content-Length: %ld\r\n"
		"Content-Type: %s\r\n"
		/* No caching, ever. The listing changes under the page's feet as
		 * files arrive, and a phone that caches a directory listing shows a
		 * file that is no longer there. */
		"Cache-Control: no-store\r\n"
		/* The page and the API are the same origin and nothing else should be
		 * calling this, so say so rather than leaving it to chance. */
		"X-Content-Type-Options: nosniff\r\n"
		"%s"
		"\r\n",
		status, status_text(status), len, type, extra ? extra : "");
	if (r->resp_head_len >= sizeof r->resp_head) {
		/* An oversized header block is a bug in a route, not something a
		 * client can cause, but truncating it would emit a malformed reply
		 * rather than an honest failure. */
		r->resp_head_len = (size_t)snprintf(r->resp_head, sizeof r->resp_head,
			"HTTP/1.1 500 %s\r\nContent-Length: 0\r\n\r\n", status_text(500));
	}
	r->resp_head_sent = 0;
	r->replied = true;
}

void httpd_reply(httpd_req *r, int status, const char *content_type,
                 const void *body, size_t len, const char *extra_headers)
{
	if (r->replied) return;
	if (len) {
		r->resp_body = malloc(len);
		if (!r->resp_body) { httpd_reply_status(r, 500, "out of memory"); return; }
		memcpy(r->resp_body, body, len);
		r->resp_body_len = len;
	}
	build_head(r, status, content_type ? content_type : "application/octet-stream",
	           (long)len, extra_headers);
}

void httpd_reply_status(httpd_req *r, int status, const char *text)
{
	char buf[256];
	int n;

	if (r->replied) return;
	/* Plain text, not JSON: these are read by a person looking at a network
	 * tab as often as by the page, and the page treats any non-200 the same
	 * way regardless of what is in the body. */
	n = snprintf(buf, sizeof buf, "%d %s\n%s\n", status, status_text(status),
	             text ? text : "");
	if (n < 0) n = 0;
	if ((size_t)n >= sizeof buf) n = (int)sizeof buf - 1;
	r->resp_body = malloc((size_t)n);
	if (r->resp_body) {
		memcpy(r->resp_body, buf, (size_t)n);
		r->resp_body_len = (size_t)n;
	} else {
		n = 0;
	}
	build_head(r, status, "text/plain; charset=utf-8", n, NULL);
}

bool httpd_reply_file(httpd_req *r, const char *path, const char *content_type,
                      const char *extra_headers)
{
	struct stat st;

	if (r->replied) return false;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
		httpd_reply_status(r, 404, "no such file");
		return false;
	}
	r->resp_f = fopen(path, "rb");
	if (!r->resp_f) { httpd_reply_status(r, 403, "cannot read that"); return false; }
	r->resp_f_left = (long)st.st_size;
	build_head(r, 200, content_type ? content_type : "application/octet-stream",
	           (long)st.st_size, extra_headers);
	return true;
}

/* ---- the connection state machine ----------------------------------- */

static void req_reset(httpd_req *r)
{
	if (r->sink_f) {
		fclose(r->sink_f);
		/* And take the part-file with it.
		 *
		 * A transfer that dies halfway - a closed lid, a dropped connection,
		 * the idle timeout - used to leave "name.part" on the card forever.
		 * It is filtered out of listings, so the folder reads as empty while
		 * rmdir keeps answering "that folder is not empty", and there is no
		 * name in the interface for the thing standing in the way. The route
		 * renames the part-file into place on success, so by the time a
		 * completed request gets here this path is already gone and the
		 * remove is a no-op. */
		remove(r->sink);
	}
	if (r->resp_f)   fclose(r->resp_f);
	free(r->mem);
	free(r->resp_body);
	memset(r, 0, sizeof *r);
}

static void conn_close(conn *c)
{
	/* Release what the request owns BEFORE the slot reads as free.
	 *
	 * These were the other way round, so `fd = -1` published an idle slot
	 * while req_reset had yet to unlink the part-file - and anything watching
	 * the connection count to decide a transfer was over could see the slot
	 * free and the half-file still on the card. It is a small window and it
	 * lost about one race in eight. "This connection is finished" should not
	 * become true before it is. */
	if (c->fd >= 0) close(c->fd);
	req_reset(&c->r);
	c->fd = -1;
	c->st = C_FREE;
}

/* Split the header block in place into NUL-separated lines, and pull out what
 * the rest of this file needs. False means the request is malformed, which is
 * answered with 400 rather than by guessing. */
static bool parse_head(conn *c, char *block)
{
	httpd_req *r = &c->r;
	char *line, *next, *sp, *q;
	const char *cl, *conn_h;

	/* Request line. */
	line = block;
	next = strstr(line, "\r\n");
	if (!next) return false;
	*next = '\0';
	next += 2;

	sp = strchr(line, ' ');
	if (!sp) return false;
	*sp = '\0';
	if (strlen(line) >= sizeof r->method) return false;
	snprintf(r->method, sizeof r->method, "%s", line);

	line = sp + 1;
	sp = strchr(line, ' ');
	if (!sp) return false;                     /* HTTP/0.9 is not a thing here */
	*sp = '\0';
	q = strchr(line, '?');
	if (q) {
		*q = '\0';
		snprintf(r->query, sizeof r->query, "%s", q + 1);
	}
	if (strlen(line) >= sizeof r->path) return false;
	snprintf(r->path, sizeof r->path, "%s", line);

	/* Headers, copied into r->head as NUL-separated lines so httpd_header can
	 * walk them without another parse. */
	{
		size_t used = 0;

		while (*next) {
			char *eol = strstr(next, "\r\n");
			size_t n;

			if (!eol) return false;
			if (eol == next) break;            /* the blank line: headers end */
			n = (size_t)(eol - next);
			if (used + n + 2 >= sizeof r->head) return false;
			memcpy(r->head + used, next, n);
			used += n;
			r->head[used++] = '\0';
			next = eol + 2;
		}
		r->head[used] = '\0';                  /* the terminating empty line */
	}

	cl = httpd_header(r, "Content-Length");
	r->content_len = cl ? strtol(cl, NULL, 10) : 0;
	if (r->content_len < 0) return false;

	/* HTTP/1.1 keeps the connection open unless told otherwise. A client that
	 * asks to close gets closed; so does one that sent a body we could not
	 * account for, because the next request would start mid-body. */
	conn_h = httpd_header(r, "Connection");
	c->close_after = conn_h && !ci_cmp(conn_h, "close", 5);
	if (httpd_header(r, "Transfer-Encoding")) return false;   /* no chunked in */
	return true;
}

/* Everything the handler asked to keep. False means the connection is done
 * for - the disk refused, or the client sent more than it said. */
static bool sink_bytes(conn *c, const char *buf, size_t n)
{
	httpd_req *r = &c->r;

	if (r->mode == HTTPD_BODY_MEM) {
		if (r->mem_len + n > HTTPD_MEM_MAX) return false;
		r->mem = realloc(r->mem, r->mem_len + n + 1);
		if (!r->mem) return false;
		memcpy(r->mem + r->mem_len, buf, n);
		r->mem_len += n;
		r->mem[r->mem_len] = '\0';
		return true;
	}
	if (r->mode == HTTPD_BODY_FILE) {
		if (!r->sink_f) {
			r->sink_f = fopen(r->sink, "wb");
			if (!r->sink_f) return false;
		}
		if (fwrite(buf, 1, n, r->sink_f) != n) return false;
		return true;
	}
	return true;                               /* HTTPD_BODY_NONE: discarded */
}

static void begin_response(conn *c)
{
	httpd_req *r = &c->r;

	if (r->sink_f) { fclose(r->sink_f); r->sink_f = NULL; }
	if (!r->replied) httpd_reply_status(r, 500, "the route set no reply");
	c->st = C_RESP;
}

/* Everything already in c->buf, without touching the socket.
 *
 * Separate from reading precisely because a packet is not a request: it can
 * hold two of them, or a request and the front of the next one, or half of
 * one. So bytes arrive in one place and are consumed in another, and the
 * consumer runs until it needs more rather than once per read.
 *
 * False closes the connection. */
static bool consume(conn *c, httpd_handler fn, void *ctx)
{
	for (;;) {
		if (c->st == C_HEAD) {
			char  *end;
			size_t head_bytes;

			c->buf[c->used] = '\0';
			end = strstr(c->buf, "\r\n\r\n");
			if (!end) {
				/* Not yet - unless it never will be. */
				if (c->used >= sizeof c->buf - 1) return false;
				return true;
			}
			head_bytes = (size_t)(end - c->buf) + 4;
			c->buf[head_bytes - 2] = '\0';       /* keep the final CRLF pair */

			if (!parse_head(c, c->buf)) {
				httpd_reply_status(&c->r, 400, "could not read that request");
				c->close_after = true;
				c->st = C_RESP;
				return true;
			}
			memmove(c->buf, c->buf + head_bytes, c->used - head_bytes);
			c->used -= head_bytes;

			/* First call: the handler says what to do with the body, and may
			 * answer outright instead.
			 *
			 * Whether that ends the connection depends on whether a body is
			 * still coming. A GET has none, so replying here is simply a fast
			 * answer and the next request may already be in the buffer behind
			 * it. A refused PUT has one, and those bytes are about to arrive
			 * with nothing willing to read them - after which this connection
			 * cannot find where the next request starts, so it closes.
			 *
			 * Conflating the two closed the connection after every GET, which
			 * looked exactly like the pipelining bug this consumer was written
			 * to fix and was in fact a second one hiding behind it. */
			c->r.seen = true;
			fn(&c->r, false, ctx);
			if (c->r.replied) {
				if (c->r.content_len > 0) c->close_after = true;
				c->st = C_RESP;
				return true;
			}
			c->st = C_BODY;
			continue;
		}

		if (c->st == C_BODY) {
			long   left = c->r.content_len - c->r.body_got;
			size_t use;

			if (left <= 0) {
				fn(&c->r, true, ctx);
				begin_response(c);
				return true;
			}
			if (c->used == 0) return true;        /* need more from the socket */
			use = c->used;
			if ((long)use > left) use = (size_t)left;
			if (!sink_bytes(c, c->buf, use)) {
				httpd_reply_status(&c->r, 507, "could not write that to the card");
				c->close_after = true;
				c->st = C_RESP;
				return true;
			}
			c->r.body_got += (long)use;
			memmove(c->buf, c->buf + use, c->used - use);
			c->used -= use;
			continue;
		}
		return true;
	}
}

/* Consume, then read, then consume again, up to the budget. */
static bool pump_read(conn *c, httpd_handler fn, void *ctx, int *did)
{
	size_t budget = POLL_BUDGET;

	for (;;) {
		ssize_t got;
		size_t  want;

		if (!consume(c, fn, ctx)) return false;
		if (c->st != C_HEAD && c->st != C_BODY) return true;
		if (budget == 0) return true;

		want = sizeof c->buf - 1 - c->used;
		if (want == 0) return false;              /* headers past the cap */
		if (want > budget) want = budget;

		got = recv(c->fd, c->buf + c->used, want, 0);
		if (got == 0) return false;               /* the client hung up */
		if (got < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
			if (errno == EINTR) continue;
			return false;
		}
		*did = 1;
		g_in += (unsigned long)got;
		budget -= (size_t)got;
		c->used += (size_t)got;
		c->last_ms = now_ms();
	}
}

/* Tell the caller a request is over. Once, and only for a request its handler
 * saw: one refused before it could be parsed was never the caller's. */
static void end_request(conn *c, bool whole, httpd_end end, void *ctx)
{
	if (end && c->r.seen && !c->r.ended) end(&c->r, whole, ctx);
	c->r.ended = true;
}

/* Write what is queued, up to the budget. Returns false to close. */
static bool pump_write(conn *c, httpd_end end, void *ctx, int *did)
{
	httpd_req *r = &c->r;
	size_t budget = POLL_BUDGET;

	while (budget > 0) {
		const char *src;
		size_t len;
		ssize_t put;
		char filebuf[CHUNK];

		if (r->resp_head_sent < r->resp_head_len) {
			src = r->resp_head + r->resp_head_sent;
			len = r->resp_head_len - r->resp_head_sent;
		} else if (r->resp_body && r->resp_body_sent < r->resp_body_len) {
			src = r->resp_body + r->resp_body_sent;
			len = r->resp_body_len - r->resp_body_sent;
		} else if (r->resp_f && r->resp_f_left > 0) {
			size_t want = sizeof filebuf;
			size_t got;

			if ((long)want > r->resp_f_left) want = (size_t)r->resp_f_left;
			/* Clamped to the budget BEFORE reading, not after.
			 *
			 * The clamp below applies to `len`, which is fine for the header
			 * and the in-memory body - those track how much has been sent and
			 * resume from there. It is not fine for a file: the read has
			 * already advanced the position and already charged resp_f_left,
			 * so sending less than was read skips the difference silently.
			 *
			 * Measured: a 40 MB download came back 196 bytes short and
			 * diverged at byte 261949, which is this budget minus a response
			 * header. Every file over 256 KB was quietly corrupt. The check
			 * did not catch it because its fixture was 200 KB - smaller than
			 * the boundary the bug lives on. */
			if (want > budget) want = budget;
			got = fread(filebuf, 1, want, r->resp_f);
			if (got == 0) return false;        /* the file shrank under us */
			src = filebuf;
			len = got;
			/* Charged before the write, and un-charged below by however much
			 * the socket refused, so a short write does not lose bytes. */
			r->resp_f_left -= (long)got;
		} else {
			/* Everything is out. Said before the close below, which would
			 * otherwise report this as cut off. */
			end_request(c, true, end, ctx);
			if (c->close_after) return false;
			/* The request is finished; the connection's buffer is not
			 * necessarily empty. Anything still in it is the next request,
			 * and httpd_poll consumes it without waiting for POLLIN. */
			req_reset(r);
			c->st = C_HEAD;
			return true;
		}

		if (len > budget) len = budget;
		put = send(c->fd, src, len, 0);
		if (put < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				if (src == filebuf) {
					/* Nothing went out: rewind both the counter and the file
					 * so this chunk is read again next time. */
					r->resp_f_left += (long)len;
					fseek(r->resp_f, -(long)len, SEEK_CUR);
				}
				return true;
			}
			if (errno == EINTR) {
				if (src == filebuf) {
					r->resp_f_left += (long)len;
					fseek(r->resp_f, -(long)len, SEEK_CUR);
				}
				continue;
			}
			return false;
		}
		*did = 1;
		g_out += (unsigned long)put;
		budget -= (size_t)put;
		c->last_ms = now_ms();

		if (r->resp_head_sent < r->resp_head_len) {
			r->resp_head_sent += (size_t)put;
		} else if (r->resp_body && r->resp_body_sent < r->resp_body_len) {
			r->resp_body_sent += (size_t)put;
		} else if (src == filebuf && (size_t)put < len) {
			/* Partly accepted: give back what did not go, and seek so the
			 * remainder is read again. */
			size_t back = len - (size_t)put;
			r->resp_f_left += (long)back;
			fseek(r->resp_f, -(long)back, SEEK_CUR);
		}
	}
	return true;
}

/* ---- the public half ------------------------------------------------- */

bool httpd_start(const int *ports, int nports)
{
	int i;

	if (g_listen >= 0) return true;
	for (i = 0; i < MAX_CONN; i++) { g_conn[i].fd = -1; g_conn[i].st = C_FREE; }

	for (i = 0; i < nports; i++) {
		struct sockaddr_in a;
		int on = 1;
		int fd = socket(AF_INET, SOCK_STREAM, 0);

		if (fd < 0) continue;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
		memset(&a, 0, sizeof a);
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_ANY);
		a.sin_port = htons((unsigned short)ports[i]);
		if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 ||
		    listen(fd, 8) != 0) {
			close(fd);
			continue;
		}
		set_nonblock(fd);
		g_listen = fd;
		g_port = ports[i];
		/* Port 0 means the kernel chooses, which is how a check gets a port
		 * without racing whatever else is on the machine. Ask what it picked
		 * rather than reporting the zero we asked for. */
		if (g_port == 0) {
			socklen_t sl = sizeof a;
			if (getsockname(fd, (struct sockaddr *)&a, &sl) == 0)
				g_port = ntohs(a.sin_port);
		}
		return true;
	}
	return false;
}

void httpd_stop(void)
{
	int i;

	for (i = 0; i < MAX_CONN; i++)
		if (g_conn[i].fd >= 0) conn_close(&g_conn[i]);
	if (g_listen >= 0) close(g_listen);
	g_listen = -1;
	g_port = 0;
}

bool httpd_running(void) { return g_listen >= 0; }
int  httpd_port(void)    { return g_port; }

void httpd_traffic(unsigned long *in, unsigned long *out)
{
	if (in)  { *in  = g_in;  g_in  = 0; }
	if (out) { *out = g_out; g_out = 0; }
}

int httpd_conn_count(void)
{
	int i, n = 0;
	for (i = 0; i < MAX_CONN; i++) if (g_conn[i].fd >= 0) n++;
	return n;
}

int httpd_poll(httpd_handler fn, httpd_end end, void *ctx)
{
	int i, did = 0;
	unsigned now = now_ms();

	if (g_listen < 0) return 0;

	/* Accept everything waiting, rather than one per frame: a browser opens
	 * the page and immediately asks for the stylesheet, the script and the
	 * font, and four frames of latency to start four transfers is four frames
	 * nobody needs to wait. */
	for (;;) {
		int fd = accept(g_listen, NULL, NULL);
		int slot, on = 1;

		if (fd < 0) break;
		for (slot = 0; slot < MAX_CONN; slot++)
			if (g_conn[slot].fd < 0) break;
		if (slot == MAX_CONN) { close(fd); break; }
		set_nonblock(fd);
		/* Nagle off. These replies are small and followed by silence, which
		 * is the exact case where waiting to coalesce adds a visible pause. */
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
		memset(&g_conn[slot], 0, sizeof g_conn[slot]);
		g_conn[slot].fd = fd;
		g_conn[slot].st = C_HEAD;
		g_conn[slot].last_ms = now;
		did = 1;
	}

	/* No readiness check on the connections themselves.
	 *
	 * Every socket here is non-blocking, so trying costs an EAGAIN rather
	 * than a wait - and gating on POLLIN was actively wrong: a second request
	 * pipelined into an earlier packet is already in the buffer, so it must be
	 * consumed on a frame where nothing new arrives and POLLIN never fires.
	 * A readiness test that answers "is there new data" cannot answer "is
	 * there work", and work is the question. */
	for (i = 0; i < MAX_CONN; i++) {
		conn *c = &g_conn[i];
		bool  live = true;

		if (c->fd < 0) continue;
		if (c->st == C_HEAD || c->st == C_BODY)
			live = pump_read(c, fn, ctx, &did);
		if (live && c->st == C_RESP)
			live = pump_write(c, end, ctx, &did);
		if (!live) {
			end_request(c, false, end, ctx);
			conn_close(c);
			continue;
		}

		/* A FRESH reading, not the `now` taken before this connection did its
		 * work. pump_read and pump_write stamp last_ms from their own call to
		 * now_ms(), which is necessarily later than the one at the top of this
		 * function - so `now - c->last_ms` on unsigned values underflows to
		 * about four billion and reads as twenty seconds of silence.
		 *
		 * It closed live connections at random: four clients mid-request, or a
		 * keep-alive connection between the first and second of two pipelined
		 * requests. Roughly one run in twenty, in a different place each time,
		 * which is exactly the shape that gets blamed on the test harness -
		 * and was, twice, before the server was traced instead of reasoned
		 * about. */
		if (now_ms() - c->last_ms > IDLE_MS) {
			end_request(c, false, end, ctx);
			conn_close(c);
		}
	}
	return did;
}
