/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config.h"
#include "src/common/slurm_protocol_socket.h"

static void *_test_accept(int, slurm_addr_t *);
#define slurm_accept_msg_conn _test_accept
#include "src/api/slurm_pmi.c"
#undef slurm_accept_msg_conn

#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#undef NDEBUG
#include <assert.h>

#ifdef __APPLE__
static int accepted = -1, attempts;
volatile static sig_atomic_t interrupted;

/* Mock only conn_t construction; the fd accept uses the actual native wrapper. */
static void *_test_accept(int listener, slurm_addr_t *address)
{
	socklen_t length = sizeof(*address);

	attempts++;
	if (attempts <= 2) {
		errno = attempts == 1 ? EAGAIN : ECONNABORTED;
		return NULL;
	}
	accepted = slurm_accept(listener, (struct sockaddr *) address, &length,
				false);
	return accepted < 0 ? NULL : &accepted;
}

static void _signal(int signo)
{
	(void) signo;
	interrupted++;
}

static double _now(void)
{
	struct timespec now;
	assert(!clock_gettime(CLOCK_MONOTONIC, &now));
	return now.tv_sec + now.tv_nsec / 1e9;
}

static void _advertised_address(const struct sockaddr_in *bound)
{
	slurm_addr_t address = { 0 };
	char hostname[HOST_NAME_MAX + 1], full[HOST_NAME_MAX + 1];
	const char *old = getenv("SLURM_PMI_RESP_IFHN");
	char *saved = old ? strdup(old) : NULL;

	assert(!unsetenv("SLURM_PMI_RESP_IFHN"));
	memcpy(&address, bound, sizeof(*bound));
	assert(!_reply_hostname(&address, hostname, sizeof(hostname)));
	assert(!strcmp(hostname, "127.0.0.1"));
	assert(!setenv("SLURM_PMI_RESP_IFHN", "explicit.example", 1));
	assert(!_reply_hostname(&address, hostname, sizeof(hostname)));
	assert(!strcmp(hostname, "explicit.example"));
	assert(_reply_hostname(&address, hostname, 2) == ENAMETOOLONG);
	assert(!unsetenv("SLURM_PMI_RESP_IFHN"));
	((struct sockaddr_in *) &address)->sin_addr.s_addr = htonl(INADDR_ANY);
	assert(!gethostname(full, sizeof(full)));
	assert(!_reply_hostname(&address, hostname, sizeof(hostname)));
	assert(!strcmp(hostname,
		       full)); /* Keep the .local or other domain suffix. */
	memset(&address, 0, sizeof(address));
	struct sockaddr_in6 *v6 = (void *) &address;
	v6->sin6_len = sizeof(*v6);
	v6->sin6_family = AF_INET6;
	v6->sin6_addr = in6addr_loopback;
	assert(!_reply_hostname(&address, hostname, sizeof(hostname)));
	assert(!strcmp(hostname, "::1"));
	v6->sin6_addr = in6addr_any;
	assert(!_reply_hostname(&address, hostname, sizeof(hostname)));
	assert(!strcmp(hostname, full));
	address.ss_family = AF_UNIX;
	assert(_reply_hostname(&address, hostname, sizeof(hostname)) ==
	       EAFNOSUPPORT);
	if (saved) {
		assert(!setenv("SLURM_PMI_RESP_IFHN", saved, 1));
		free(saved);
	}
}

int main(void)
{
	struct sockaddr_in address = { .sin_family = AF_INET,
				       .sin_addr.s_addr =
					       htonl(INADDR_LOOPBACK) };
	socklen_t length = sizeof(address);
	slurm_addr_t peer;
	struct sigaction action = { .sa_handler = _signal };
	int listener, status, flags;
	pid_t child;
	double start, elapsed;

	alarm(10);
	slurm_conf.msg_timeout = 1;
	sigemptyset(&action.sa_mask);
	assert(!sigaction(SIGUSR1, &action, NULL));
	assert((listener = socket(AF_INET, SOCK_STREAM, 0)) >= 0);
	assert(!bind(listener, (struct sockaddr *) &address, sizeof(address)));
	assert(!listen(listener, 1));
	assert(!getsockname(listener, (struct sockaddr *) &address, &length));
	_advertised_address(&address);
	assert((flags = fcntl(listener, F_GETFL)) >= 0);
	assert(!fcntl(listener, F_SETFL, flags | O_NONBLOCK));
	assert((child = fork()) >= 0);
	if (!child) {
		int client;
		close(listener);
		usleep(50000);
		assert(!kill(getppid(), SIGUSR1));
		/* A healthy rank can take longer than MessageTimeout to arrive. */
		usleep(1200000);
		assert((client = socket(AF_INET, SOCK_STREAM, 0)) >= 0);
		assert(!connect(client, (struct sockaddr *) &address,
				sizeof(address)));
		assert(!close(client));
		_exit(0);
	}
	start = _now();
	assert(_accept_reply(listener, &peer) == (conn_t *) &accepted);
	elapsed = _now() - start;
	assert(elapsed > slurm_conf.msg_timeout);
	assert(interrupted && attempts == 3 && accepted >= 0);
	assert(fcntl(listener, F_GETFL) & O_NONBLOCK);
	assert(fcntl(accepted, F_GETFD) & FD_CLOEXEC);
	assert(!(fcntl(accepted, F_GETFL) & O_NONBLOCK));
	assert(!close(accepted));
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status) && !WEXITSTATUS(status));

	assert(!close(listener));
	assert(!_accept_reply(listener, &peer) && errno == EBADF);
	assert(!_accept_reply(-1, &peer) && errno == EBADF);
	puts("PMI reply: bound IPv4/IPv6 callback, full wildcard hostname/override, collective arrival beyond MessageTimeout, EINTR/transient retries and CLOEXEC PASS");
	return 0;
}
#else
static void *_test_accept(int listener, slurm_addr_t *address)
{
	return slurm_accept_msg_conn(listener, address);
}

int main(void)
{
	return 77;
}
#endif
