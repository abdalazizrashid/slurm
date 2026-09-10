/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the real job-level sampler with deterministic stepd responses. */
#include <check.h>
#include <fcntl.h>

#define stepd_available test_stepd_available
#define stepd_available_checked test_stepd_available_checked
#define stepd_connect test_stepd_connect
#define stepd_stat_jobacct test_stepd_stat_jobacct
#define stepd_get_mem_limit test_stepd_get_mem_limit
#define jobacctinfo_getinfo test_jobacctinfo_getinfo
#define slurm_free_job_step_stat test_free_job_step_stat
#define slurm_send_only_controller_msg test_send_only_controller_msg
#include "src/slurmd/slurmd/job_mem_limit.c"
#undef stepd_available
#undef stepd_available_checked
#undef stepd_connect
#undef stepd_stat_jobacct
#undef stepd_get_mem_limit
#undef jobacctinfo_getinfo
#undef slurm_free_job_step_stat
#undef slurm_send_only_controller_msg

slurmd_conf_t *conf;

typedef struct {
	uint32_t job_id;
	uint64_t rss, vsize;
	bool connect_error, stat_error, no_accounting;
} sample_t;

static sample_t samples[8];
static size_t sample_count;
static uint32_t cancelled[8];
static size_t cancel_count;
static bool enumeration_error;

list_t *test_stepd_available(const char *directory, const char *nodename)
{
	list_t *steps;

	if (enumeration_error)
		return NULL;
	steps = list_create(xfree_ptr);
	for (size_t i = 0; i < sample_count; i++) {
		step_loc_t *step = xmalloc(sizeof(*step));
		step->step_id.job_id = samples[i].job_id;
		step->step_id.step_id = i;
		list_append(steps, step);
	}
	return steps;
}

list_t *test_stepd_available_checked(const char *directory,
				     const char *nodename, bool *complete)
{
	*complete = !enumeration_error;
	/* Real scan failures can return an empty, non-NULL list. */
	if (enumeration_error)
		return list_create(xfree_ptr);
	return test_stepd_available(directory, nodename);
}

int test_stepd_connect(const char *directory, const char *nodename,
		       slurm_step_id_t *id, uint16_t *version)
{
	if (samples[id->step_id].connect_error)
		return -1;
	return open("/dev/null", O_RDONLY);
}

int test_stepd_get_mem_limit(int fd, uint16_t version, uint64_t *limit)
{
	*limit = 0;
	return SLURM_SUCCESS;
}

int test_stepd_stat_jobacct(int fd, uint16_t version, slurm_step_id_t *id,
			    job_step_stat_t *resp)
{
	sample_t *sample = &samples[id->step_id];

	if (sample->stat_error)
		return SLURM_ERROR;
	if (!sample->no_accounting)
		resp->jobacct = (jobacctinfo_t *) sample;
	return SLURM_SUCCESS;
}

int test_jobacctinfo_getinfo(jobacctinfo_t *acct, enum jobacct_data_type type,
			     void *data, uint16_t version)
{
	sample_t *sample = (sample_t *) acct;
	*(uint64_t *) data =
		(type == JOBACCT_DATA_TOT_RSS) ? sample->rss : sample->vsize;
	return SLURM_SUCCESS;
}

void test_free_job_step_stat(job_step_stat_t *resp)
{
	xfree(resp);
}

int test_send_only_controller_msg(slurm_msg_t *msg,
				  slurmdb_cluster_rec_t *cluster)
{
	if (msg->msg_type == REQUEST_CANCEL_JOB_STEP) {
		job_step_kill_msg_t *kill = msg->data;
		ck_assert_uint_eq(kill->signal, SIGKILL);
		ck_assert_uint_eq(kill->flags, KILL_OOM);
		ck_assert_uint_eq(kill->step_id.step_id, NO_VAL);
		ck_assert_uint_lt(cancel_count, 8);
		cancelled[cancel_count++] = kill->step_id.job_id;
	}
	return SLURM_SUCCESS;
}

static void _setup(void)
{
	static slurmd_conf_t daemon;
	conf = &daemon;
	slurm_conf.job_acct_oom_kill = true;
	slurm_conf.vsize_factor = 0;
	sample_count = cancel_count = 0;
	enumeration_error = false;
	memset(samples, 0, sizeof(samples));
	job_mem_limit_init();
}

static void _register(uint32_t job_id, uint64_t mib)
{
	slurm_step_id_t step = { .job_id = job_id };
	job_mem_limit_register(&step, mib);
}

START_TEST(test_unknown_then_excess)
{
	_register(1, 64);
	samples[0] = (sample_t) { .job_id = 1,
				  .rss = INFINITE64,
				  .vsize = INFINITE64 };
	sample_count = 1;
	job_mem_limit_enforce();
	ck_assert_uint_eq(cancel_count, 0);
	ck_assert_int_eq(list_count(job_limits_list), 1);
	samples[0].rss = NO_VAL64;
	job_mem_limit_enforce();
	ck_assert_uint_eq(cancel_count, 0);
	ck_assert_int_eq(list_count(job_limits_list), 1);
	samples[0].rss = 65 * 1048576ULL;
	job_mem_limit_enforce();
	ck_assert_uint_eq(cancel_count, 1);
	ck_assert_uint_eq(cancelled[0], 1);
}

END_TEST

START_TEST(test_transient_failures)
{
	_register(1, 64);
	samples[0] = (sample_t) { .job_id = 1,
				  .rss = 65 * 1048576ULL,
				  .connect_error = true };
	sample_count = 1;
	job_mem_limit_enforce();
	ck_assert_int_eq(list_count(job_limits_list), 1);
	samples[0].connect_error = false;
	samples[0].stat_error = true;
	job_mem_limit_enforce();
	ck_assert_int_eq(list_count(job_limits_list), 1);
	samples[0].stat_error = false;
	samples[0].no_accounting = true;
	job_mem_limit_enforce();
	ck_assert_int_eq(list_count(job_limits_list), 1);
	enumeration_error = true;
	job_mem_limit_enforce();
	ck_assert_int_eq(list_count(job_limits_list), 1);
	ck_assert_uint_eq(cancel_count, 0);
	enumeration_error = false;
	samples[0].no_accounting = false;
	job_mem_limit_enforce();
	ck_assert_uint_eq(cancel_count, 1);
}

END_TEST

START_TEST(test_multiple_steps_and_completed_job)
{
	_register(1,
		  64); /* completed entry must not skip checking the next job */
	_register(2, 64);
	samples[0] = (sample_t) { .job_id = 2, .rss = 40 * 1048576ULL };
	samples[1] = samples[0];
	samples[2] = (sample_t) { .job_id = 2,
				  .rss = INFINITE64,
				  .vsize = NO_VAL64 };
	sample_count = 3;
	job_mem_limit_enforce();
	ck_assert_int_eq(list_count(job_limits_list), 1);
	ck_assert_uint_eq(cancel_count, 1);
	ck_assert_uint_eq(cancelled[0], 2);
	sample_count = 0;
	job_mem_limit_enforce();
	ck_assert_int_eq(list_count(job_limits_list), 0);
}

END_TEST

START_TEST(test_virtual_memory_independent)
{
	slurm_conf.vsize_factor = 200;
	_register(1, 64);
	samples[0] = (sample_t) { .job_id = 1,
				  .rss = INFINITE64,
				  .vsize = 129 * 1048576ULL };
	sample_count = 1;
	job_mem_limit_enforce();
	ck_assert_uint_eq(cancel_count, 1);
	ck_assert_int_eq(list_count(job_limits_list), 1);
}

END_TEST

int main(void)
{
	Suite *suite = suite_create("Sampled job memory limits");
	TCase *test = tcase_create("unknown and multiple steps");
	SRunner *runner;
	int failed;

	tcase_add_checked_fixture(test, _setup, job_mem_limit_fini);
	tcase_add_test(test, test_unknown_then_excess);
	tcase_add_test(test, test_transient_failures);
	tcase_add_test(test, test_multiple_steps_and_completed_job);
	tcase_add_test(test, test_virtual_memory_independent);
	suite_add_tcase(suite, test);
	runner = srunner_create(suite);
	srunner_run_all(runner, CK_ENV);
	failed = srunner_ntests_failed(runner);
	srunner_free(runner);
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
