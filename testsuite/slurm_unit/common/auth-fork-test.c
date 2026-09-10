/*****************************************************************************\
 * auth-fork-test.c - authentication lock recovery after fork.
 *****************************************************************************
 * This file is part of Slurm. See <https://slurm.schedmd.com/>.
 * Distributed under the GNU General Public License, version 2 or later.
\*****************************************************************************/

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "src/interfaces/auth.h"

int main(void)
{
	alarm(15);
	/* No plugins are needed to exercise the real interface's fork hooks. */
	unsetenv("SLURM_JWT");
	unsetenv("SLURM_SACK_KEY");
	unsetenv("SLURM_SACK_JWKS");
	if (auth_g_init())
		return EXIT_FAILURE;
	for (int locked = 0; locked <= 2; locked++) {
		int status;
		pid_t pid;

		if (locked == 1)
			auth_setuid_lock();
		else if (locked == 2)
			auth_context_lock();
		pid = fork();
		if (!pid) {
			alarm(5);
			if (locked == 1)
				auth_setuid_unlock();
			if (auth_g_init() || auth_g_fini())
				_exit(EXIT_FAILURE);
			_exit(EXIT_SUCCESS);
		}
		if (locked == 1)
			auth_setuid_unlock();
		else if (locked == 2)
			auth_context_unlock();
		if ((pid < 0) || (waitpid(pid, &status, 0) != pid) ||
		    !WIFEXITED(status) || WEXITSTATUS(status))
			return EXIT_FAILURE;
		if (auth_g_init())
			return EXIT_FAILURE;
	}
	if (auth_g_fini())
		return EXIT_FAILURE;
	puts("Authentication locks survive unlocked, privilege-locked, and context-locked forks");
	return EXIT_SUCCESS;
}
