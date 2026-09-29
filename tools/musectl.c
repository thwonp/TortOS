/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* Talk to Muse by hand, over ssh, with no screen in front of it.
 *
 *   musectl [-s SOCKET] [-t SECONDS] 'PLAY<TAB>path=/mnt/SDCARD/Music/x.mp3' ...
 *
 * Each argument is sent as one line - a literal "\t" in an argument is turned
 * into a tab, since typing a real one through ssh and a shell is miserable -
 * and then whatever Muse says is printed for SECONDS (default 2).
 *
 * -s talks to another line socket instead: Diatom's, /tmp/diatom.sock, speaks
 * the same shape. Mind that Diatom serves ONE client and the newest wins (its
 * ADR-0033), so this takes the connection from the launcher, which gets it
 * back the next time it needs the emulator.
 *
 * An instrument, like keyinject: the device's BusyBox nc cannot open a Unix
 * socket, and this is the only way to reach the daemon before the launcher
 * speaks to it. */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	const char *path = "/tmp/muse.sock";
	double secs = 2.0;
	int fd, i = 1;
	time_t end;
	char buf[4096];

	for (;;) {
		if (argc > i + 1 && !strcmp(argv[i], "-s")) { path = argv[i + 1]; i += 2; }
		else if (argc > i + 1 && !strcmp(argv[i], "-t")) { secs = atof(argv[i + 1]); i += 2; }
		else break;
	}
	snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		fprintf(stderr, "nothing is listening on %s\n", path);
		return 1;
	}
	for (; i < argc; i++) {
		char line[2048];
		size_t n = 0;
		const char *p;

		for (p = argv[i]; *p && n + 2 < sizeof line; p++) {
			if (p[0] == '\\' && p[1] == 't') { line[n++] = '\t'; p++; }
			else line[n++] = *p;
		}
		line[n++] = '\n';
		if (write(fd, line, n) < 0) { perror("write"); return 1; }
	}
	end = time(NULL) + (time_t)(secs + 0.999);
	while (time(NULL) < end) {
		struct pollfd p = { fd, POLLIN, 0 };
		ssize_t got;

		if (poll(&p, 1, 100) <= 0) continue;
		got = read(fd, buf, sizeof buf - 1);
		if (got <= 0) break;
		buf[got] = '\0';
		fputs(buf, stdout);
		fflush(stdout);
	}
	close(fd);
	return 0;
}
