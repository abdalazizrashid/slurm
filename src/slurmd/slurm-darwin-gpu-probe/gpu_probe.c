/*****************************************************************************\
 *  gpu_probe.c - qualify the native GPU connection policy.
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
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "src/common/darwin_sandbox.h"

#ifndef DARWIN_METAL_PROBE_PATH
#error "DARWIN_METAL_PROBE_PATH must name the installed trusted worker"
#endif

int main(int argc, char **argv)
{
	int deny, rc;
	char *clean_env[] = { "PATH=/usr/bin:/bin", NULL };
	char *worker_argv[] = { DARWIN_METAL_PROBE_PATH, NULL, NULL };

	if ((argc != 2) ||
	    (strcmp(argv[1], "--baseline") && strcmp(argv[1], "--deny"))) {
		fprintf(stderr, "Usage: %s --baseline|--deny\n", argv[0]);
		return 2;
	}
	deny = !strcmp(argv[1], "--deny");
	/* The alarm survives exec; the caller also imposes a command deadline. */
	alarm(90);
	if (deny && (rc = darwin_sandbox_apply(DARWIN_SANDBOX_DENY_GPU_OPEN))) {
		fprintf(stderr, "GPU sandbox installation failed: %s\n",
			strerror(rc));
		return 1;
	}
	/* Metal and its initializers load only after the policy decision. */
	worker_argv[1] = deny ? "--expect-denied" : "--expect-allowed";
	execve(worker_argv[0], worker_argv, clean_env);
	fprintf(stderr, "Cannot execute trusted Metal probe %s: %s\n",
		worker_argv[0], strerror(errno));
	return 1;
}
