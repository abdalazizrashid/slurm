/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/common/log.h"
#include "src/common/slurm_step_layout.h"
#include "src/common/xmalloc.h"
#include "src/common/xsched.h"

#include <check.h>

START_TEST(test_mask_bits)
{
	xcpuset_t *mask = xcpuset_alloc();
	size_t cpus[] = { 0, 31, 32, 63, 64, mask->max_cpus - 1 };
	char *str;
	xcpuset_t *copy;

	ck_assert_int_eq(XCPU_COUNT(mask), 0);
	for (size_t i = 0; i < sizeof(cpus) / sizeof(cpus[0]); i++) {
		XCPU_SET(cpus[i], mask);
		ck_assert(XCPU_ISSET(cpus[i], mask));
	}
	ck_assert_int_eq(XCPU_COUNT(mask), 6);
	str = task_cpuset_to_str(mask);
	copy = task_str_to_cpuset(str);
	ck_assert_ptr_nonnull(copy);
	ck_assert_int_eq(XCPU_COUNT(copy), 6);
	for (size_t cpu = 0; cpu < mask->max_cpus; cpu++)
		ck_assert_int_eq(!!XCPU_ISSET(cpu, mask),
				 !!XCPU_ISSET(cpu, copy));

	XCPU_CLR(32, mask);
	ck_assert(!XCPU_ISSET(32, mask));
	ck_assert_int_eq(XCPU_COUNT(mask), 5);
	XCPU_ZERO(mask);
	ck_assert_int_eq(XCPU_COUNT(mask), 0);
	xfree(str);
	str = task_cpuset_to_str(mask);
	ck_assert_str_eq(str, "0");
	xfree(str);
	xfree(copy);
	xfree(mask);
}

END_TEST

START_TEST(test_topology_rank_order)
{
	uint16_t cpus[] = { 2, 1, 1 };
	uint32_t ranks[] = { 10, 3, 3 };
	slurm_step_layout_req_t req = {
		.node_list = "node[0-2]",
		.cpus_per_node = cpus,
		.node_ranks = ranks,
		.num_hosts = 3,
		.num_tasks = 3,
		.task_dist = SLURM_DIST_BLOCK,
	};
	slurm_step_layout_t *layout = slurm_step_layout_create(&req);

	ck_assert_ptr_nonnull(layout);
	ck_assert_int_eq(layout->tasks[0], 1);
	ck_assert_int_eq(layout->tasks[1], 1);
	ck_assert_int_eq(layout->tasks[2], 1);
	ck_assert_int_eq(layout->tids[1][0], 0);
	ck_assert_int_eq(layout->tids[2][0], 1);
	ck_assert_int_eq(layout->tids[0][0], 2);
	slurm_step_layout_destroy(layout);
}

END_TEST

START_TEST(test_mask_parse)
{
	xcpuset_t *mask = task_str_to_cpuset("0x100000001");
	char *str;

	ck_assert_ptr_nonnull(mask);
	ck_assert_int_eq(XCPU_COUNT(mask), 2);
	ck_assert(XCPU_ISSET(0, mask));
	ck_assert(XCPU_ISSET(32, mask));
	str = xmalloc(mask->max_cpus / 4 + 2);
	memset(str, 'f', mask->max_cpus / 4 + 1);
	ck_assert_ptr_null(task_str_to_cpuset(str));
	ck_assert_ptr_null(task_str_to_cpuset(NULL));
	ck_assert_ptr_null(task_str_to_cpuset(""));
	ck_assert_ptr_null(task_str_to_cpuset("0x"));
	ck_assert_ptr_null(task_str_to_cpuset("12z4"));
	xfree(str);
	xfree(mask);
}

END_TEST

#ifdef __APPLE__
START_TEST(test_darwin_capabilities)
{
	xcpuset_t *mask = xcpuset_alloc(), *assigned;
	int rc, saved_errno;

	XCPU_SET(mask->max_cpus, mask);
	XCPU_SET((size_t) -1, mask);
	XCPU_CLR(mask->max_cpus, mask);
	ck_assert(!XCPU_ISSET(mask->max_cpus, mask));
	ck_assert(!XCPU_ISSET((size_t) -1, mask));
	ck_assert_int_eq(XCPU_COUNT(mask), 0);
	errno = 0;
	assigned = xgetaffinity(0);
	saved_errno = errno;
	ck_assert_ptr_null(assigned);
	ck_assert_int_eq(saved_errno, ENOTSUP);
	rc = xsetaffinity(0, mask);
	saved_errno = errno;
	ck_assert_int_eq(rc, -1);
	ck_assert_int_eq(saved_errno, ENOTSUP);
	ck_assert_int_eq(get_assigned_cpu_count(),
			 sysconf(_SC_NPROCESSORS_ONLN));
	xfree(mask);
}

END_TEST
#endif

int main(void)
{
	Suite *suite = suite_create("xsched");
	TCase *test = tcase_create("cpu masks");
	SRunner *runner;
	int failed;
	log_options_t opts = LOG_OPTS_INITIALIZER;

	log_init("xsched-test", opts, 0, NULL);
	tcase_add_test(test, test_mask_bits);
	tcase_add_test(test, test_mask_parse);
	tcase_add_test(test, test_topology_rank_order);
#ifdef __APPLE__
	tcase_add_test(test, test_darwin_capabilities);
#endif
	suite_add_tcase(suite, test);
	runner = srunner_create(suite);
	srunner_run_all(runner, CK_ENV);
	failed = srunner_ntests_failed(runner);
	srunner_free(runner);
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
