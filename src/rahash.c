/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* See rahash.h. Three pieces: MD5, enough zip to reach the ROM, and
 * RetroAchievements' per-console rule about how much of it counts. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "rahash.h"

/* ---------------------------------------------------------------- MD5 ---- */

typedef struct {
	uint32_t a, b, c, d;
	uint64_t len;
	unsigned char buf[64];
	size_t have;
} md5_ctx;

static const uint32_t K[64] = {
	0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,
	0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
	0x6b901122,0xfd987193,0xa679438e,0x49b40821,0xf61e2562,0xc040b340,
	0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
	0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,
	0x676f02d9,0x8d2a4c8a,0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
	0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,
	0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
	0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,
	0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
	0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};
static const unsigned char S[64] = {
	7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
	5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
	4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
	6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
};

static uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

static void md5_block(md5_ctx *m, const unsigned char *p)
{
	uint32_t M[16], a = m->a, b = m->b, c = m->c, d = m->d;
	int i;

	for (i = 0; i < 16; i++)
		M[i] = (uint32_t)p[i*4] | ((uint32_t)p[i*4+1] << 8) |
		       ((uint32_t)p[i*4+2] << 16) | ((uint32_t)p[i*4+3] << 24);

	for (i = 0; i < 64; i++) {
		uint32_t f;
		int g;

		if (i < 16)      { f = (b & c) | (~b & d);        g = i; }
		else if (i < 32) { f = (d & b) | (~d & c);        g = (5*i + 1) & 15; }
		else if (i < 48) { f = b ^ c ^ d;                 g = (3*i + 5) & 15; }
		else             { f = c ^ (b | ~d);              g = (7*i) & 15; }

		f += a + K[i] + M[g];
		a = d; d = c; c = b;
		b += rol(f, S[i]);
	}
	m->a += a; m->b += b; m->c += c; m->d += d;
}

static void md5_init(md5_ctx *m)
{
	m->a = 0x67452301; m->b = 0xefcdab89;
	m->c = 0x98badcfe; m->d = 0x10325476;
	m->len = 0; m->have = 0;
}

static void md5_update(md5_ctx *m, const void *data, size_t n)
{
	const unsigned char *p = data;

	m->len += n;
	while (n) {
		size_t take = 64 - m->have;
		if (take > n) take = n;
		memcpy(m->buf + m->have, p, take);
		m->have += take; p += take; n -= take;
		if (m->have == 64) { md5_block(m, m->buf); m->have = 0; }
	}
}

static void md5_hex(md5_ctx *m, char *out)
{
	uint64_t bits = m->len * 8;
	unsigned char tail[8];
	static const unsigned char pad[64] = { 0x80 };
	size_t padn;
	uint32_t w[4];
	int i;

	padn = (m->have < 56) ? 56 - m->have : 120 - m->have;
	md5_update(m, pad, padn);
	for (i = 0; i < 8; i++) tail[i] = (unsigned char)(bits >> (8 * i));
	md5_update(m, tail, 8);

	w[0] = m->a; w[1] = m->b; w[2] = m->c; w[3] = m->d;
	for (i = 0; i < 16; i++)
		sprintf(out + i * 2, "%02x", (unsigned)((w[i / 4] >> (8 * (i % 4))) & 0xff));
	out[32] = '\0';
}

/* ------------------------------------------------------------ the zip ---- */

/* zlib by dlopen rather than by linking, for the reason Diatom's zip.c gives:
 * the device ships libz.so.1 in firmware and the cross toolchain carries no
 * aarch64 zlib to link against. A STORED entry needs none of it, so the
 * dependency is honestly optional rather than pretend-optional. */
typedef struct {
	const unsigned char *next_in;  unsigned avail_in;  unsigned long total_in;
	unsigned char       *next_out; unsigned avail_out; unsigned long total_out;
	const char *msg; void *state;
	void *zalloc, *zfree, *opaque;
	int data_type; unsigned long adler, reserved;
} z_streamish;

static int  (*p_inflateInit2_)(z_streamish *, int, const char *, int);
static int  (*p_inflate)(z_streamish *, int);
static int  (*p_inflateEnd)(z_streamish *);
static int  zlib_tried;

static bool zlib_ready(void)
{
	void *h;

	if (zlib_tried) return p_inflate != NULL;
	zlib_tried = 1;
	/* The device's name first, then the host's. The native build is how the
	 * two hashers get checked against each other (make check-rahash), so it
	 * has to work here too - and it did not, which is exactly the sort of
	 * thing a check that only ever ran on the device would never have said. */
	h = dlopen("libz.so.1", RTLD_NOW | RTLD_LOCAL);
	if (!h) h = dlopen("libz.so", RTLD_NOW | RTLD_LOCAL);
	if (!h) h = dlopen("libz.1.dylib", RTLD_NOW | RTLD_LOCAL);
	if (!h) h = dlopen("libz.dylib", RTLD_NOW | RTLD_LOCAL);
	if (!h) {
		fprintf(stderr, "rahash: no zlib; only uncompressed ROMs can be hashed\n");
		return false;
	}
	p_inflateInit2_ = dlsym(h, "inflateInit2_");
	p_inflate       = dlsym(h, "inflate");
	p_inflateEnd    = dlsym(h, "inflateEnd");
	return p_inflateInit2_ && p_inflate && p_inflateEnd;
}

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool is_zip(const char *path)
{
	size_t n = path ? strlen(path) : 0;
	return n > 4 && strcasecmp(path + n - 4, ".zip") == 0;
}

/* The largest entry, whole. Deliberately the same choice Diatom's zip.c
 * makes - see rahash.h for why that coupling matters. */
static unsigned char *zip_largest(const char *path, size_t *out_len)
{
	FILE *f = fopen(path, "rb");
	unsigned char tail[66000 + 22], hdr[46], lh[30];
	long fsize, tail_off;
	size_t tail_len, i;
	uint32_t cd_off = 0, cd_n = 0;
	uint32_t best_u = 0, best_c = 0, best_lho = 0;
	uint16_t best_m = 0;
	unsigned char *comp = NULL, *un = NULL;
	int found = 0;

	*out_len = 0;
	if (!f) return NULL;

	fseek(f, 0, SEEK_END);
	fsize = ftell(f);
	tail_len = fsize < (long)sizeof tail ? (size_t)fsize : sizeof tail;
	if (tail_len < 22) { fclose(f); return NULL; }
	tail_off = fsize - (long)tail_len;
	fseek(f, tail_off, SEEK_SET);
	if (fread(tail, 1, tail_len, f) != tail_len) { fclose(f); return NULL; }
	for (i = tail_len - 22 + 1; i-- > 0; ) {
		if (tail[i] == 'P' && tail[i+1] == 'K' && tail[i+2] == 5 && tail[i+3] == 6) {
			cd_n   = rd16(tail + i + 10);
			cd_off = rd32(tail + i + 16);
			found = 1;
			break;
		}
	}
	if (!found || !cd_n) { fclose(f); return NULL; }

	fseek(f, (long)cd_off, SEEK_SET);
	for (i = 0; i < cd_n; i++) {
		char en[512];
		uint16_t nlen, elen, clen, method;
		uint32_t usize;

		if (fread(hdr, 1, 46, f) != 46 || rd32(hdr) != 0x02014b50) break;
		method = rd16(hdr + 10);
		usize  = rd32(hdr + 24);
		nlen   = rd16(hdr + 28);
		elen   = rd16(hdr + 30);
		clen   = rd16(hdr + 32);
		if (nlen >= sizeof en) break;
		if (fread(en, 1, nlen, f) != nlen) break;
		en[nlen] = '\0';
		fseek(f, elen + clen, SEEK_CUR);

		if (usize == 0 || (nlen && en[nlen - 1] == '/')) continue;
		if (method != 0 && method != 8) continue;
		if (usize > best_u) {
			best_u = usize;
			best_c = rd32(hdr + 20);
			best_m = method;
			best_lho = rd32(hdr + 42);
		}
	}
	if (!best_u) { fclose(f); return NULL; }

	fseek(f, (long)best_lho, SEEK_SET);
	if (fread(lh, 1, 30, f) != 30 || rd32(lh) != 0x04034b50) { fclose(f); return NULL; }
	fseek(f, rd16(lh + 26) + rd16(lh + 28), SEEK_CUR);

	comp = malloc(best_c ? best_c : 1);
	un   = malloc(best_u);
	if (!comp || !un) goto fail;
	if (fread(comp, 1, best_c, f) != best_c) goto fail;
	fclose(f);
	f = NULL;

	if (best_m == 0) {
		memcpy(un, comp, best_u);
	} else {
		z_streamish z;
		int rc;

		if (!zlib_ready()) goto fail;
		memset(&z, 0, sizeof z);
		z.next_in = comp; z.avail_in = best_c;
		z.next_out = un;  z.avail_out = best_u;
		/* -15: raw deflate, no zlib wrapper, which is what a zip stores. */
		if (p_inflateInit2_(&z, -15, "1.2.8", (int)sizeof z) != 0) goto fail;
		rc = p_inflate(&z, 4 /* Z_FINISH */);
		p_inflateEnd(&z);
		if (rc != 1 /* Z_STREAM_END */ || z.total_out != best_u) goto fail;
	}
	free(comp);
	*out_len = best_u;
	return un;

fail:
	if (f) fclose(f);
	free(comp);
	free(un);
	return NULL;
}

/* --------------------------------------------------------- the rules ----- */

/* How much of a ROM RetroAchievements actually hashes. Theirs, not ours:
 *
 *   NES         an iNES header is 16 bytes of container, not game
 *   SFC         copier headers are 512 bytes and only sometimes present,
 *               which is what the size test detects
 *   PCE         the same idea against a different block size
 *   MD/SMS/GG   an SMD-style header, likewise conditional
 *   GB/GBC/GBA  the whole file; there is no container to strip
 */
static size_t ra_skip(const char *tag, const unsigned char *d, size_t n)
{
	if (!strcmp(tag, "NES"))
		return (n >= 4 && !memcmp(d, "NES\x1a", 4)) ? 16 : 0;
	if (!strcmp(tag, "SFC"))
		return (n % 1024 == 512) ? 512 : 0;
	if (!strcmp(tag, "PCE"))
		return (n % 131072 == 512) ? 512 : 0;
	if (!strcmp(tag, "MD") || !strcmp(tag, "SMS") || !strcmp(tag, "GG"))
		return (n % 16384 == 512) ? 512 : 0;
	return 0;
}

void ra_md5_hex(const void *data, size_t len, char *out)
{
	md5_ctx m;

	md5_init(&m);
	md5_update(&m, data, len);
	md5_hex(&m, out);
}

bool ra_hash_rom(const char *path, const char *tag, char *out)
{
	unsigned char *data = NULL;
	size_t len = 0, skip;
	md5_ctx m;

	if (!path || !tag || !out) return false;
	out[0] = '\0';
	/* A PlayStation disc is hundreds of MB and this reads the whole file into
	 * memory - fatal on the Brick - for a hash RetroAchievements would not
	 * recognise anyway: discs are hashed by their boot executable. No PS set
	 * can be fetched until that is written (plorpos-gkd.49). */
	if (!strcmp(tag, "PS")) return false;
	/* Arcade sets are hashed by their NAME, not their contents, and the zip's
	 * largest file would be the wrong thing at up to tens of MB. Not written
	 * yet; until it is, no arcade set can be fetched. */
	if (!strcmp(tag, "ARCADE") || !strcmp(tag, "NEOGEO")) return false;

	if (is_zip(path)) {
		data = zip_largest(path, &len);
		if (!data) return false;
	} else {
		FILE *f = fopen(path, "rb");
		long sz;

		if (!f) return false;
		fseek(f, 0, SEEK_END);
		sz = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (sz <= 0) { fclose(f); return false; }
		data = malloc((size_t)sz);
		if (!data) { fclose(f); return false; }
		if (fread(data, 1, (size_t)sz, f) != (size_t)sz) {
			free(data); fclose(f); return false;
		}
		fclose(f);
		len = (size_t)sz;
	}

	skip = ra_skip(tag, data, len);
	if (skip >= len) { free(data); return false; }

	md5_init(&m);
	md5_update(&m, data + skip, len - skip);
	md5_hex(&m, out);
	free(data);
	return true;
}
