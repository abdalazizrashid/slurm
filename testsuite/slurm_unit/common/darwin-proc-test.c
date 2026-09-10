/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "src/plugins/jobacct_gather/darwin/darwin_proc.h"

#define REQUIRE(condition) \
	do { \
		if (!(condition)) { \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, \
				#condition); \
			return EXIT_FAILURE; \
		} \
	} while (0)

static uint64_t _monotonic_ns(void)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now))
		abort();
	return (uint64_t) now.tv_sec * 1000000000 + now.tv_nsec;
}

int main(void)
{
	darwin_proc_sample_t before, after, child_sample;
	const size_t allocation = 8 * 1024 * 1024;
	volatile char *memory;
	volatile unsigned long work = 1;
	uint64_t deadline, cpu_ns;
	clock_t cpu_deadline;
	int fds[2], status, child_rc;
	pid_t child;

	REQUIRE(darwin_proc_sample(0, &before) == EINVAL);
	REQUIRE(darwin_proc_sample(getpid(), NULL) == EINVAL);
	REQUIRE(darwin_proc_sample(getpid(), &before) == 0);
	REQUIRE(before.pid == getpid());
	REQUIRE(before.ppid == getppid());
	REQUIRE(before.start_sec > 0);
	REQUIRE(before.start_abstime > 0);
	REQUIRE(before.vsize >= before.rss);
	REQUIRE(before.rss > 0);
	REQUIRE(before.footprint > 0);

	memory = mmap(NULL, allocation, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANON, -1, 0);
	REQUIRE(memory != MAP_FAILED);
	for (size_t i = 0; i < allocation; i += getpagesize())
		memory[i] = 1;
	deadline = _monotonic_ns() + 2000000000;
	cpu_deadline = clock() + CLOCKS_PER_SEC / 10;
	while ((clock() < cpu_deadline) && (_monotonic_ns() < deadline))
		for (int i = 0; i < 1000; i++)
			work = work * 1664525 + 1013904223;
	REQUIRE(darwin_proc_sample(getpid(), &after) == 0);
	munmap((void *) memory, allocation);
	REQUIRE(after.start_sec == before.start_sec);
	REQUIRE(after.start_usec == before.start_usec);
	REQUIRE(after.start_abstime == before.start_abstime);
	REQUIRE(after.user_ns >= before.user_ns);
	REQUIRE(after.system_ns >= before.system_ns);
	cpu_ns = after.user_ns - before.user_ns + after.system_ns -
		 before.system_ns;
	REQUIRE(cpu_ns > 10000000);
	REQUIRE(cpu_ns < 1000000000);
	REQUIRE(after.footprint >= before.footprint + allocation / 2);

	REQUIRE(pipe(fds) == 0);
	child = fork();
	REQUIRE(child >= 0);
	if (!child) {
		char byte;
		close(fds[1]);
		(void) read(fds[0], &byte, 1);
		_exit(0);
	}
	close(fds[0]);
	child_rc = darwin_proc_sample(child, &child_sample);
	close(fds[1]);
	REQUIRE(waitpid(child, &status, 0) == child);
	REQUIRE(WIFEXITED(status) && !WEXITSTATUS(status));
	REQUIRE(child_rc == 0);
	REQUIRE(child_sample.pid == child);
	REQUIRE(child_sample.ppid == getpid());
	REQUIRE(darwin_proc_sample(child, &child_sample) == ESRCH);
	printf("Native sampler passed: CPU delta=%llu ns, footprint delta=%llu bytes\n",
	       (unsigned long long) cpu_ns,
	       (unsigned long long) (after.footprint - before.footprint));
	return EXIT_SUCCESS;
}
