/*****************************************************************************\
 * conmgr-signals-fork-test.c - signal registry snapshots across concurrent fork
 *****************************************************************************
 * This file is part of Slurm. See <https://slurm.schedmd.com/>.
 * Distributed under the GNU General Public License, version 2 or later.
\*****************************************************************************/

#include "config.h"

#include <stdatomic.h>
#include <sys/wait.h>
#include <unistd.h>

#include "src/common/xmalloc.h"

static bool in_child;

static void _checked_free(void **pointer)
{
	/* The production child handler must not enter the heap allocator. */
	if (in_child)
		_exit(20);
	slurm_xfree(pointer);
}

#define slurm_xfree _checked_free
#include "src/conmgr/signals.c"
#undef slurm_xfree

static atomic_bool stopping, ready;
static const int test_signals[] = { SIGUSR1, SIGUSR2, SIGWINCH };

static void _mark_child(void)
{
	in_child = true;
}

static void *_churn(void *arg)
{
	while (!atomic_load(&stopping)) {
		slurm_rwlock_wrlock(&lock);
		for (size_t i = 0; i < sizeof(test_signals) / sizeof(int); i++)
			_register_signal_handler(test_signals[i]);
		atomic_store(&ready, true);
		_reset_all_signal_handlers();
		slurm_rwlock_unlock(&lock);
	}
	return NULL;
}

int main(void)
{
	log_options_t opts = LOG_OPTS_INITIALIZER;
	struct sigaction ignore = { .sa_handler = SIG_IGN };
	pthread_t thread;

	alarm(20);
	log_init("conmgr-signals-fork-test", opts, 0, NULL);
	closeall_init();
	for (size_t i = 0; i < sizeof(test_signals) / sizeof(int); i++) {
		if (sigaction(test_signals[i], &ignore, NULL))
			return 1;
	}
	if (pthread_atfork(NULL, NULL, _mark_child) ||
	    pthread_atfork(_atfork_prepare, _atfork_parent, _atfork_child) ||
	    pthread_create(&thread, NULL, _churn, NULL))
		return 1;
	while (!atomic_load(&ready))
		usleep(1000);

	for (int i = 0; i < 100; i++) {
		int status;
		pid_t pid = fork();

		if (pid < 0)
			return 1;
		if (!pid) {
			struct sigaction current;

			if (signal_handlers || signal_handler_count ||
			    signal_work || signal_work_count || signal_fd != -1)
				_exit(21);
			for (size_t n = 0;
			     n < sizeof(test_signals) / sizeof(int); n++) {
				if (sigaction(test_signals[n], NULL,
					      &current) ||
				    current.sa_handler != SIG_IGN)
					_exit(22);
			}
			if (pthread_rwlock_trywrlock(&lock))
				_exit(23);
			pthread_rwlock_unlock(&lock);
			if (pthread_mutex_trylock(&registry_mutex))
				_exit(24);
			pthread_mutex_unlock(&registry_mutex);
			_exit(0);
		}
		if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
		    WEXITSTATUS(status)) {
			fprintf(stderr, "fork snapshot failed: status=%d\n",
				status);
			return 1;
		}
	}
	atomic_store(&stopping, true);
	if (pthread_join(thread, NULL))
		return 1;
	log_fini();
	return 0;
}
