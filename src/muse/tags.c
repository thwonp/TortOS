/* SPDX-License-Identifier: MIT */
/* See tags.h. */
#include "tags.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <libavformat/avformat.h>

/* More than any album has; a folder past it is answered from these. */
#define TAGS_MAX_FILES 200

/* A tag, the container's first and then the audio stream's: Ogg keeps Vorbis
 * and Opus comments on the stream, as dec_tag in dec.c says. */
static void tag_of(AVFormatContext *f, int si, const char *key, char *out, size_t n)
{
	AVDictionaryEntry *e = av_dict_get(f->metadata, key, NULL, 0);

	if (!e && si >= 0) e = av_dict_get(f->streams[si]->metadata, key, NULL, 0);
	snprintf(out, n, "%s", e && e->value ? e->value : "");
}

static int audio_stream(const AVFormatContext *f)
{
	unsigned i;

	for (i = 0; i < f->nb_streams; i++)
		if (f->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) return (int)i;
	return -1;
}

int tags_folder_artist(const char *dir, char *out, size_t n)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	char album_artist[256] = "", artist[256] = "", v[256], path[2048];
	bool aa_split = false, ar_split = false;
	int read = 0;

	out[0] = '\0';
	if (!d) return -1;
	while ((e = readdir(d)) && read < TAGS_MAX_FILES) {
		AVFormatContext *f = NULL;
		int si;

		if (e->d_name[0] == '.') continue;
		if (snprintf(path, sizeof path, "%s/%s", dir, e->d_name) >= (int)sizeof path)
			continue;
		if (avformat_open_input(&f, path, NULL, NULL) < 0) continue;
		if ((si = audio_stream(f)) < 0) { avformat_close_input(&f); continue; }
		read++;

		tag_of(f, si, "album_artist", v, sizeof v);
		if (v[0]) {
			if (!album_artist[0]) snprintf(album_artist, sizeof album_artist, "%s", v);
			else if (strcmp(album_artist, v)) aa_split = true;
		}
		tag_of(f, si, "artist", v, sizeof v);
		if (!v[0] || (artist[0] && strcmp(artist, v))) ar_split = true;
		else if (!artist[0]) snprintf(artist, sizeof artist, "%s", v);
		avformat_close_input(&f);
	}
	closedir(d);

	if (album_artist[0] && !aa_split) snprintf(out, n, "%s", album_artist);
	else if (artist[0] && !ar_split) snprintf(out, n, "%s", artist);
	return read;
}
