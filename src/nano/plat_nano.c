/* SPDX-License-Identifier: MIT */
/* The two platform calls src/musec.c makes, for nanoshelf on the RG Nano
 * (plorpos-ggv). The rest of src/platform.h is the SDL2 launcher's and is not
 * built here. */
#include <stdbool.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../platproc.h"

unsigned plat_now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (unsigned)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

/* As src/platform.c's: double fork, so Muse outlives nanoshelf and leaves no
 * zombie; every descriptor above stderr closed first, so the daemon holds none
 * of the launcher's (ADR-0033's lesson). envkv is unused here - Muse is
 * started with nanoshelf's own environment. */
bool plat_spawn_detached(char *const argv[], const char *const envkv[],
                         const char *workdir)
{
	struct rlimit rl;
	int top = 1024, fd;
	pid_t pid;

	(void)envkv;
	if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
		top = rl.rlim_cur < 65536 ? (int)rl.rlim_cur : 65536;
	pid = fork();
	if (pid < 0) return false;
	if (pid == 0) {
		if (fork() == 0) {
			setsid();
			for (fd = 3; fd < top; fd++) close(fd);
			if (workdir && chdir(workdir) != 0) { /* still try */ }
			execv(argv[0], argv);
			_exit(127);
		}
		_exit(0);
	}
	waitpid(pid, NULL, 0);
	return true;
}
