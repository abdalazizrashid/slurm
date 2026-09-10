/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <check.h>

/* Exercise the real interface's rollback using a deliberately failing plugin. */
#include "src/interfaces/jobacct_gather.c"

static int calls;

static int _reject(pid_t pid, jobacct_id_t *id)
{
	calls++;
	return SLURM_ERROR;
}

static int _accept(pid_t pid, jobacct_id_t *id)
{
	calls++;
	return SLURM_SUCCESS;
}

static void _unexpected_poll(list_t *list, uint64_t container, bool profile)
{
	ck_abort_msg("Failed registration must not poll");
}

START_TEST(test_registration_rollback)
{
	jobacct_id_t id = { 0 };
	plugin_inited = PLUGIN_INITED;
	jobacct_shutdown = false;
	task_list = list_create(jobacctinfo_destroy);
	ops.add_task = _reject;
	ops.poll_data = _unexpected_poll;
	ck_assert_int_eq(jobacct_gather_add_task(123, &id, 1), SLURM_ERROR);
	ck_assert_int_eq(calls, 1);
	ck_assert_int_eq(list_count(task_list), 0);
	ops.add_task = _accept;
	ck_assert_int_eq(jobacct_gather_add_task(123, &id, 0), SLURM_SUCCESS);
	ck_assert_int_eq(calls, 2);
	ck_assert_int_eq(list_count(task_list), 1);
	FREE_NULL_LIST(task_list);
}

END_TEST

int main(void)
{
	Suite *suite = suite_create("Task accounting registration");
	TCase *test = tcase_create("rollback");
	SRunner *runner;
	int failed;
	tcase_add_test(test, test_registration_rollback);
	suite_add_tcase(suite, test);
	runner = srunner_create(suite);
	srunner_run_all(runner, CK_ENV);
	failed = srunner_ntests_failed(runner);
	srunner_free(runner);
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
