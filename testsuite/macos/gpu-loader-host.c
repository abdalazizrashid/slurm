/* SPDX-License-Identifier: GPL-2.0-or-later
 * Deliberately no Metal dependency or policy installation: the harness checks
 * the separately injected constructor's fresh marker as well as this output.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void)
{
	const char *marker = getenv("SLURM_GPU_LOADER_HOST_MARKER");
	const char *context = getenv("SLURM_SCRIPT_CONTEXT");

	/* Epilog stdout is not the task's stream: leave a separate main marker. */
	if (marker) {
		int fd = open(marker,
			      O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW |
				      O_CLOEXEC,
			      0600);

		if (fd < 0) {
			perror("GPU loader host marker");
			return 94;
		}
		int rc = dprintf(fd, "GPU_LOADER_HOST_MAIN=PASS\nCONTEXT=%s\n",
				 context ? context : "");
		if (close(fd) || rc < 0)
			return 95;
	}
	puts("GPU_LOADER_HOST_MAIN=PASS");
	return 0;
}
