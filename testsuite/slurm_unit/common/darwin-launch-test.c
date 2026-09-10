/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Real image limits affect only bounded children created by this test. */
#include "config.h"
#undef NDEBUG
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "src/common/darwin_launch.h"

extern char **environ;

static void *_fake_dlsym(void *, const char *);
static int _fake_sysctl(const char *, void *, size_t *, void *, size_t);
static int _fake_init(posix_spawnattr_t *);
static int _fake_flags(posix_spawnattr_t *, short);
static int _fake_destroy(posix_spawnattr_t *);
static int _fake_spawn(pid_t *, const char *,
		       const posix_spawn_file_actions_t *,
		       const posix_spawnattr_t *, char *const[], char *const[]);
#define dlsym _fake_dlsym
#define sysctlbyname _fake_sysctl
#define posix_spawnattr_init _fake_init
#define posix_spawnattr_setflags _fake_flags
#define posix_spawnattr_destroy _fake_destroy
#define posix_spawn _fake_spawn
#define darwin_launch_probe fake_launch_probe
#define darwin_launch_prepare fake_launch_prepare
#define darwin_launch_configured fake_launch_configured
#define darwin_launch_exec fake_launch_exec
#define darwin_launch_prepare_limits fake_launch_prepare_limits
#define darwin_launch_apply_epilog fake_launch_apply_epilog
#include "src/common/darwin_launch.c"
#undef darwin_launch_apply_epilog
#undef darwin_launch_prepare_limits
#undef darwin_launch_exec
#undef darwin_launch_configured
#undef darwin_launch_prepare
#undef darwin_launch_probe
#undef posix_spawn
#undef posix_spawnattr_destroy
#undef posix_spawnattr_setflags
#undef posix_spawnattr_init
#undef sysctlbyname
#undef dlsym

enum fault {
	OK,
	NO_API,
	DISABLED,
	SYSCTL_ERROR,
	INIT_ERROR,
	FLAGS_ERROR,
	ATTR_ERROR,
	SPAWN_ERROR
};
static enum fault fault;
static unsigned initialized, destroyed, spawned;

static void _epilog_failures(void)
{
	assert(!fake_launch_configured());
	fake_launch_prepare_limits(UINT64_MAX, 0);
	assert(fake_launch_apply_epilog() == EOVERFLOW);
	fake_launch_prepare_limits(0, UINT64_MAX);
	assert(fake_launch_apply_epilog() == EOVERFLOW);
	fake_launch_prepare_limits(0, 0);
	assert(!fake_launch_apply_epilog());
	assert(!fake_launch_configured());
}

static int _fake_jetsam(posix_spawnattr_t *attr, short flags, int priority,
			int active, int inactive)
{
	assert(attr && flags == (JETSAM_FATAL_ACTIVE | JETSAM_FATAL_INACTIVE));
	assert(priority == -1 && active == 32 && inactive == 32);
	return fault == ATTR_ERROR ? EACCES : 0;
}

static void *_fake_dlsym(void *handle, const char *symbol)
{
	assert(handle == RTLD_DEFAULT);
	assert(!strcmp(symbol, "posix_spawnattr_setjetsam_ext"));
	return fault == NO_API ? NULL : (void *) _fake_jetsam;
}

static int _fake_sysctl(const char *name, void *value, size_t *size,
			void *new_value, size_t new_size)
{
	assert(!strcmp(name, "kern.memorystatus_highwater_enabled"));
	assert(*size == sizeof(int) && !new_value && !new_size);
	if (fault == DISABLED) {
		*(int *) value = 0;
		return 0;
	}
	errno = fault == SYSCTL_ERROR ? EACCES : ENOENT;
	return -1;
}

static int _fake_init(posix_spawnattr_t *attr)
{
	assert(attr);
	if (fault == INIT_ERROR)
		return ENOMEM;
	initialized++;
	return 0;
}

static int _fake_flags(posix_spawnattr_t *attr, short flags)
{
	assert(attr && flags == POSIX_SPAWN_SETEXEC);
	return fault == FLAGS_ERROR ? EINVAL : 0;
}

static int _fake_destroy(posix_spawnattr_t *attr)
{
	assert(attr);
	destroyed++;
	return 0;
}

static int _fake_spawn(pid_t *pid, const char *path,
		       const posix_spawn_file_actions_t *actions,
		       const posix_spawnattr_t *attr, char *const argv[],
		       char *const env[])
{
	assert(pid && path && !actions && attr && argv && env);
	spawned++;
	return fault == SPAWN_ERROR ? ENOEXEC : 0;
}

static void _failures(void)
{
	const int expected[] = { 0,      ENOTSUP, ENOTSUP, EACCES,
				 ENOMEM, EINVAL,  EACCES,  0 };
	char *args[] = { "unused", NULL };

	for (unsigned i = 0; i < sizeof(expected) / sizeof(*expected); i++) {
		fault = i;
		initialized = destroyed = spawned = 0;
		assert(fake_launch_probe(32) == expected[i]);
		assert(initialized == destroyed && !spawned);
	}
	fault = OK;
	assert(!fake_launch_prepare(32));
	assert(fake_launch_configured());
	fault = ATTR_ERROR;
	spawned = 0;
	assert(fake_launch_exec("unused", args, environ) == EACCES);
	assert(!spawned); /* Attribute failure cannot fall back to execve. */
	fault = SPAWN_ERROR;
	assert(fake_launch_exec("unused", args, environ) == ENOEXEC);
	fault = OK;
	assert(fake_launch_exec("unused", args, environ) == EIO);
	assert(initialized == destroyed);
	assert(fake_launch_prepare(UINT64_MAX) == EOVERFLOW);
	assert(fake_launch_configured()); /* Failed reconfiguration retains policy. */
	assert(!fake_launch_prepare(0));
	assert(!fake_launch_configured());
}

typedef struct {
	pid_t pid;
	unsigned allocated_mib;
} observation_t;

static char self[PATH_MAX];

static void _allocate(int fd)
{
	const size_t bytes = 96 * 1024 * 1024;
	volatile unsigned char *memory;

	alarm(8);
	memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
		      MAP_ANON | MAP_PRIVATE, -1, 0);
	assert(memory != MAP_FAILED);
	for (size_t offset = 0; offset < bytes; offset += 4096) {
		memory[offset] = 1;
		if ((offset + 4096) % (8 * 1024 * 1024) == 0) {
			observation_t event = {
				getpid(), (offset + 4096) / (1024 * 1024)
			};
			assert(write(fd, &event, sizeof(event)) ==
			       sizeof(event));
			usleep(10000);
		}
	}
	assert(!munmap((void *) memory, bytes));
}

static int _wait(pid_t pid)
{
	struct timespec start, now;
	int status;

	assert(!clock_gettime(CLOCK_MONOTONIC, &start));
	for (;;) {
		pid_t got = waitpid(pid, &status, WNOHANG);
		if (got == pid)
			return status;
		assert(!got || errno == EINTR);
		assert(!clock_gettime(CLOCK_MONOTONIC, &now));
		if (now.tv_sec - start.tv_sec > 12) {
			/* Signal only the unreaped child we created. */
			kill(pid, SIGKILL);
			while (waitpid(pid, &status, 0) < 0)
				assert(errno == EINTR);
			assert(!"child timed out");
		}
		usleep(10000);
	}
}

static void _image(const char *mode, unsigned mib, bool killed)
{
	int gate[2], status;
	pid_t pid;
	observation_t event = { 0 }, last = { 0 };
	char fd[32];

	assert(!pipe(gate));
	snprintf(fd, sizeof(fd), "%d", gate[1]);
	pid = fork();
	assert(pid >= 0);
	if (!pid) {
		char *args[] = { self, (char *) mode, fd, NULL };
		close(gate[0]);
		alarm(10);
		assert(!darwin_launch_prepare(mib));
		if (!strcmp(mode, "--shell")) {
			char *shell[] = { "/bin/sh",
					  "-c",
					  "exec \"$1\" --allocate \"$2\"",
					  "sh",
					  self,
					  fd,
					  NULL };
			_exit(darwin_launch_exec("/bin/sh", shell, environ));
		}
		_exit(darwin_launch_exec(self, args, environ));
	}
	close(gate[1]);
	status = _wait(pid);
	while (read(gate[0], &event, sizeof(event)) == sizeof(event))
		last = event;
	close(gate[0]);
	if (killed) {
		assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
		assert(last.allocated_mib > 0 && last.allocated_mib < mib);
	} else {
		assert(WIFEXITED(status) && !WEXITSTATUS(status));
		assert(last.allocated_mib == 96);
	}
	assert(last.pid > 0);
	if (strcmp(mode, "--fork"))
		assert(last.pid ==
		       pid); /* SETEXEC preserves the tracked task PID. */
	else
		assert(last.pid != pid);
	printf("%s limit=%u status=%d final_allocation=%u MiB\n", mode, mib,
	       status, last.allocated_mib);
}

int main(int argc, char **argv)
{
	assert(realpath(argv[0], self));
	if (argc == 3) {
		int fd = atoi(argv[2]);
		if (!strcmp(argv[1], "--fork")) {
			pid_t pid = fork();
			assert(pid >= 0);
			if (!pid) {
				_allocate(fd);
				_exit(0);
			}
			assert(_wait(pid) == 0);
		} else if (!strcmp(argv[1], "--exec")) {
			execl(self, self, "--allocate", argv[2], NULL);
			return 127;
		} else {
			assert(!strcmp(argv[1], "--allocate"));
			_allocate(fd);
		}
		close(fd);
		return 0;
	}
	assert(argc == 1);
	_failures();
	_epilog_failures();
	assert(!darwin_launch_configured());
	_image("--allocate", 0, false);
	_image("--allocate", 32, true);
	_image("--allocate", 64, true);
	_image("--fork", 32, false);
	_image("--exec", 32, false);
	_image("--shell", 32, false);
	assert(!darwin_launch_configured());
	puts("Initial-image footprint, error propagation and lifecycle tests passed");
	return 0;
}
