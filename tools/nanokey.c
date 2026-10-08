/* SPDX-License-Identifier: MIT */
/* Presses keys on the RG Nano from adb, for tests that drive its screens
 * (plorpos-ggv). The Nano's buttons reach programs as one evdev keyboard,
 * fkgpiod's uinput device; an input_event written to that node is delivered
 * to its readers as if fkgpiod had sent it.
 *
 *   nanokey [-d /dev/input/event0] [-h ms] KEY...
 *
 * KEY is a letter fkgpiod maps a button to (u d l r a b x y s k m n q - see
 * /etc/fkgpiod.conf), or "wNNN" to wait NNN ms. Each key is pressed, held
 * -h ms (default 60) and released, then 120 ms pass before the next.
 *
 * fkgpiod's own `echo "KEYPRESS KEY_A" > /tmp/fkgpiod.fifo` reaches the same
 * place. Either only reaches an SDL program while VT switching works: the
 * Nano's SDL locks it, and a program killed outside FunKey's frontend loop
 * leaves it locked, keys going nowhere. `termfix_all; chvt 1` afterwards, as
 * that loop does (measured 2026-10-08). */
#include <fcntl.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static int fd;

/* The kernel's input_event on this armv7 4.14 kernel: a 32-bit timeval, 16
 * bytes. The SDK's musl 1.2 has a 64-bit time_t, so <linux/input.h>'s struct
 * comes out 24 bytes, and the kernel took the first 16 of each write as an
 * event and returned short. */
struct kev { uint32_t sec, usec; uint16_t type, code; int32_t value; };

static void emit(int type, int code, int value)
{
	struct kev ev;
	struct timeval t;

	gettimeofday(&t, NULL);
	ev.sec = (uint32_t)t.tv_sec;
	ev.usec = (uint32_t)t.tv_usec;
	ev.type = type;
	ev.code = code;
	ev.value = value;
	if (write(fd, &ev, sizeof ev) != sizeof ev) perror("write");
}

static int code(char c)
{
	static const char keys[] = "udlrabxyskmnqvohji";
	static const int codes[] = { KEY_U, KEY_D, KEY_L, KEY_R, KEY_A, KEY_B, KEY_X,
		KEY_Y, KEY_S, KEY_K, KEY_M, KEY_N, KEY_Q, KEY_V, KEY_O, KEY_H, KEY_J, KEY_I };
	const char *p = strchr(keys, c);

	return p && c ? codes[p - keys] : -1;
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/input/event0";
	int hold = 60, i = 1;

	for (; i < argc && argv[i][0] == '-'; i += 2) {
		if (i + 1 >= argc) break;
		if (!strcmp(argv[i], "-d")) dev = argv[i + 1];
		else if (!strcmp(argv[i], "-h")) hold = atoi(argv[i + 1]);
	}
	fd = open(dev, O_WRONLY);
	if (fd < 0) { perror(dev); return 1; }
	for (; i < argc; i++) {
		int k;

		if (argv[i][0] == 'w') { usleep(atoi(argv[i] + 1) * 1000); continue; }
		k = code(argv[i][0]);
		if (k < 0 || argv[i][1]) { fprintf(stderr, "unknown key %s\n", argv[i]); return 2; }
		emit(EV_KEY, k, 1);
		emit(EV_SYN, SYN_REPORT, 0);
		usleep(hold * 1000);
		emit(EV_KEY, k, 0);
		emit(EV_SYN, SYN_REPORT, 0);
		usleep(120000);
	}
	return 0;
}
