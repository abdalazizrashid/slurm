/*****************************************************************************\
 * fetch_config-test.c - temporary configuration and script file lifetime
 *****************************************************************************
 * This file is part of Slurm. See <https://slurm.schedmd.com/>.
 * Distributed under the GNU General Public License, version 2 or later.
\*****************************************************************************/

#include "config.h"
#include "src/common/slurm_xlator.h"

/*
 * Exercise the child ownership handler without requiring real PID reuse.
 * Keep the included functions private to this test, including when linking
 * against the monolithic libslurm.o used without shared libraries.
 */
#define strong_alias(name, aliasname)
#undef dump_to_memfd
#undef close_memfd
#define fetch_config test_fetch_config
#define fetch_config_from_controller test_fetch_config_from_controller
#define dump_to_memfd test_dump_to_memfd
#define close_memfd test_close_memfd
#define chown_memfd_files test_chown_memfd_files
#define find_conf_by_name test_find_conf_by_name
#define write_one_config test_write_one_config
#define write_config_to_memfd test_write_config_to_memfd
#define write_configs_to_conf_cache test_write_configs_to_conf_cache
#define find_map_conf_file test_find_map_conf_file
#define new_config_response test_new_config_response
#define destroy_config_file test_destroy_config_file
#define grab_include_directives test_grab_include_directives
#include "src/common/fetch_config.c"
#undef strong_alias

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(expr) \
	do { \
		if (!(expr)) { \
			fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__, \
				#expr, strerror(errno)); \
			exit(EXIT_FAILURE); \
		} \
	} while (0)

static void _wait_success(pid_t child, int expected)
{
	int status;

	CHECK(child > 0);
	CHECK(waitpid(child, &status, 0) == child);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != expected) {
		fprintf(stderr,
			"child %ld: wait status=0x%x exit=%d signal=%d, expected exit=%d\n",
			(long) child, status,
			WIFEXITED(status) ? WEXITSTATUS(status) : -1,
			WIFSIGNALED(status) ? WTERMSIG(status) : 0, expected);
		exit(EXIT_FAILURE);
	}
}

static void _exec_failed(const char *path)
{
	int error = errno;

	fprintf(stderr, "execl(%s) failed: errno=%d (%s)\n", path, error,
		strerror(error));
	_exit(127);
}

static void _check_removed(int fd, char *path)
{
	CHECK(fcntl(fd, F_GETFD) < 0 && errno == EBADF);
	CHECK(access(path, F_OK) < 0 && errno == ENOENT);
}

static void _independent_reads(const char *self)
{
	char *path = NULL;
	char content[] = "temporary-config-content\n";
	char buffer[sizeof(content)] = { 0 };
	char descriptor[32];
	struct stat status;
	int fd = dump_to_memfd("slurm-test", content, &path);
	int first, second;
	pid_t child;

	CHECK(fd >= 0);
	CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC);
	CHECK(!fstat(fd, &status));
	CHECK(status.st_size == (off_t) strlen(content));
#ifdef __APPLE__
	CHECK((status.st_mode & 0777) == 0700);
	CHECK(status.st_uid == geteuid());
	CHECK(!strncmp(path, "/tmp/slurm-memfd-", 17));
#else
	CHECK(!strncmp(path, "/proc/", 6));
#endif
	CHECK((first = open(path, O_RDONLY)) >= 0);
	CHECK(read(first, buffer, 2) == 2);
	CHECK((second = open(path, O_RDONLY)) >= 0);
	CHECK(read(second, buffer, sizeof(buffer)) ==
	      (ssize_t) strlen(content));
	CHECK(!strcmp(buffer, content));
	CHECK(lseek(first, 0, SEEK_CUR) == 2);
	close(first);
	close(second);

	/* The original descriptor must also disappear across an ordinary exec. */
	snprintf(descriptor, sizeof(descriptor), "%d", fd);
	CHECK((child = fork()) >= 0);
	if (!child) {
		alarm(10);
		execl(self, self, "--closed", descriptor, (char *) NULL);
		_exec_failed(self);
	}
	_wait_success(child, 0);
	close_memfd(fd, path);
	_check_removed(fd, path);
	xfree(path);
}

static void _execute_after_closeall(void)
{
	char *path = NULL;
	char script[] = "#!/bin/sh\nexit 19\n";
	int fd = dump_to_memfd("slurm-script-test", script, &path);
	pid_t child;

	CHECK(fd >= 0);
	CHECK(!access(path, R_OK | X_OK));
	CHECK((child = fork()) >= 0);
	if (!child) {
		alarm(10);
		closeall(3);
		/* Linux reopens the parent's memfd; Darwin uses its named file. */
		execl(path, path, (char *) NULL);
		_exec_failed(path);
	}
	_wait_success(child, 19);
	close_memfd(fd, path);
	_check_removed(fd, path);
	xfree(path);
}

static void _empty_and_fd_zero(void)
{
	pid_t child;

	CHECK((child = fork()) >= 0);
	if (!child) {
		char *path = NULL;
		char byte;
		int fd, reader;

		alarm(10);
		close(STDIN_FILENO);
		fd = dump_to_memfd("slurm-empty-test", NULL, &path);
		CHECK(fd == STDIN_FILENO);
		CHECK((reader = open(path, O_RDONLY)) >= 0);
		CHECK(read(reader, &byte, 1) == 0);
		close(reader);
		close_memfd(fd, path);
		_check_removed(fd, path);
		xfree(path);
		close_memfd(-1, NULL);
		_exit(0);
	}
	_wait_success(child, 0);
}

static void _cached_config_cleanup(void)
{
	config_file_t *config = xmalloc(sizeof(*config));
	char *path;
	int fd;

	config->exists = true;
	config->file_name = xstrdup("slurm.conf");
	config->file_content = xstrdup("ClusterName=temporary\n");
	CHECK(!write_config_to_memfd(config, NULL));
	fd = config->memfd_fd;
	path = xstrdup(config->memfd_path);
	destroy_config_file(config);
	_check_removed(fd, path);
	xfree(path);
}

/* Model the downloaded configuration list without contacting a controller. */
static int _configless_exit(int report_fd)
{
	const char *names[] = { "slurm.conf", "included.conf", "empty.conf" };
	FILE *report = fdopen(report_fd, "w");

	CHECK(report);
	for (int i = 0; i < 3; i++) {
		config_file_t *config = xmalloc(sizeof(*config));

		config->exists = true;
		config->file_name = xstrdup(names[i]);
		if (i < 2)
			config->file_content =
				xstrdup("ClusterName=temporary\n");
		CHECK(!write_config_to_memfd(config, NULL));
		CHECK(fprintf(report, "%s\n", config->memfd_path) > 0);
	}
	CHECK(!fclose(report));
	/* Ordinary clients return without destroying their downloaded configs. */
	return 0;
}

static void _normal_configless_exit(const char *self)
{
	char descriptor[32], paths[3][PATH_MAX];
	int report_pipe[2];
	FILE *report;
	pid_t child;

	CHECK(!pipe(report_pipe));
	snprintf(descriptor, sizeof(descriptor), "%d", report_pipe[1]);
	CHECK((child = fork()) >= 0);
	if (!child) {
		close(report_pipe[0]);
		execl(self, self, "--configless-exit", descriptor,
		      (char *) NULL);
		_exec_failed(self);
	}
	close(report_pipe[1]);
	CHECK((report = fdopen(report_pipe[0], "r")));
	for (int i = 0; i < 3; i++) {
		CHECK(fgets(paths[i], sizeof(paths[i]), report));
		paths[i][strcspn(paths[i], "\n")] = '\0';
	}
	CHECK(fgetc(report) == EOF);
	CHECK(!fclose(report));
	_wait_success(child, 0);
	for (int i = 0; i < 3; i++)
		CHECK(access(paths[i], F_OK) < 0 && errno == ENOENT);
}

static void _forked_config_cleanup(void)
{
	config_file_t *config = xmalloc(sizeof(*config));
	char child_path[PATH_MAX];
	int report_pipe[2], reader;
	FILE *report;
	pid_t child;

	config->exists = true;
	config->file_name = xstrdup("slurm.conf");
	config->file_content = xstrdup("ClusterName=parent\n");
	CHECK(!write_config_to_memfd(config, NULL));

	/* The inherited exit handler must leave the parent's active file. */
	CHECK((child = fork()) >= 0);
	if (!child)
		exit(0);
	_wait_success(child, 0);
	CHECK((reader = open(config->memfd_path, O_RDONLY)) >= 0);
	close(reader);

	CHECK(!pipe(report_pipe));
	CHECK((child = fork()) >= 0);
	if (!child) {
		char *path = NULL;

		close(report_pipe[0]);
		CHECK((report = fdopen(report_pipe[1], "w")));
		/* Reinitialization may explicitly destroy an inherited cache. */
		destroy_config_file(config);
		CHECK(dump_to_memfd("child.conf", NULL, &path) >= 0);
		CHECK(fprintf(report, "%s\n", path) > 0);
		CHECK(!fclose(report));
		/* The child still owns, and must remove, its newly created file. */
		exit(0);
	}
	close(report_pipe[1]);
	CHECK((report = fdopen(report_pipe[0], "r")));
	CHECK(fgets(child_path, sizeof(child_path), report));
	child_path[strcspn(child_path, "\n")] = '\0';
	CHECK(!fclose(report));
	_wait_success(child, 0);
	CHECK(access(child_path, F_OK) < 0 && errno == ENOENT);
	CHECK((reader = open(config->memfd_path, O_RDONLY)) >= 0);
	close(reader);
	destroy_config_file(config);
}

#ifdef __APPLE__
static void _descendant_config_cleanup(void)
{
	char *ancestor_path = NULL, *child_path = NULL, *path = NULL;
	int ancestor_fd = dump_to_memfd("ancestor.conf", NULL, &ancestor_path);
	int child_fd, fd;

	/* Simulate a descendant receiving the ancestor's numeric PID. */
	_named_memfd_lock();
	_named_memfd_child();
	_named_memfd_exit();
	CHECK(!access(ancestor_path, R_OK));
	child_fd = dump_to_memfd("child.conf", NULL, &child_path);

	/* Ownership stays revoked across further generations. */
	_named_memfd_lock();
	_named_memfd_child();
	fd = dump_to_memfd("descendant.conf", NULL, &path);
	_named_memfd_exit();
	CHECK(!access(ancestor_path, R_OK));
	CHECK(!access(child_path, R_OK));
	CHECK(access(path, F_OK) < 0 && errno == ENOENT);

	close_memfd(ancestor_fd, ancestor_path);
	CHECK(fcntl(ancestor_fd, F_GETFD) < 0 && errno == EBADF);
	CHECK(!access(ancestor_path, R_OK));
	close_memfd(child_fd, child_path);
	CHECK(fcntl(child_fd, F_GETFD) < 0 && errno == EBADF);
	CHECK(!access(child_path, R_OK));
	close_memfd(fd, path);
	_check_removed(fd, path);

	/* These simulated ancestors are still responsible for their files. */
	CHECK(!unlink(ancestor_path));
	CHECK(!unlink(child_path));
	xfree(ancestor_path);
	xfree(child_path);
	xfree(path);
}

static void _credential_config_cleanup(void)
{
	char *path = NULL;
	char unrelated[] = "/tmp/slurm-handoff-test-XXXXXX";
	struct stat status, other_status;
	uid_t uid = geteuid();
	gid_t gid = getegid();
	int fd = dump_to_memfd("credentials.conf", NULL, &path);
	int backup = dup(fd);
	int other_fd = slurm_mkstemp(unrelated);
	pid_t child;

	CHECK(backup >= 0 && other_fd >= 0);
	CHECK(!fstat(other_fd, &other_status));
	CHECK(!chown_memfd_files(uid, gid));
	CHECK(!chown_memfd_files(uid, gid));
	CHECK(!fstat(fd, &status));
	CHECK(status.st_uid == uid && status.st_gid == gid);
	CHECK((status.st_mode & 0777) == 0700);

	/* Real unprivileged fchown failure must leave cleanup possible. */
	if (uid) {
		CHECK(chown_memfd_files(0, gid) == EPERM);
		CHECK(!fstat(fd, &status));
		CHECK(status.st_uid == uid && status.st_gid == gid);
		CHECK(!access(path, R_OK));
	}

	/* A recycled descriptor must not be mistaken for the registered inode. */
	CHECK(dup2(other_fd, fd) == fd);
	CHECK(chown_memfd_files(uid, gid) == ESTALE);
	CHECK(geteuid() == uid && getegid() == gid);
	CHECK(!fstat(other_fd, &status));
	CHECK(status.st_uid == other_status.st_uid &&
	      status.st_gid == other_status.st_gid);
	CHECK(dup2(backup, fd) == fd);
	CHECK(!close(backup));
	CHECK(!close(other_fd));
	CHECK(!unlink(unrelated));

	CHECK((child = fork()) >= 0);
	if (!child) {
		char *child_path = NULL;

		/* Revoked parent ownership must also exclude handoff operations. */
		CHECK(!close(fd));
		CHECK(!chown_memfd_files(uid, gid));
		CHECK(dump_to_memfd("child-credentials.conf", NULL,
				    &child_path) >= 0);
		CHECK(!chown_memfd_files(uid, gid));
		exit(0);
	}
	_wait_success(child, 0);
	CHECK(!access(path, R_OK));
	close_memfd(fd, path);
	_check_removed(fd, path);
	xfree(path);
}
#endif

static void *_config_worker(void *arg)
{
	(void) arg;
	for (int i = 0; i < 100; i++) {
		char *path = NULL;
		int fd = dump_to_memfd("thread.conf", NULL, &path);

		CHECK(fd >= 0);
		CHECK(!access(path, R_OK));
		close_memfd(fd, path);
		xfree(path);
	}
	return NULL;
}

static void _concurrent_config_cleanup(void)
{
	pthread_t workers[2];
	pid_t child;

	for (int i = 0; i < 2; i++)
		CHECK(!pthread_create(&workers[i], NULL, _config_worker, NULL));
	for (int i = 0; i < 10; i++) {
		CHECK((child = fork()) >= 0);
		if (!child) {
			alarm(5);
			exit(0);
		}
		_wait_success(child, 0);
	}
	for (int i = 0; i < 2; i++)
		CHECK(!pthread_join(workers[i], NULL));
}

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "--configless-exit")) {
		alarm(10);
		return _configless_exit(atoi(argv[2]));
	}
	if (argc == 3 && !strcmp(argv[1], "--closed")) {
		int fd = atoi(argv[2]);

		CHECK(fcntl(fd, F_GETFD) < 0 && errno == EBADF);
		return 0;
	}
	CHECK(argc == 1);
	alarm(30);
	closeall_init();
	_independent_reads(argv[0]);
	_execute_after_closeall();
	_empty_and_fd_zero();
	_cached_config_cleanup();
	_normal_configless_exit(argv[0]);
	_forked_config_cleanup();
#ifdef __APPLE__
	_descendant_config_cleanup();
	_credential_config_cleanup();
#endif
	_concurrent_config_cleanup();
	puts("temporary configuration/script lifecycle: PASS");
	return 0;
}
