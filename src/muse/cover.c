/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* See cover.h. */
#include "cover.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <libavformat/avformat.h>

/* Which of a file's pictures is the cover. ID3 and FLAC say what each picture
 * is, and FFmpeg passes that on as the stream's comment - a file can carry the
 * back of the sleeve and the disc as well, and the first one is not always the
 * front. MP4 says nothing, and there the first picture is the only one. */
static AVStream *pick(AVFormatContext *fmt, const char **ext)
{
	AVStream *first = NULL;
	const char *first_ext = NULL;
	unsigned i;

	for (i = 0; i < fmt->nb_streams; i++) {
		AVStream *st = fmt->streams[i];
		AVDictionaryEntry *c;
		const char *e;

		if (!(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) continue;
		if (st->attached_pic.size <= 0) continue;
		/* The two formats the launcher's SDL_image was built with and that
		 * taggers write. A GIF or a BMP cover exists and is not worth one. */
		if (st->codecpar->codec_id == AV_CODEC_ID_MJPEG)    e = "jpg";
		else if (st->codecpar->codec_id == AV_CODEC_ID_PNG) e = "png";
		else continue;

		c = av_dict_get(st->metadata, "comment", NULL, 0);
		if (c && !strcmp(c->value, "Cover (front)")) { *ext = e; return st; }
		if (!first) { first = st; first_ext = e; }
	}
	*ext = first_ext;
	return first;
}

int cover_extract(const char *path, const char *base, char *out, size_t n)
{
	AVFormatContext *fmt = NULL;
	AVStream *st;
	const char *ext = NULL;
	char dir[1024], part[1100], *slash;
	FILE *f;
	int ok;

	out[0] = '\0';
	/* The header alone: every container puts its pictures there, so nothing
	 * has to be decoded or even probed past it. */
	if (avformat_open_input(&fmt, path, NULL, NULL) < 0) return -1;
	st = pick(fmt, &ext);
	if (!st) { avformat_close_input(&fmt); return 0; }

	snprintf(dir, sizeof dir, "%s", base);
	slash = strrchr(dir, '/');
	if (slash) { *slash = '\0'; mkdir(dir, 0755); }

	snprintf(out, n, "%s.%s", base, ext);
	snprintf(part, sizeof part, "%s.part", out);
	f = fopen(part, "wb");
	ok = f && fwrite(st->attached_pic.data, 1, (size_t)st->attached_pic.size, f)
	          == (size_t)st->attached_pic.size;
	if (f && fclose(f) != 0) ok = 0;
	avformat_close_input(&fmt);
	if (!ok || rename(part, out) != 0) {
		remove(part);
		out[0] = '\0';
		return -1;
	}
	return 1;
}
