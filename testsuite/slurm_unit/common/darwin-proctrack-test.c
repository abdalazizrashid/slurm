/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <check.h>
#include <spawn.h>
#include <sys/wait.h>

#include "src/plugins/proctrack/darwin/proctrack_darwin.c"

static pid_t own_pids[8];
static int own_count;
static darwin_identity_t descendant;
static char *self_path;
extern char **environ;

static pid_t _paused_child(void)
{
	pid_t pid = fork();
	ck_assert_int_ge(pid, 0);
	if (!pid) {
		alarm(15);
		while (1)
			pause();
	}
	own_pids[own_count++] = pid;
	return pid;
}

static void _setup(void)
{
	own_count = 0;
	memset(&descendant, 0, sizeof(descendant));
	slurm_conf.unkillable_timeout = 2;
	ck_assert_int_eq(init(), SLURM_SUCCESS);
}

static void _teardown(void)
{
	darwin_identity_t current;
	fini();
	if (descendant.pid && !_identity(descendant.pid, &current) &&
	    _same(&descendant, &current))
		kill(descendant.pid, SIGKILL);
	for (int i = 0; i < own_count; i++) {
		if (own_pids[i] <= 0)
			continue;
		kill(own_pids[i], SIGKILL);
		while (waitpid(own_pids[i], NULL, 0) < 0 && errno == EINTR)
			;
	}
}

START_TEST(test_registration_and_concurrent_containers)
{
	stepd_step_rec_t first = { 0 }, second = { 0 };
	pid_t one, two, *pids = NULL;
	int count, status;

	ck_assert_int_eq(proctrack_p_create(&first), SLURM_SUCCESS);
	ck_assert_int_eq(proctrack_p_create(&second), SLURM_SUCCESS);
	ck_assert_uint_ne(first.cont_id, second.cont_id);
	one = _paused_child();
	two = _paused_child();
	ck_assert_int_eq(proctrack_p_add(&first, one), SLURM_SUCCESS);
	ck_assert_int_eq(proctrack_p_add(&second, two), SLURM_SUCCESS);
	ck_assert_int_eq(proctrack_p_add(&second, one), SLURM_ERROR);
	ck_assert(proctrack_p_has_pid(first.cont_id, one));
	ck_assert(!proctrack_p_has_pid(first.cont_id, two));
	ck_assert_int_eq(proctrack_p_get_pids(first.cont_id, &pids, &count), 0);
	ck_assert_int_eq(count, 1);
	ck_assert_int_eq(pids[0], one);
	xfree(pids);
	ck_assert_int_eq(proctrack_p_signal(first.cont_id, SIGSTOP), 0);
	ck_assert_int_eq(waitpid(one, &status, WUNTRACED), one);
	ck_assert(WIFSTOPPED(status));
	ck_assert_int_eq(proctrack_p_signal(first.cont_id, SIGCONT), 0);
	ck_assert_int_eq(proctrack_p_wait(first.cont_id), SLURM_SUCCESS);
	ck_assert_int_eq(kill(two, 0), 0);
}

END_TEST

START_TEST(test_exec_and_spawn_lifecycle)
{
	stepd_step_rec_t step = { 0 };
	darwin_identity_t before, after;
	int command[2], response[2];
	char descriptor[32], byte = 'x';
	char *argv[] = { self_path, "--ready-pause", descriptor, NULL };
	pid_t parent, child;
	bool found = false;

	ck_assert_int_eq(pipe(command), 0);
	ck_assert_int_eq(pipe(response), 0);
	snprintf(descriptor, sizeof(descriptor), "%d", response[1]);
	ck_assert_int_eq(proctrack_p_create(&step), 0);
	parent = fork();
	ck_assert_int_ge(parent, 0);
	if (!parent) {
		alarm(15);
		close(command[1]);
		close(response[0]);
		if (read(command[0], &byte, 1) != 1)
			_exit(1);
		if (!_i) {
			execv(self_path, argv);
			_exit(1);
		}
		if (posix_spawn(&child, self_path, NULL, NULL, argv, environ))
			_exit(1);
		while (1)
			pause();
	}
	own_pids[own_count++] = parent;
	close(command[0]);
	close(response[1]);
	ck_assert_int_eq(_identity(parent, &before), 0);
	ck_assert_int_eq(proctrack_p_add(&step, parent), 0);
	ck_assert_int_eq(write(command[1], &byte, 1), 1);
	ck_assert_int_eq(read(response[0], &child, sizeof(child)),
			 sizeof(child));
	ck_assert_int_eq(_identity(child, &after), 0);
	if (!_i) {
		ck_assert(_same(&before, &after));
	} else {
		ck_assert_int_ne(child, parent);
		descendant = after;
	}
	for (int i = 0; i < 100; i++) {
		if ((found = proctrack_p_has_pid(step.cont_id, child)))
			break;
		usleep(10000);
	}
	ck_assert(found);
	ck_assert_int_eq(proctrack_p_wait(step.cont_id), 0);
	close(command[1]);
	close(response[0]);
}

END_TEST

/* Child protocol: F creates a detached child, E releases the parent to exit. */
static pid_t _forking_child(int *command, int *response, bool unobserved_middle)
{
	int input[2], output[2];
	pid_t pid;
	ck_assert_int_eq(pipe(input), 0);
	ck_assert_int_eq(pipe(output), 0);
	pid = fork();
	ck_assert_int_ge(pid, 0);
	if (!pid) {
		char c;
		pid_t child;
		int ready[2] = { -1, -1 };
		close(input[1]);
		close(output[0]);
		alarm(15);
		if (read(input[0], &c, 1) != 1)
			_exit(1);
		child = fork();
		if (!child) {
			if (unobserved_middle) {
				if (pipe(ready))
					_exit(1);
				child = fork();
				if (child > 0) {
					close(ready[1]);
					if (read(ready[0], &c, 1) != 1)
						_exit(1);
					_exit(0);
				}
				if (child < 0)
					_exit(1);
				close(ready[0]);
			}
			alarm(15);
			if (setsid() < 0)
				_exit(1);
			child = getpid();
			if (write(output[1], &child, sizeof(child)) !=
			    sizeof(child))
				_exit(1);
			if (unobserved_middle) {
				if (write(ready[1], "x", 1) != 1)
					_exit(1);
				close(ready[1]);
			}
			while (1)
				pause();
		}
		if (unobserved_middle) {
			if (waitpid(child, NULL, 0) != child)
				_exit(1);
			if (write(output[1], "x", 1) != 1)
				_exit(1);
		}
		if (read(input[0], &c, 1) != 1)
			_exit(1);
		_exit(0);
	}
	close(input[0]);
	close(output[1]);
	own_pids[own_count++] = pid;
	*command = input[1];
	*response = output[0];
	return pid;
}

START_TEST(test_observed_detached_descendant)
{
	stepd_step_rec_t step = { 0 };
	int command, response;
	pid_t parent, child;
	char byte = 'x';
	bool found = false;

	ck_assert_int_eq(proctrack_p_create(&step), 0);
	parent = _forking_child(&command, &response, false);
	ck_assert_int_eq(proctrack_p_add(&step, parent), 0);
	ck_assert_int_eq(write(command, &byte, 1), 1);
	ck_assert_int_eq(read(response, &child, sizeof(child)), sizeof(child));
	ck_assert_int_eq(_identity(child, &descendant), 0);
	for (int i = 0; i < 100; i++) {
		if ((found = proctrack_p_has_pid(step.cont_id, child)))
			break;
		usleep(10000);
	}
	ck_assert(found);
	ck_assert_int_eq(write(command, &byte, 1), 1);
	ck_assert_int_eq(waitpid(parent, NULL, 0), parent);
	own_pids[0] = 0;
	ck_assert(proctrack_p_has_pid(step.cont_id, child));
	ck_assert_int_eq(proctrack_p_wait(step.cont_id), 0);
	close(command);
	close(response);
}

END_TEST

START_TEST(test_unobserved_parent_limitation)
{
	stepd_step_rec_t step = { 0 };
	int command, response;
	pid_t parent, child;
	char byte = 'x';

	ck_assert_int_eq(proctrack_p_create(&step), 0);
	parent = _forking_child(&command, &response, true);
	ck_assert_int_eq(proctrack_p_add(&step, parent), 0);
	/* Reproduce a scheduling delay until the intermediate parent is reaped. */
	pthread_mutex_lock(&tracking_lock);
	ck_assert_int_eq(write(command, &byte, 1), 1);
	ck_assert_int_eq(read(response, &child, sizeof(child)), sizeof(child));
	ck_assert_int_eq(read(response, &byte, 1), 1);
	pthread_mutex_unlock(&tracking_lock);
	ck_assert_int_eq(_identity(child, &descendant), 0);
	ck_assert(!proctrack_p_has_pid(step.cont_id, child));
	ck_assert_int_eq(write(command, &byte, 1), 1);
	close(command);
	close(response);
}

END_TEST

START_TEST(test_recycled_identity_is_not_signalled)
{
	stepd_step_rec_t step = { 0 };
	tracked_process_t *process;
	pid_t pid;

	ck_assert_int_eq(proctrack_p_create(&step), 0);
	pid = _paused_child();
	ck_assert_int_eq(proctrack_p_add(&step, pid), 0);
	pthread_mutex_lock(&tracking_lock);
	process = _find(_container(step.cont_id), pid);
	process->identity.abstime++;
	pthread_mutex_unlock(&tracking_lock);
	ck_assert(!proctrack_p_has_pid(step.cont_id, pid));
	ck_assert_int_eq(proctrack_p_signal(step.cont_id, SIGKILL), 0);
	ck_assert_int_eq(kill(pid, 0), 0);
	ck_assert_int_eq(proctrack_p_wait(step.cont_id), 0);
}

END_TEST

START_TEST(test_unsupported_tracking_and_uncertain_cleanup)
{
	stepd_step_rec_t step = { .flags = LAUNCH_WAIT_FOR_CHILDREN };
	struct kevent event;
	int queue = kqueue(), rc, saved_errno;

	ck_assert_int_ge(queue, 0);
	EV_SET(&event, getpid(), EVFILT_PROC, EV_ADD, NOTE_TRACK, 0, NULL);
	/* Successful Check assertions may change errno during bookkeeping. */
	rc = kevent(queue, &event, 1, NULL, 0, NULL);
	saved_errno = errno;
	ck_assert_int_eq(rc, -1);
	ck_assert_int_eq(saved_errno, ENOTSUP);
	close(queue);
	ck_assert_int_eq(proctrack_p_create(&step), SLURM_ERROR);
	step.flags = 0;
	ck_assert_int_eq(proctrack_p_create(&step), SLURM_SUCCESS);
	pthread_mutex_lock(&tracking_lock);
	_container(step.cont_id)->incomplete = true;
	pthread_mutex_unlock(&tracking_lock);
	rc = proctrack_p_wait(step.cont_id);
	saved_errno = errno;
	ck_assert_int_eq(rc, SLURM_ERROR);
	ck_assert_int_eq(saved_errno, EIO);
}

END_TEST

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "--ready-pause")) {
		pid_t pid = getpid();
		int fd = atoi(argv[2]);
		alarm(15);
		if (write(fd, &pid, sizeof(pid)) != sizeof(pid))
			return EXIT_FAILURE;
		close(fd);
		while (1)
			pause();
	}
	self_path = realpath(argv[0], NULL);
	if (!self_path)
		return EXIT_FAILURE;
	Suite *suite = suite_create("Darwin observed process tracking");
	TCase *test = tcase_create("lifecycle");
	SRunner *runner;
	int failed;
	tcase_set_timeout(test, 10);
	tcase_add_checked_fixture(test, _setup, _teardown);
	tcase_add_test(test, test_registration_and_concurrent_containers);
	tcase_add_loop_test(test, test_exec_and_spawn_lifecycle, 0, 2);
	tcase_add_test(test, test_observed_detached_descendant);
	tcase_add_test(test, test_unobserved_parent_limitation);
	tcase_add_test(test, test_recycled_identity_is_not_signalled);
	tcase_add_test(test, test_unsupported_tracking_and_uncertain_cleanup);
	suite_add_tcase(suite, test);
	runner = srunner_create(suite);
	srunner_run_all(runner, CK_ENV);
	failed = srunner_ntests_failed(runner);
	srunner_free(runner);
	free(self_path);
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
