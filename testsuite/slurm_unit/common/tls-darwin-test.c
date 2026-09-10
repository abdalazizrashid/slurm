/*****************************************************************************\
 * tls-darwin-test.c - native TLS transports must preserve SIGPIPE disposition.
 * This file is part of Slurm. Distributed under GPL version 2 or later.
\*****************************************************************************/

#include "config.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>

#include "src/plugins/tls/s2n/tls_s2n.c"

/* This test never enters daemon certificate-path configuration. */
slurmd_conf_t *conf;

#define CHECK(expr) \
	do { \
		if (!(expr)) { \
			fprintf(stderr, "%s:%d: %s failed\n", __FILE__, \
				__LINE__, #expr); \
			exit(EXIT_FAILURE); \
		} \
	} while (0)

static void _check_broken_transport(tls_conn_t *conn, bool socket_transport)
{
	int pair[2], received;
	sigset_t pipe_signal, previous_mask, pending;
	struct sigaction disposition;

	if (socket_transport)
		CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
	else
		CHECK(!pipe(pair));
	/* Both initial attachment and later transport replacement use this. */
	CHECK(!_set_write_fd(conn, pair[1]));
	CHECK(fcntl(pair[1], F_GETNOSIGPIPE) == 1);
	CHECK(!tls_p_set_conn_fds(conn, pair[0], pair[1]));
	close(pair[0]);

	/* Default disposition would terminate this process without the flag. */
	CHECK(write(pair[1], "x", 1) == -1 && errno == EPIPE);
	CHECK(!sigaction(SIGPIPE, NULL, &disposition));
	CHECK(disposition.sa_handler == SIG_DFL);

	/* A signal already pending belongs to the caller and must remain. */
	sigemptyset(&pipe_signal);
	sigaddset(&pipe_signal, SIGPIPE);
	CHECK(!pthread_sigmask(SIG_BLOCK, &pipe_signal, &previous_mask));
	CHECK(!pthread_kill(pthread_self(), SIGPIPE));
	CHECK(write(pair[1], "x", 1) == -1 && errno == EPIPE);
	CHECK(!sigpending(&pending));
	CHECK(sigismember(&pending, SIGPIPE) == 1);
	CHECK(!sigwait(&pipe_signal, &received) && received == SIGPIPE);
	CHECK(!pthread_sigmask(SIG_SETMASK, &previous_mask, NULL));
	close(pair[1]);
}

int main(void)
{
	tls_conn_t conn = { .magic = TLS_CONN_MAGIC };

	alarm(10);
	CHECK(signal(SIGPIPE, SIG_DFL) != SIG_ERR);
	CHECK(!s2n_init());
	CHECK((conn.s2n_conn = s2n_connection_new(S2N_CLIENT)));
	_check_broken_transport(&conn, false);
	_check_broken_transport(&conn, true);
	CHECK(_set_write_fd(&conn, -1) == SLURM_ERROR);
	CHECK(!s2n_connection_free(conn.s2n_conn));
	CHECK(!s2n_cleanup());
	puts("Native TLS pipe/socket EPIPE, replacement and pending SIGPIPE checks passed");
	return EXIT_SUCCESS;
}
