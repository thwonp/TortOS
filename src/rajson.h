/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef TORTOS_RAJSON_H
#define TORTOS_RAJSON_H

#include <stdbool.h>
#include <stddef.h>

/* Just enough JSON to read RetroAchievements' replies.
 *
 * A scanner over spans rather than a parser into a tree: the patch response
 * for a large set runs past 100KB and holds a few hundred objects, and every
 * field wanted from it is read once. Building a tree would allocate for all of
 * it to use six fields per achievement.
 *
 * Not pattern-matching, though. `MemAddr` is a machine-generated condition
 * string and titles carry apostrophes, quotes and unicode escapes, so
 * "find the next quote" is wrong in a way that only shows up on the games with
 * the most interesting achievements.
 */

typedef struct { const char *b, *e; } jsv;   /* one value, [b,e) */

/* The whole document as a value. */
jsv js_root(const char *text, size_t len);

/* A member of an object. False if `v` is not an object or has no such key. */
bool js_member(jsv v, const char *name, jsv *out);

/* Iterate an array. Zero `it` before the first call; returns false at the end.
 *
 *     jsv it = {0}, e;
 *     while (js_next(arr, &it, &e)) { ... }
 */
bool js_next(jsv arr, jsv *it, jsv *elem);

/* A string, unescaped, NUL terminated and truncated to fit. False if the value
 * is not a string. \uXXXX becomes UTF-8; a lone surrogate becomes U+FFFD
 * rather than a malformed sequence the font will render as a box. */
bool js_str(jsv v, char *out, size_t n);

/* A number, truncated to integer. Zero if the value is not a number, which is
 * indistinguishable from a real zero - so use js_member's result to tell
 * "absent" from "zero" when that matters. */
long js_int(jsv v);

bool js_is_true(jsv v);

#endif
