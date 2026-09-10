/*****************************************************************************\
 * conmgr-delay-test.c - delayed work ordering, cancellation and shutdown
 *****************************************************************************
 * This file is part of Slurm. See <https://slurm.schedmd.com/>.
 * Distributed under the GNU General Public License, version 2 or later.
\*****************************************************************************/

#include "config.h"

#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "src/common/fd.h"
#include "src/common/log.h"
#include "src/common/read_config.h"
#include "src/common/slurm_time.h"
#include "src/common/threadpool.h"
#include "src/common/workerpool.h"
#include "src/conmgr/conmgr.h"

static atomic_int completed, cancelled, failed, connections;
static struct timespec started;

static void _maybe_shutdown(void)
{
	if ((atomic_load(&completed) == 3) && (atomic_load(&connections) == 2))
		conmgr_request_shutdown();
}

static void *_connected(conmgr_callback_args_t args, void *arg)
{
	(void) args;
	atomic_fetch_add(&connections, 1);
	_maybe_shutdown();
	return arg;
}

static int _on_data(conmgr_callback_args_t args, void *arg)
{
	(void) args;
	(void) arg;
	return SLURM_SUCCESS;
}

static int _register_sockets(void)
{
	static const conmgr_events_t events = {
		.on_connection = _connected,
		.on_data = _on_data,
	};
	static int context;
	struct sockaddr_in address = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	socklen_t length = sizeof(address);
	int listener = slurm_socket(AF_INET, SOCK_STREAM, 0);
	int peer = slurm_socket(AF_INET, SOCK_STREAM, 0);

	if ((listener < 0) || (peer < 0) ||
	    bind(listener, (struct sockaddr *) &address, sizeof(address)) ||
	    listen(listener, 1) ||
	    getsockname(listener, (struct sockaddr *) &address, &length) ||
	    connect(peer, (struct sockaddr *) &address, length))
		return SLURM_ERROR;
	/* Exercise native detection of both listening and connected sockets. */
	if (conmgr_process_fd_listen(listener, CON_TYPE_RAW,
				     &conmgr_timeouts_disabled, &events, 0,
				     &context) ||
	    conmgr_process_fd(CON_TYPE_RAW, &conmgr_timeouts_disabled, peer,
			      peer, &events, 0, NULL, 0, NULL, &context))
		return SLURM_ERROR;
	return SLURM_SUCCESS;
}

static void _delayed(conmgr_callback_args_t args, void *arg)
{
	long minimum_ms = (long) arg;
	struct timespec now;
	long elapsed_ms;

	if (args.status == CONMGR_WORK_STATUS_CANCELLED) {
		atomic_fetch_add(&cancelled, 1);
		if (minimum_ms != 3600000)
			atomic_store(&failed, 1);
		return;
	}
	clock_gettime(CLOCK_MONOTONIC, &now);
	elapsed_ms = (now.tv_sec - started.tv_sec) * 1000 +
		     (now.tv_nsec - started.tv_nsec) / 1000000;
	if ((elapsed_ms < minimum_ms - 1) || (minimum_ms == 3600000))
		atomic_store(&failed, 1);
	atomic_fetch_add(&completed, 1);
	_maybe_shutdown();
}

static void *_watchdog(void *arg)
{
	struct timespec remaining = { .tv_sec = 10 };

	(void) arg;
	while (nanosleep(&remaining, &remaining) && (errno == EINTR))
		;
	/* A dropped timer would otherwise wait for the one-hour work item. */
	_exit(EXIT_FAILURE);
}

int main(void)
{
	log_options_t opts = LOG_OPTS_INITIALIZER;
	pthread_t watchdog;

	if (pthread_create(&watchdog, NULL, _watchdog, NULL))
		return EXIT_FAILURE;
	pthread_detach(watchdog);
	log_init("conmgr-delay-test", opts, 0, NULL);
	closeall_init();
	slurm_conf.msg_timeout = 10;
	threadpool_init(0, NULL);
	workerpool_init(2, 2, NULL);
	conmgr_init(0);
	if (_register_sockets())
		return EXIT_FAILURE;
	clock_gettime(CLOCK_MONOTONIC, &started);
	/* Inserting an earlier deadline must replace the timer's first deadline. */
	conmgr_add_work_delayed_fifo(_delayed, (void *) 150L, 0, 150000000);
	conmgr_add_work_delayed_fifo(_delayed, (void *) 30L, 0, 30000000);
	conmgr_add_work_delayed_fifo(_delayed, (void *) 80L, 0, 80000000);
	conmgr_add_work_delayed_fifo(_delayed, (void *) 3600000L, 3600, 0);
	if (conmgr_run(true))
		return EXIT_FAILURE;
	conmgr_fini();
	workerpool_fini();
	threadpool_fini();
	log_fini();
	if (atomic_load(&failed) || (atomic_load(&completed) != 3) ||
	    (atomic_load(&cancelled) != 1) ||
	    (atomic_load(&connections) != 2)) {
		fprintf(stderr,
			"delayed work: completed=%d cancelled=%d failed=%d\n",
			atomic_load(&completed), atomic_load(&cancelled),
			atomic_load(&failed));
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
