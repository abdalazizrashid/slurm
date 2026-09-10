/*****************************************************************************\
 * conmgr-quiesce-test.c - delayed work across quiesce, resume and re-exec
 *****************************************************************************
 * This file is part of Slurm. See <https://slurm.schedmd.com/>.
 * Distributed under the GNU General Public License, version 2 or later.
\*****************************************************************************/

#include "config.h"

#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "src/common/fd.h"
#include "src/common/log.h"
#include "src/common/read_config.h"
#include "src/common/threadpool.h"
#include "src/common/workerpool.h"
#include "src/conmgr/conmgr.h"
#include "src/conmgr/mgr.h"

#define CHECK(expr) \
	do { \
		if (!(expr)) { \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, \
				#expr); \
			_exit(EXIT_FAILURE); \
		} \
	} while (0)

static atomic_int completed, failed;

static void _sleep_ms(long milliseconds)
{
	struct timespec remaining = {
		.tv_sec = milliseconds / 1000,
		.tv_nsec = (milliseconds % 1000) * 1000000,
	};

	while (nanosleep(&remaining, &remaining))
		CHECK(errno == EINTR);
}

static void _delayed(conmgr_callback_args_t args, void *arg)
{
	(void) arg;
	if (args.status == CONMGR_WORK_STATUS_CANCELLED)
		atomic_store(&failed, 1);
	atomic_fetch_add(&completed, 1);
}

static void _anchor(conmgr_callback_args_t args, void *arg)
{
	(void) arg;
	CHECK(args.status == CONMGR_WORK_STATUS_CANCELLED);
}

static void _init(void)
{
	log_options_t opts = LOG_OPTS_INITIALIZER;

	log_init("conmgr-quiesce-test", opts, 0, NULL);
	closeall_init();
	slurm_conf.msg_timeout = 10;
	threadpool_init(0, NULL);
	workerpool_init(2, 2, NULL);
	conmgr_init(0);
	/* Keep watch alive without racing a short deadline during startup. */
	conmgr_add_work_delayed_fifo(_anchor, NULL, 3600, 0);
	CHECK(!conmgr_run(false));
}

/* No manager worker may still touch fds after quiesce returns. */
static void _assert_quiesced(void)
{
	slurm_mutex_lock(&mgr.mutex);
	CHECK(!mgr.poll_active);
	CHECK(!mgr.inspecting);
	slurm_mutex_unlock(&mgr.mutex);
}

static void _resume(void)
{
	_init();
	for (int iteration = 0; iteration < 2; iteration++) {
		conmgr_quiesce(__func__);
		_assert_quiesced();
		conmgr_add_work_delayed_fifo(_delayed, NULL, 0, 300000000);
		CHECK(atomic_load(&completed) == iteration);
		/* The original deadline expires while execution is suspended. */
		_sleep_ms(600);
		CHECK(atomic_load(&completed) == iteration);
		conmgr_unquiesce(__func__);
		for (int wait = 0;
		     (wait < 200) && (atomic_load(&completed) == iteration);
		     wait++)
			_sleep_ms(10);
		CHECK(atomic_load(&completed) == iteration + 1);
	}
	conmgr_fini();
	workerpool_fini();
	threadpool_fini();
	log_fini();
	CHECK(!atomic_load(&failed) && (atomic_load(&completed) == 2));
}

#ifdef __APPLE__
static void _reexec(const char *self)
{
	_init();
	conmgr_quiesce(__func__);
	_assert_quiesced();
	conmgr_add_work_delayed_fifo(_delayed, NULL, 0, 300000000);
	/* Foreground slurmctld reconfiguration closes fds before execve(). */
	closeall(3);
	/* Let a surviving native timer thread observe its now-closed kqueue. */
	_sleep_ms(600);
	execl(self, self, "--after-exec", (char *) NULL);
	_exit(127);
}
#endif

static void _run_child(const char *self, const char *mode)
{
	pid_t child = fork();
	int status = 0;

	CHECK(child >= 0);
	if (!child) {
		execl(self, self, mode, (char *) NULL);
		_exit(127);
	}
	/* The parent owns the watchdog; conmgr uses SIGALRM in the child. */
	for (int wait = 0; wait < 1500; wait++) {
		pid_t result = waitpid(child, &status, WNOHANG);

		if (result == child) {
			if (!WIFEXITED(status) || WEXITSTATUS(status))
				fprintf(stderr, "%s failed: status=%d\n", mode,
					status);
			CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
			return;
		}
		CHECK((result == 0) || ((result < 0) && (errno == EINTR)));
		_sleep_ms(10);
	}
	(void) kill(child, SIGKILL);
	while ((waitpid(child, &status, 0) < 0) && (errno == EINTR))
		;
	fprintf(stderr, "%s timed out\n", mode);
	_exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
	if (argc == 2) {
		if (!strcmp(argv[1], "--after-exec"))
			return EXIT_SUCCESS;
		if (!strcmp(argv[1], "--resume")) {
			_resume();
			return EXIT_SUCCESS;
		}
#ifdef __APPLE__
		if (!strcmp(argv[1], "--reexec")) {
			_reexec(argv[0]);
			return EXIT_FAILURE;
		}
#endif
		return EXIT_FAILURE;
	}
	CHECK(argc == 1);
	_run_child(argv[0], "--resume");
#ifdef __APPLE__
	_run_child(argv[0], "--reexec");
#endif
	return EXIT_SUCCESS;
}
