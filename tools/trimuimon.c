/* SPDX-License-Identifier: MIT */
/* Volume and brightness on the TrimUI Model S (plorpos-80b), on the shelf and
 * in games alike: the stock boot loop starts its keymon only for its own
 * MainUI, so with plorpOS in its place nothing sets either, and the codec
 * comes up at volume 0.
 *
 *   trimuimon FILE      FILE keeps "volume brightness", 0-20 and 0-10
 *
 * Sets both from FILE at start, then reads the buttons beside whoever else
 * does (no grab): SELECT + L / R is volume down / up, START + L / R
 * brightness, as MinUI had them. Volume is the codec's "head phone volume"
 * (control 22, 0-63), which is the speaker's too; brightness is the
 * display driver's lcdbl, 70-120 (MinUI's range, so its 0-10 means the
 * same). FILE is written a second after the last change, not per press. */
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define VOL_MAX 20
#define BRI_MAX 10
#define LCDBL   "/sys/class/disp/disp/attr/lcdbl"

/* The kernel's input_event here: 32-bit timeval, 16 bytes. */
struct kev { uint32_t sec, usec; uint16_t type, code; int32_t value; };

static int vol = 12, bri = 7;

static void apply(void)
{
	char cmd[64];
	FILE *f;

	snprintf(cmd, sizeof cmd, "tinymix -D 0 set 22 %d >/dev/null 2>&1", vol * 63 / VOL_MAX);
	if (system(cmd) == -1) { /* stays as it was */ }
	if ((f = fopen(LCDBL, "w"))) { fprintf(f, "%d", 70 + bri * 5); fclose(f); }
}

int main(int argc, char **argv)
{
	const char *file = argc > 1 ? argv[1] : "/tmp/trimuimon.txt";
	int fd = open("/dev/input/event0", O_RDONLY), sel = 0, start = 0, dirty = 0;
	FILE *f;

	if ((f = fopen(file, "r"))) {
		if (fscanf(f, "%d %d", &vol, &bri) != 2) { vol = 12; bri = 7; }
		fclose(f);
	}
	if (vol < 0 || vol > VOL_MAX) vol = 12;
	if (bri < 0 || bri > BRI_MAX) bri = 7;
	apply();
	if (fd < 0) { perror("event0"); return 1; }
	for (;;) {
		struct pollfd p = { fd, POLLIN, 0 };
		struct kev ev;
		int d = 0;

		if (poll(&p, 1, dirty ? 1000 : -1) == 0) {
			if ((f = fopen(file, "w"))) { fprintf(f, "%d %d\n", vol, bri); fclose(f); }
			dirty = 0;
			continue;
		}
		if (read(fd, &ev, sizeof ev) != sizeof ev) return 1;
		if (ev.type != EV_KEY) continue;
		if (ev.code == KEY_RIGHTCTRL) { sel = ev.value != 0; continue; }
		if (ev.code == KEY_ENTER) { start = ev.value != 0; continue; }
		if (ev.value == 0 || (ev.code != KEY_TAB && ev.code != KEY_BACKSPACE)) continue;
		d = ev.code == KEY_TAB ? -1 : 1;
		if (sel && vol + d >= 0 && vol + d <= VOL_MAX) vol += d;
		else if (start && !sel && bri + d >= 0 && bri + d <= BRI_MAX) bri += d;
		else continue;
		apply();
		dirty = 1;
	}
}
