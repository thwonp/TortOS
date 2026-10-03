/* SPDX-License-Identifier: MIT */
/* Percent-encoding, declared in net.h and implemented HERE.
 *
 * Apart from net.c on purpose. It is a pure string function with no socket in
 * it, and tools/artrun-check.c links artscrape.c against a STUBBED network -
 * three functions it defines itself - so it cannot link net.c without
 * colliding with its own stubs. Leaving the encoder in net.c therefore forced
 * a choice between a second copy and a check that tests a different encoder
 * than the launcher uses. A file of its own costs twenty lines and refuses
 * both.
 */
#include <ctype.h>
#include <stddef.h>

#include "net.h"

void net_urlencode(const char *in, char *out, size_t outn)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t o = 0;

	if (!out || !outn) return;
	for (; in && *in && o + 4 < outn; in++) {
		unsigned char c = (unsigned char)*in;

		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			out[o++] = (char)c;
		} else {
			out[o++] = '%';
			out[o++] = hex[c >> 4];
			out[o++] = hex[c & 15];
		}
	}
	out[o] = '\0';
}
