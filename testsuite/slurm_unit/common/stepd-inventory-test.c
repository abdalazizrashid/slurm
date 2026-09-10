/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config.h"
#undef NDEBUG
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static struct dirent *_test_readdir(DIR *directory);
#define readdir _test_readdir
#include "src/common/stepd_api.c"
#undef readdir

static bool read_fault;

static struct dirent *_test_readdir(DIR *directory)
{
	if (read_fault) {
		errno = EIO;
		return NULL;
	}
	return readdir(directory);
}

static void _expect(const char *path, bool expected)
{
	bool complete = !expected;
	list_t *steps =
		stepd_available_checked(path, "inventory-test", &complete);

	assert(complete == expected);
	assert(steps && !list_count(steps));
	FREE_NULL_LIST(steps);
}

int main(void)
{
	char directory[] = "/tmp/slurm-stepd-inventory-XXXXXX";
	char path[PATH_MAX];
	int fd;

	assert(mkdtemp(directory));
	_expect(directory, true);
	assert(snprintf(path, sizeof(path), "%s/missing", directory) <
	       sizeof(path));
	_expect(path, false);
	assert(snprintf(path, sizeof(path), "%s/regular-file", directory) <
	       sizeof(path));
	fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
	assert(fd >= 0);
	assert(!close(fd));
	_expect(path, false);
	assert(!unlink(path));
	if (geteuid()) {
		bool complete = true;
		list_t *steps;

		assert(!chmod(directory, 0000));
		steps = stepd_available_checked(directory, "inventory-test",
						&complete);
		assert(!chmod(directory, 0700));
		assert(!complete && steps && !list_count(steps));
		FREE_NULL_LIST(steps);
	} else {
		puts("SKIP permission-denied directory check for root");
	}
	read_fault = true;
	_expect(directory, false);
	read_fault = false;
	_expect(directory, true);
	assert(!rmdir(directory));
	puts("Stepd inventory distinguishes empty from incomplete scans: PASS");
	return 0;
}
