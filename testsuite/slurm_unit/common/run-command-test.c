/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "src/common/fd.h"
#include "src/common/log.h"
#include "src/common/run_command.h"
#include "src/common/xmalloc.h"

#include <check.h>

static char *self_path;

static void _write_input(int fd, void *arg)
{
	const char input[] = "input\n";

	ck_assert_int_eq(write(fd, input, sizeof(input) - 1),
			 sizeof(input) - 1);
}

START_TEST(test_command_io)
{
	char *argv[] = { "sh", "-c",
			 "IFS= read -r line; printf '%s\n' \"$line\"; "
			 "printf 'stderr\n' >&2; exit 7",
			 NULL };
	int status;
	run_command_args_t args = {
		.cb = _write_input,
		.direct_exec = _i,
		.max_wait = 2000,
		.script_argv = argv,
		.script_path = "/bin/sh",
		.script_type = "test command",
		.status = &status,
		.write_to_child = true,
	};
	char *output = run_command(&args);

	ck_assert_ptr_nonnull(output);
	ck_assert_str_eq(output, "input\nstderr\n");
	ck_assert(WIFEXITED(status));
	ck_assert_int_eq(WEXITSTATUS(status), 7);
	ck_assert_int_eq(run_command_count(), 0);
	xfree(output);
}

END_TEST

START_TEST(test_command_identity)
{
	char *argv[] = { self_path, "--identity", NULL };
	char expected[128];
	int status;
	run_command_args_t args = {
		.direct_exec = _i,
		.max_wait = 2000,
		.script_argv = argv,
		.script_path = self_path,
		.script_type = "test identity",
		.status = &status,
	};
	char *output = run_command(&args);

	snprintf(expected, sizeof(expected), "%u %u %u %u\n",
		 (unsigned) geteuid(), (unsigned) geteuid(),
		 (unsigned) getegid(), (unsigned) getegid());
	ck_assert_ptr_nonnull(output);
	ck_assert_str_eq(output, expected);
	ck_assert(WIFEXITED(status));
	ck_assert_int_eq(WEXITSTATUS(status), 0);
	xfree(output);
}

END_TEST

START_TEST(test_command_timeout)
{
	char *argv[] = { self_path, "--sleep", NULL };
	int status;
	bool timed_out = false;
	run_command_args_t args = {
		.max_wait = 100,
		.script_argv = argv,
		.script_path = self_path,
		.script_type = "test timeout",
		.status = &status,
		.timed_out = &timed_out,
	};
	char *output = run_command(&args);

	ck_assert(timed_out);
	ck_assert(WIFSIGNALED(status));
	ck_assert_int_eq(run_command_count(), 0);
	xfree(output);
}

END_TEST

int main(int argc, char **argv)
{
	Suite *suite;
	TCase *test;
	SRunner *runner;
	int failed;
	log_options_t opts = LOG_OPTS_INITIALIZER;

	closeall_init();
	if (run_command_is_launcher(argc, argv))
		run_command_launcher(argc, argv);
	if ((argc == 2) && !strcmp(argv[1], "--identity")) {
		printf("%u %u %u %u\n", (unsigned) getuid(),
		       (unsigned) geteuid(), (unsigned) getgid(),
		       (unsigned) getegid());
		return EXIT_SUCCESS;
	}
	if ((argc == 2) && !strcmp(argv[1], "--sleep")) {
		sleep(5);
		return EXIT_SUCCESS;
	}
	log_init("run-command-test", opts, 0, NULL);
	self_path = realpath(argv[0], NULL);
	if (!self_path || run_command_init(argc, argv, NULL))
		return EXIT_FAILURE;

	suite = suite_create("run command");
	test = tcase_create("process launch");
	tcase_add_loop_test(test, test_command_io, 0, 2);
	tcase_add_loop_test(test, test_command_identity, 0, 2);
	tcase_add_test(test, test_command_timeout);
	suite_add_tcase(suite, test);
	runner = srunner_create(suite);
	srunner_run_all(runner, CK_ENV);
	failed = srunner_ntests_failed(runner);
	srunner_free(runner);
	free(self_path);
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
