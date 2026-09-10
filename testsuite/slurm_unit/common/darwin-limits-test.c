/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native resource tests change only forked test processes. */
#include "config.h"
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <mach/mach.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "src/common/darwin_limits.h"

#define MIB (UINT64_C(1024) * 1024)

static int _wait(pid_t pid)
{
	int status;
	while (waitpid(pid, &status, 0) < 0)
		assert(errno == EINTR);
	return status;
}

static void _address_space(void)
{
	mach_task_basic_info_data_t info;
	mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
	struct rlimit before, after;
	pid_t pid;

	assert(!getrlimit(RLIMIT_AS, &before));
	pid = fork();
	assert(pid >= 0);
	if (!pid) {
		uint64_t limit;
		void *memory;
		assert(task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
				 (task_info_t) &info, &count) == KERN_SUCCESS);
		limit = (info.virtual_size + 33 * MIB - 1) / MIB;
		assert(!darwin_limits_apply(0, limit));
		errno = 0;
		memory = mmap(NULL, 64 * MIB, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANON, -1, 0);
		assert(memory == MAP_FAILED && errno == ENOMEM);
		_exit(0);
	}
	assert(_wait(pid) == 0);
	assert(!getrlimit(RLIMIT_AS, &after));
	assert(before.rlim_cur == after.rlim_cur);
	assert(before.rlim_max == after.rlim_max);
}

static void _inheritance(const char *self)
{
	struct rlimit before, after;
	pid_t pid;

	assert(!getrlimit(RLIMIT_CPU, &before));
	pid = fork();
	assert(pid >= 0);
	if (!pid) {
		struct rlimit lower;
		assert(!darwin_limits_apply(5, 0));
		assert(!getrlimit(RLIMIT_CPU, &lower));
		/* A second, less restrictive configuration must not raise it. */
		assert(!darwin_limits_apply(10, 0));
		assert(!getrlimit(RLIMIT_CPU, &after));
		assert(lower.rlim_max == after.rlim_max);
		pid = fork();
		assert(pid >= 0);
		if (!pid) {
			execl(self, self, "verify-cpu", NULL);
			_exit(127);
		}
		_exit(_wait(pid) == 0 ? 0 : 1);
	}
	assert(_wait(pid) == 0);
	assert(!getrlimit(RLIMIT_CPU, &after));
	assert(before.rlim_cur == after.rlim_cur);
	assert(before.rlim_max == after.rlim_max);
}

static void _cpu_time(void)
{
	pid_t pid = fork();
	int status;

	assert(pid >= 0);
	if (!pid) {
		volatile unsigned long value = 1;
		assert(!darwin_limits_apply(1, 0));
		/* Bound a broken kernel/API test by wall time as well. */
		alarm(15);
		while (1)
			value = value * 33 + 1;
	}
	status = _wait(pid);
	assert(WIFSIGNALED(status));
	assert(WTERMSIG(status) == SIGKILL || WTERMSIG(status) == SIGXCPU);
}

int main(int argc, char **argv)
{
	struct rlimit limit;

	if (argc == 2 && !strcmp(argv[1], "verify-cpu")) {
		assert(!getrlimit(RLIMIT_CPU, &limit));
		assert(limit.rlim_max <= 5 && limit.rlim_cur <= 5);
		return 0;
	}
	assert(argc == 1);
	assert(!darwin_limits_validate(0, 0));
	assert(darwin_limits_validate(UINT64_MAX, 0) == EOVERFLOW);
	assert(darwin_limits_validate(0, UINT64_MAX) == EOVERFLOW);
	assert(!darwin_limits_apply(0, 0));
	_address_space();
	_inheritance(argv[0]);
	_cpu_time();
	puts("Native resource limit tests passed");
	return 0;
}
