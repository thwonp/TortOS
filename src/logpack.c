/* SPDX-License-Identifier: MIT */
/* See logpack.h. */
#include "logpack.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

void logpack_mask_word(const char *word, char *out, size_t n)
{
	size_t len = strlen(word);

	if (len < 3) { snprintf(out, n, "***"); return; }
	snprintf(out, n, "%c...%c", word[0], word[len - 1]);
}

/* Replace `len` bytes at `at` in `line` with `with`, within `cap`. */
static void splice(char *line, size_t cap, char *at, size_t len, const char *with)
{
	size_t wl = strlen(with), tail = strlen(at + len);

	if ((size_t)(at - line) + wl + tail + 1 > cap) {
		/* No room for the rest: keep what fits, masked, and cut the tail. */
		if ((size_t)(at - line) + wl + 1 > cap) { *at = '\0'; return; }
		tail = cap - (size_t)(at - line) - wl - 1;
	}
	memmove(at + wl, at + len, tail);
	memcpy(at, with, wl);
	at[wl + tail] = '\0';
}

/* Six hex pairs joined by `sep`, starting at `p`. */
static bool mac_at(const char *p, char sep)
{
	int k;

	for (k = 0; k < 6; k++) {
		if (!isxdigit((unsigned char)p[k * 3]) || !isxdigit((unsigned char)p[k * 3 + 1]))
			return false;
		if (k < 5 && p[k * 3 + 2] != sep) return false;
	}
	/* Not the middle of a longer run of hex, which would be something else. */
	return !isxdigit((unsigned char)p[17]);
}

void logpack_mask_line(char *line, size_t cap, const char *const *secrets, int n)
{
	char masked[128], *p;
	int order[64], m = 0, a, b;

	/* Longest first, so a network named "Home" cannot eat into one named
	 * "Home 5G" before that one is found. */
	for (a = 0; a < n && m < 64; a++)
		if (secrets[a] && strlen(secrets[a]) >= 3) order[m++] = a;
	for (a = 1; a < m; a++)
		for (b = a; b > 0 && strlen(secrets[order[b]]) > strlen(secrets[order[b - 1]]); b--) {
			int t = order[b];

			order[b] = order[b - 1];
			order[b - 1] = t;
		}
	for (a = 0; a < m; a++) {
		const char *s = secrets[order[a]];
		size_t sl = strlen(s);

		logpack_mask_word(s, masked, sizeof masked);
		for (p = strstr(line, s); p; p = strstr(p + strlen(masked), s))
			splice(line, cap, p, sl, masked);
	}

	for (p = line; strlen(p) >= 17; p++) {
		char sep;
		int k;

		if (!mac_at(p, ':') && !mac_at(p, '_')) continue;
		/* Not the tail of a longer run of hex either. */
		if (p > line && isxdigit((unsigned char)p[-1])) continue;
		sep = p[2];
		for (k = 0; k < 5; k++) {
			p[k * 3] = 'X';
			p[k * 3 + 1] = 'X';
			p[k * 3 + 2] = sep;
		}
		p += 16;
	}
}

/* ---- the pack ------------------------------------------------------------- */

static bool copy_masked(const char *from, const char *to,
                        const char *const *secrets, int n)
{
	FILE *in = fopen(from, "r"), *out;
	char line[4096];

	if (!in) return false;
	if (!(out = fopen(to, "w"))) { fclose(in); return false; }
	while (fgets(line, sizeof line, in)) {
		logpack_mask_line(line, sizeof line, secrets, n);
		fputs(line, out);
	}
	fclose(in);
	return fclose(out) == 0;
}

static bool run(char *const argv[])
{
	pid_t pid = fork();
	int status;

	if (pid < 0) return false;
	if (pid == 0) {
		execvp(argv[0], argv);
		_exit(127);
	}
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR) return false;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool logpack_build(const char *logs_dir, const char *about,
                   const char *const *secrets, int n,
                   const char *folder, const char *out, char *err, size_t en)
{
	char stage[] = "/tmp/tortos-logpack.XXXXXX", dir[512], path[1024], from[1024];
	DIR *d;
	struct dirent *e;
	FILE *f;
	int logs = 0;
	bool ok;

	err[0] = '\0';
	if (!mkdtemp(stage)) { snprintf(err, en, "no room in /tmp"); return false; }
	snprintf(dir, sizeof dir, "%s/%s", stage, folder);
	if (mkdir(dir, 0755) != 0) { snprintf(err, en, "no room in /tmp"); ok = false; goto out; }

	if ((d = opendir(logs_dir))) {
		while ((e = readdir(d))) {
			if (strncmp(e->d_name, "tortos.log", 10) != 0) continue;
			snprintf(from, sizeof from, "%s/%s", logs_dir, e->d_name);
			snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
			if (copy_masked(from, path, secrets, n)) logs++;
		}
		closedir(d);
	}
	snprintf(path, sizeof path, "%s/about.txt", dir);
	if ((f = fopen(path, "w"))) {
		const char *line = about ? about : "";

		/* about.txt names the same things a log does, so it is masked too.
		 * Line by line and blank lines kept: strtok ran two newlines into
		 * one and lost the gap before the shelves. */
		while (*line) {
			const char *nl = strchr(line, '\n');
			size_t len = nl ? (size_t)(nl - line) : strlen(line);
			char buf[1024];

			snprintf(buf, sizeof buf, "%.*s", (int)(len < sizeof buf ? len : sizeof buf - 1), line);
			logpack_mask_line(buf, sizeof buf, secrets, n);
			fprintf(f, "%s\n", buf);
			line += len + (nl ? 1 : 0);
		}
		fclose(f);
	}
	if (!logs) { snprintf(err, en, "no logs found"); ok = false; goto out; }

	{
		char *argv[] = { (char *)"tar", (char *)"-czf", (char *)out, (char *)"-C",
		                 stage, (char *)folder, NULL };

		remove(out);
		ok = run(argv);
		if (!ok) snprintf(err, en, "tar failed");
	}
out:
	{
		char *argv[] = { (char *)"rm", (char *)"-rf", stage, NULL };

		run(argv);
	}
	return ok;
}
