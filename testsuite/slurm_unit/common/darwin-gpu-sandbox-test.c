/*****************************************************************************\
 *  darwin-gpu-sandbox-test.c - Metal denial and inheritance.
 *****************************************************************************
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include "config.h"

#include <errno.h>
#include <sandbox.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "src/common/darwin_sandbox.h"

#ifndef DARWIN_METAL_PROBE_TEST_PATH
#error "Define the build-tree Metal probe path for this test"
#endif

static int _wait(pid_t child)
{
	int status;
	pid_t rc;

	do {
		rc = waitpid(child, &status, 0);
	} while ((rc < 0) && (errno == EINTR));
	if ((rc != child) || !WIFEXITED(status)) {
		fprintf(stderr,
			"Metal probe wait failed: pid=%d status=%d errno=%d\n",
			child, rc == child ? status : -1, errno);
		return 1;
	}
	return WEXITSTATUS(status);
}

static int _run(int mode)
{
	pid_t child = fork();

	if (child < 0)
		return 1;
	if (!child) {
		char *args[] = { DARWIN_METAL_PROBE_TEST_PATH,
				 mode ? "--expect-denied" : "--expect-allowed",
				 NULL };
		char *clean_env[] = { "PATH=/usr/bin:/bin", NULL };
		int rc;

		alarm(90);
		if (mode &&
		    (rc = darwin_sandbox_apply(DARWIN_SANDBOX_DENY_GPU_OPEN))) {
			fprintf(stderr, "GPU policy installation failed: %d\n",
				rc);
			_exit(1);
		}
		if (mode == 2) {
			pid_t next = fork();
			if (next < 0)
				_exit(1);
			if (next)
				_exit(_wait(next));
			alarm(90);
		} else if (mode == 3) {
			pid_t next;
			rc = posix_spawn(&next, args[0], NULL, NULL, args,
					 clean_env);
			_exit(rc ? 1 : _wait(next));
		} else if (mode == 4) {
			if (setsid() < 0)
				_exit(1);
		} else if (mode == 5) {
			char *message = NULL;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
			rc = sandbox_init("(version 1)(allow default)", 0,
					  &message);
			int saved_errno = errno;
			sandbox_free_error(message);
#pragma clang diagnostic pop
			/* sandbox_init documents failure, not an errno value. */
			fprintf(stderr,
				"Sandbox relaxation result: rc=%d errno=%d\n",
				rc, saved_errno);
			if (!rc)
				_exit(1);
			/* The original GPU restriction must still deny acquisition. */
		}
		execve(args[0], args, clean_env);
		fprintf(stderr, "Metal probe exec failed: %d\n", errno);
		_exit(1);
	}
	return _wait(child);
}

int main(void)
{
	const char *names[] = { "baseline",   "deny-exec",   "deny-fork-exec",
				"deny-spawn", "deny-setsid", "deny-reset" };

	for (unsigned mode = 0; mode < sizeof(names) / sizeof(names[0]);
	     mode++) {
		int rc = _run(mode);
		printf("%s: exit=%d\n", names[mode], rc);
		/* No available GPU is a test skip, never production qualification. */
		if (!mode && (rc == 77))
			return 77;
		if (rc)
			return 1;
	}
	return 0;
}
