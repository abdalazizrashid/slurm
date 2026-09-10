/*****************************************************************************\
 * fd-test.c - descriptor flags and concurrent fork/exec regression tests
 *****************************************************************************
 * This file is part of Slurm. See <https://slurm.schedmd.com/>.
 * Distributed under the GNU General Public License, version 2 or later.
\*****************************************************************************/

#include "config.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "src/common/fd.h"
#include "src/common/net.h"

#define CHECK(expr) \
	do { \
		if (!(expr)) { \
			fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__, \
				#expr, strerror(errno)); \
			exit(EXIT_FAILURE); \
		} \
	} while (0)

static atomic_bool stop;

static void *_create(void *arg)
{
	(void) arg;
	while (!atomic_load(&stop)) {
		char template[] = "/tmp/slurm-fd-test-XXXXXX";
		int fd, pipefd[2], temporary;

		CHECK(!slurm_pipe(pipefd, O_CLOEXEC));
		CHECK((fd = slurm_socket(AF_INET, SOCK_STREAM, 0)) >= 0);
		CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC);
		CHECK(fcntl(pipefd[0], F_GETFD) & FD_CLOEXEC);
		CHECK(fcntl(pipefd[1], F_GETFD) & FD_CLOEXEC);
		CHECK((temporary = slurm_mkstemp(template)) >= 0);
		CHECK(fcntl(temporary, F_GETFD) & FD_CLOEXEC);
		CHECK(!unlink(template));
		close(temporary);
		close(fd);
		close(pipefd[0]);
		close(pipefd[1]);
	}
	return NULL;
}

static void *_accept_until_cancelled(void *arg)
{
	int listener = *(int *) arg;

	while (true) {
		int fd = slurm_accept(listener, NULL, NULL, false);

		if (fd >= 0)
			close(fd);
		pthread_testcancel();
	}
	return NULL;
}

static void _peer_creds(void)
{
#if defined(__linux__) || defined(__APPLE__)
	int pair[2];
	uid_t uid;
	gid_t gid;
	pid_t pid;

	/* No creation thread is running yet; these sockets never cross exec. */
	CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
	CHECK(!net_get_peer(pair[0], &uid, &gid, &pid));
	CHECK((uid == geteuid()) && (gid == getegid()) && (pid == getpid()));
	close(pair[0]);
	close(pair[1]);
	CHECK(net_get_peer(-1, &uid, &gid, &pid));
	CHECK(pid == 0);
#endif
}

static void _accept_flags(void)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	socklen_t len = sizeof(addr);
	int listener, client, accepted;

	CHECK((listener = slurm_socket(AF_INET, SOCK_STREAM, 0)) >= 0);
	CHECK(!bind(listener, (struct sockaddr *) &addr, sizeof(addr)));
	CHECK(!getsockname(listener, (struct sockaddr *) &addr, &len));
	CHECK(!listen(listener, 2));
	CHECK(!fcntl(listener, F_SETFL, O_NONBLOCK));
	for (int nonblocking = 0; nonblocking <= 1; nonblocking++) {
		struct pollfd ready = { .fd = listener, .events = POLLIN };

		CHECK((client = slurm_socket(AF_INET, SOCK_STREAM, 0)) >= 0);
		CHECK(!connect(client, (struct sockaddr *) &addr,
			       sizeof(addr)));
		/* The client handshake can finish before accept becomes ready. */
		CHECK(poll(&ready, 1, 2000) == 1);
		CHECK(ready.revents & POLLIN);
		CHECK((accepted = slurm_accept(listener, NULL, NULL,
					       nonblocking)) >= 0);
		CHECK(fcntl(accepted, F_GETFD) & FD_CLOEXEC);
		CHECK(!!(fcntl(accepted, F_GETFL) & O_NONBLOCK) == nonblocking);
		CHECK(fcntl(listener, F_GETFL) & O_NONBLOCK);
		close(client);
		close(accepted);
	}
	CHECK(slurm_accept(listener, NULL, NULL, false) == -1);
	CHECK((errno == EAGAIN) || (errno == EWOULDBLOCK));
	/* Cancelling an accept thread must not leave fork/creation locked. */
	{
		pthread_t thread;
		void *result;
		struct timespec delay = { .tv_nsec = 10000000 };

		CHECK(!pthread_create(&thread, NULL, _accept_until_cancelled,
				      &listener));
		nanosleep(&delay, NULL);
		CHECK(!pthread_cancel(thread));
		CHECK(!pthread_join(thread, &result));
		CHECK(result == PTHREAD_CANCELED);
	}
	close(listener);
}

#ifdef __APPLE__
static void _high_limit_closeall(void)
{
	int status;
	pid_t child = fork();

	CHECK(child >= 0);
	if (!child) {
		struct rlimit limit;
		int descriptors[600], high, preserve, skipped[3];
		struct timespec start, end;
		pid_t worker;

		alarm(5);
		CHECK(!getrlimit(RLIMIT_NOFILE, &limit));
		limit.rlim_cur = INT_MAX;
		CHECK(!setrlimit(RLIMIT_NOFILE, &limit));
		closeall_init();
		for (int i = 0; i < 600; i++)
			CHECK((descriptors[i] = open("/dev/null", O_RDONLY)) >=
			      0);
		CHECK((high = fcntl(descriptors[0], F_DUPFD, 4096)) >= 4096);
		CHECK((preserve = fcntl(descriptors[0], F_DUPFD, 8192)) >=
		      8192);
		skipped[0] = descriptors[300];
		skipped[1] = preserve;
		skipped[2] = -1;
		/* Descriptors above a new soft limit are still inherited. */
		limit.rlim_cur = 128;
		CHECK(!setrlimit(RLIMIT_NOFILE, &limit));
		CHECK((worker = fork()) >= 0);
		if (!worker) {
			CHECK(!clock_gettime(CLOCK_MONOTONIC, &start));
			closeall_except(3, skipped);
			for (int i = 0; i < 600; i++) {
				if (i == 300)
					CHECK(fcntl(descriptors[i], F_GETFD) >=
					      0);
				else
					CHECK(fcntl(descriptors[i], F_GETFD) ==
						      -1 &&
					      errno == EBADF);
			}
			CHECK(fcntl(high, F_GETFD) == -1 && errno == EBADF);
			CHECK(fcntl(preserve, F_GETFD) >= 0);
			closeall(3);
			CHECK(fcntl(skipped[0], F_GETFD) == -1 &&
			      errno == EBADF);
			CHECK(fcntl(preserve, F_GETFD) == -1 && errno == EBADF);
			CHECK(!clock_gettime(CLOCK_MONOTONIC, &end));
			CHECK((end.tv_sec - start.tv_sec) < 2);
			_exit(EXIT_SUCCESS);
		}
		CHECK(waitpid(worker, &status, 0) == worker);
		CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
		_exit(EXIT_SUCCESS);
	}
	CHECK(waitpid(child, &status, 0) == child);
	CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
}
#endif

int main(int argc, char **argv)
{
	pthread_t creators[4];
	int pipefd[2];

	if ((argc == 2) && !strcmp(argv[1], "--child")) {
		for (int fd = STDERR_FILENO + 1; fd < 256; fd++) {
			if ((fcntl(fd, F_GETFD) >= 0) || (errno != EBADF))
				return EXIT_FAILURE;
		}
		return EXIT_SUCCESS;
	}

	alarm(30);
	closeall_init();
#ifdef __APPLE__
	_high_limit_closeall();
#endif
	/* Isolate the inheritance check from descriptors owned by the runner. */
	for (int fd = STDERR_FILENO + 1; fd < 256; fd++)
		close(fd);
	CHECK(!slurm_pipe(pipefd, O_CLOEXEC | O_NONBLOCK));
	CHECK(fcntl(pipefd[0], F_GETFL) & O_NONBLOCK);
	CHECK(fcntl(pipefd[1], F_GETFL) & O_NONBLOCK);
	close(pipefd[0]);
	close(pipefd[1]);
	CHECK(slurm_pipe(pipefd, O_APPEND) == -1);
	CHECK(errno == EINVAL);
	CHECK(!slurm_pipe(pipefd, 0));
	CHECK(!(fcntl(pipefd[0], F_GETFD) & FD_CLOEXEC));
	CHECK(!(fcntl(pipefd[1], F_GETFD) & FD_CLOEXEC));
	close(pipefd[0]);
	close(pipefd[1]);
	_accept_flags();
	_peer_creds();

	for (int i = 0; i < 4; i++)
		CHECK(!pthread_create(&creators[i], NULL, _create, NULL));
	for (int i = 0; i < 100; i++) {
		pid_t child;
		int status;
		char *args[] = { argv[0], "--child", NULL };

		CHECK((child = fork()) >= 0);
		if (!child) {
			execv(argv[0], args);
			_exit(127);
		}
		CHECK(waitpid(child, &status, 0) == child);
		CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
	}
	atomic_store(&stop, true);
	for (int i = 0; i < 4; i++)
		CHECK(!pthread_join(creators[i], NULL));
	return EXIT_SUCCESS;
}
