/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config.h"
#undef NDEBUG
#include <assert.h>
#include <stdatomic.h>
#include <unistd.h>

#include "src/common/fd.h"
#include "src/common/slurm_protocol_api.h"

static void *test_accept(int fd, slurm_addr_t *addr);
#define slurm_accept_msg_conn test_accept
#include "src/srun/srun_pty.c"
#undef slurm_accept_msg_conn

/* Actual sockets/accept; inject transients and substitute only the TLS wrapper. */
static atomic_uint accept_calls;
static unsigned transient_count;
static atomic_bool entered, finished;

typedef struct {
	int fd;
} test_connection_t;

typedef struct {
	int listener, error;
	slurm_addr_t peer;
	conn_t *conn;
} waiting_t;

static void *test_accept(int fd, slurm_addr_t *addr)
{
	const int errors[] = { EAGAIN, EWOULDBLOCK, ECONNABORTED, EINTR };
	unsigned call = atomic_fetch_add(&accept_calls, 1);
	if (call < transient_count) {
		errno = errors[call];
		return NULL;
	}
	socklen_t length = sizeof(*addr);
	int accepted =
		slurm_accept(fd, (struct sockaddr *) addr, &length, false);
	if (accepted < 0)
		return NULL;
	test_connection_t *connection = malloc(sizeof(*connection));
	assert(connection);
	connection->fd = accepted;
	return connection;
}

/* Unused by the isolated accept loop, needed by the included full PTY source. */
extern srun_job_state_t srun_job_state(srun_job_t *job)
{
	return job->state;
}

static void *wait_for_connection(void *arg)
{
	waiting_t *waiting = arg;
	atomic_store(&entered, true);
	waiting->conn = _pty_accept(waiting->listener, &waiting->peer);
	waiting->error = errno;
	atomic_store(&finished, true);
	return NULL;
}

static int listener(slurm_addr_t *address)
{
	int fd;
	slurm_set_addr(address, 0, "127.0.0.1");
	assert((fd = slurm_init_msg_engine(address, false)) >= 0);
	assert(!slurm_get_stream_addr(fd, address));
	/* The production PTY loop must make even an initially blocking fd safe. */
	int flags = fcntl(fd, F_GETFL);
	assert(flags >= 0 && !fcntl(fd, F_SETFL, flags & ~O_NONBLOCK));
	return fd;
}

static void start(waiting_t *waiting)
{
	slurm_mutex_lock(&winch_lock);
	pty_shutdown = false;
	slurm_mutex_unlock(&winch_lock);
	atomic_store(&entered, false);
	atomic_store(&finished, false);
	atomic_store(&accept_calls, 0);
	pty_listen_fd = waiting->listener;
	assert(!pthread_create(&pty_tid, NULL, wait_for_connection, waiting));
	while (!atomic_load(&entered))
		usleep(1000);
}

int main(void)
{
	slurm_addr_t address;
	log_options_t options = LOG_OPTS_INITIALIZER;
	alarm(15);
	log_init("srun-pty-accept-test", options, 0, NULL);

	for (unsigned retries = 0; retries <= 4; retries += 4) {
		waiting_t waiting = { .listener = listener(&address) };
		transient_count = retries;
		start(&waiting);
		usleep(150000);
		assert(!atomic_load(&finished));
		assert(!atomic_load(&accept_calls));
		assert(address.ss_family == AF_INET);
		int client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		assert(client >= 0);
		assert(!connect(client, (struct sockaddr *) &address,
				sizeof(struct sockaddr_in)));
		assert(!pthread_join(pty_tid, NULL));
		pty_tid = 0;
		assert(waiting.conn);
		assert(atomic_load(&accept_calls) == retries + 1);
		test_connection_t *connection = (void *) waiting.conn;
		int accepted = connection->fd;
		assert(!(fcntl(accepted, F_GETFL) & O_NONBLOCK));
		assert(fcntl(accepted, F_GETFD) & FD_CLOEXEC);
		close(accepted);
		free(connection);
		close(client);
		close(waiting.listener);
	}

	waiting_t waiting = { .listener = listener(&address) };
	transient_count = 0;
	start(&waiting);
	usleep(150000);
	assert(!atomic_load(&finished));
	/* Actual production shutdown/join, with no client ever connecting. */
	pty_thread_fini();
	assert(atomic_load(&finished));
	assert(!waiting.conn && waiting.error == ECANCELED);
	assert(!pty_tid);
	close(waiting.listener);
	pty_listen_fd = -1;
	puts("PASS: delayed loopback connection, transient readiness retry, accepted fd flags and PTY shutdown before connect");
	return 0;
}
