/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Epilog ceilings affect only bounded children created by this test. */
#include "config.h"
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "src/common/darwin_launch.h"

#define MIB (UINT64_C(1024) * 1024)

static struct rlimit cpu_before;
#ifdef RLIMIT_AS
static struct rlimit address_before;
static uint64_t address_mib = 512;
#endif

static void _unchanged(void)
{
	struct rlimit actual;

	assert(!getrlimit(RLIMIT_CPU, &actual));
	assert(actual.rlim_cur == cpu_before.rlim_cur);
	assert(actual.rlim_max == cpu_before.rlim_max);
#ifdef RLIMIT_AS
	assert(!getrlimit(RLIMIT_AS, &actual));
	assert(actual.rlim_cur == address_before.rlim_cur);
	assert(actual.rlim_max == address_before.rlim_max);
#endif
}

static void _ceiling(int resource, const struct rlimit *before, rlim_t value)
{
	struct rlimit actual;
	rlim_t maximum = before->rlim_max < value ? before->rlim_max : value;
	rlim_t current = before->rlim_cur < maximum ? before->rlim_cur : maximum;

	assert(!getrlimit(resource, &actual));
	assert(actual.rlim_cur == current);
	assert(actual.rlim_max == maximum);
}

static void _child(int apply)
{
	pid_t pid = fork(), got;
	int status;

	assert(pid >= 0);
	if (!pid) {
		alarm(5);
		assert(!darwin_launch_apply_epilog());
		if (apply) {
			_ceiling(RLIMIT_CPU, &cpu_before, 60);
#ifdef RLIMIT_AS
			_ceiling(RLIMIT_AS, &address_before, address_mib * MIB);
#endif
		} else {
			_unchanged();
		}
		_exit(0);
	}
	do {
		got = waitpid(pid, &status, 0);
	} while (got < 0 && errno == EINTR);
	assert(got == pid);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(void)
{
#if defined(__APPLE__) && defined(RLIMIT_AS)
	mach_task_basic_info_data_t info;
	mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;

	/* Leave room above the mappings already owned by this test image. */
	assert(task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
			 (task_info_t) &info, &count) == KERN_SUCCESS);
	assert(info.virtual_size <= UINT64_MAX - 33 * MIB);
	address_mib = (info.virtual_size + 33 * MIB - 1) / MIB;
#endif
	assert(!getrlimit(RLIMIT_CPU, &cpu_before));
#ifdef RLIMIT_AS
	assert(!getrlimit(RLIMIT_AS, &address_before));
#endif
	/* Invalid ceilings fail before changing either resource limit. */
	darwin_launch_prepare_limits(UINT64_MAX, 0);
	assert(darwin_launch_apply_epilog() == EOVERFLOW);
	_unchanged();
	darwin_launch_prepare_limits(0, UINT64_MAX);
	assert(darwin_launch_apply_epilog() == EOVERFLOW);
	_unchanged();

	/* Parent preparation does not lower the daemon's limits. */
#ifdef RLIMIT_AS
	darwin_launch_prepare_limits(60, address_mib);
#else
	darwin_launch_prepare_limits(60, 0);
#endif
	_unchanged();
	_child(1);
	_unchanged();

	/* Resetting the stored policy leaves a later epilog unchanged. */
	darwin_launch_prepare_limits(0, 0);
	_child(0);
	_unchanged();
	puts("Epilog CPU/address-space ceilings and reset tests passed");
	return 0;
}
