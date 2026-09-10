/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <check.h>
#include <errno.h>
#include <unistd.h>

#include "config.h"
#include "src/common/parse_config.h"

static void _destroy_configuration(s_p_hashtbl_t *table);

/* Qualification is injected here; real Metal execution has its own test. */
#ifndef HAVE_METAL
#define HAVE_METAL 1
#endif
#ifndef HAVE_SANDBOX_INIT
#define HAVE_SANDBOX_INIT 1
#endif
#ifdef DARWIN_GPU_PROBE_PATH
#undef DARWIN_GPU_PROBE_PATH
#endif
#define DARWIN_GPU_PROBE_PATH "/test-only/slurm-darwin-gpu-probe"
#define s_p_hashtbl_destroy _destroy_configuration
#define run_command test_run_command
#define spank_get_plugin_names test_spank_names
#define spank_require_empty_remote_stack test_require_empty_stack
#define runtime_g_is_none test_runtime_g_is_none
#include "src/plugins/task/darwin/task_darwin.c"
#undef runtime_g_is_none
#undef spank_get_plugin_names
#undef spank_require_empty_remote_stack
#undef run_command
#undef s_p_hashtbl_destroy

static bool runtime_none = true;
static bool spank_loaded;
static bool require_empty_stack;
static unsigned qualification_calls, qualification_failure;
static bool clobber_cleanup_errno;
static unsigned configuration_cleanups;

static void _destroy_configuration(s_p_hashtbl_t *table)
{
	s_p_hashtbl_destroy(table);
	configuration_cleanups++;
	if (clobber_cleanup_errno)
		errno = EBUSY;
}

extern void test_require_empty_stack(bool required)
{
	require_empty_stack = required;
}

extern char *test_run_command(run_command_args_t *args)
{
	qualification_calls++;
	ck_assert(args->direct_exec);
	ck_assert_str_eq(args->script_path, DARWIN_GPU_PROBE_PATH);
	ck_assert_str_eq(args->env[0], "PATH=/usr/bin:/bin");
	ck_assert_ptr_null(args->env[1]);
	ck_assert_int_eq(args->max_wait, 120000);
	ck_assert_str_eq(args->script_argv[1],
			 qualification_calls == 1 ? "--baseline" : "--deny");
	*args->status = qualification_failure == qualification_calls ? 256 : 0;
	*args->timed_out = false;
	return xstrdup("mock qualification");
}

extern size_t test_spank_names(char ***names)
{
	if (!spank_loaded)
		return 0;
	*names = xcalloc(2, sizeof(char *));
	(*names)[0] = xstrdup("test-early-hook");
	return 1;
}

extern bool test_runtime_g_is_none(void)
{
	return runtime_none;
}

#include "src/slurmd/slurmstepd/ulimits.c"

stepd_step_rec_t *step;

static char directory[] = "/tmp/slurm-darwin-task-XXXXXX";
static char *saved_conf;

static void _setup(void)
{
	char *path;
	strcpy(directory, "/tmp/slurm-darwin-task-XXXXXX");
	ck_assert_ptr_nonnull(mkdtemp(directory));
	saved_conf =
		getenv("SLURM_CONF") ? xstrdup(getenv("SLURM_CONF")) : NULL;
	path = xstrdup_printf("%s/slurm.conf", directory);
	ck_assert_int_eq(setenv("SLURM_CONF", path, 1), 0);
	xfree(path);
	slurm_conf.task_plugin_param = 0;
	runtime_none = true;
	spank_loaded = false;
	qualification_calls = qualification_failure = 0;
	require_empty_stack = false;
	clobber_cleanup_errno = false;
	configuration_cleanups = 0;
}

static void _write_conf(const char *contents)
{
	char *path = xstrdup_printf("%s/darwin.conf", directory);
	FILE *file = fopen(path, "w");
	ck_assert_ptr_nonnull(file);
	ck_assert_int_ge(fputs(contents, file), 0);
	ck_assert_int_eq(fclose(file), 0);
	xfree(path);
}

static void _teardown(void)
{
	char *path = xstrdup_printf("%s/darwin.conf", directory);
	fini();
	unlink(path);
	xfree(path);
	ck_assert_int_eq(rmdir(directory), 0);
	if (saved_conf)
		setenv("SLURM_CONF", saved_conf, 1);
	else
		unsetenv("SLURM_CONF");
	xfree(saved_conf);
}

START_TEST(test_required_configuration)
{
	ck_assert_int_eq(init(), SLURM_ERROR);
	_write_conf("# Explicitly disabled controls\n");
	ck_assert_int_eq(init(), SLURM_SUCCESS);
	ck_assert_uint_eq(cpu_seconds, 0);
	ck_assert_uint_eq(address_mib, 0);
	ck_assert_uint_eq(footprint_mib, 0);
}

END_TEST

START_TEST(test_invalid_configuration)
{
	const char *invalid[] = {
		"PerProcessCPUTimeSeconds=bad\n",
		"PerProcessAddressSpaceMiB=18446744073709551615\n",
		"InitialTaskImageFootprintMiB=2147483648\n",
		"UnrecognizedOption=1\n",
	};
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		_write_conf(invalid[i]);
		ck_assert_int_eq(init(), SLURM_ERROR);
	}
}

END_TEST

START_TEST(test_nonregular_configuration)
{
	char *path = xstrdup_printf("%s/darwin.conf", directory);
	int rc, saved_errno;

	ck_assert_int_eq(mkdir(path, 0700), 0);
	rc = init();
	saved_errno = errno;
	ck_assert_int_eq(rc, SLURM_ERROR);
	ck_assert_int_eq(saved_errno, EINVAL);
	ck_assert_uint_eq(qualification_calls, 0);
	ck_assert_int_eq(rmdir(path), 0);
	/* Nonregular files must fail before a blocking parser open. */
	ck_assert_int_eq(mkfifo(path, 0600), 0);
	rc = init();
	saved_errno = errno;
	ck_assert_int_eq(rc, SLURM_ERROR);
	ck_assert_int_eq(saved_errno, EINVAL);
	ck_assert_uint_eq(qualification_calls, 0);
	ck_assert_int_eq(unlink(path), 0);
	xfree(path);
}

END_TEST

START_TEST(test_unreadable_empty_configuration)
{
	char *path;
	int rc;

	/* Root can read a mode-000 file, so this control requires a user. */
	if (!geteuid())
		return;
	_write_conf("");
	path = xstrdup_printf("%s/darwin.conf", directory);
	ck_assert_int_eq(chmod(path, 0), 0);
	rc = init();
	ck_assert_int_eq(chmod(path, 0600), 0);
	ck_assert_int_eq(rc, SLURM_ERROR);
	xfree(path);
}

END_TEST

START_TEST(test_limits_configuration)
{
	_write_conf(
		"PerProcessCPUTimeSeconds=60\nPerProcessAddressSpaceMiB=1048576\n");
	ck_assert_int_eq(init(), SLURM_SUCCESS);
	ck_assert_uint_eq(cpu_seconds, 60);
	ck_assert_uint_eq(address_mib, 1048576);
	ck_assert_int_eq(task_p_add_pid(getpid()), SLURM_ERROR);
}

END_TEST

START_TEST(test_initial_image_runtime_guard)
{
	stepd_step_rec_t record = { 0 };

	_write_conf("InitialTaskImageFootprintMiB=32\n");
	ck_assert_int_eq(init(), SLURM_SUCCESS);
	ck_assert_uint_eq(footprint_mib, 32);
	ck_assert_int_eq(task_p_pre_setuid(&record), SLURM_SUCCESS);
	runtime_none = false;
	ck_assert_int_eq(task_p_pre_setuid(&record), SLURM_ERROR);
	ck_assert_int_eq(task_p_pre_launch_priv(&record, 0, 0), SLURM_ERROR);
	ck_assert_int_eq(task_p_add_pid(getpid()), SLURM_ERROR);
	/* No parent-side hook installs or enables a child image policy. */
	ck_assert(!darwin_launch_configured());
	runtime_none = true;
	ck_assert_int_eq(task_p_pre_launch(&record), SLURM_SUCCESS);
	ck_assert(darwin_launch_configured());
	ck_assert_int_eq(fini(), SLURM_SUCCESS);
	ck_assert(!darwin_launch_configured());
}

END_TEST

START_TEST(test_capability_rejection)
{
	char *message = NULL;
	ck_assert_int_eq(_unsupported(CPU_BIND_NONE, MEM_BIND_NONE, NO_VAL,
				      NO_VAL, NO_VAL, NULL, NULL),
			 SLURM_SUCCESS);
	ck_assert_int_eq(_unsupported(CPU_BIND_MASK, 0, 0, 0, 0, NULL,
				      &message),
			 SLURM_ERROR);
	ck_assert_ptr_nonnull(message);
	xfree(message);
	ck_assert_int_eq(_unsupported(0, MEM_BIND_LOCAL, 0, 0, 0, NULL, NULL),
			 SLURM_ERROR);
	ck_assert_int_eq(_unsupported(0, 0, 1000, 0, 0, NULL, NULL),
			 SLURM_ERROR);
	ck_assert_int_eq(_unsupported(0, 0, 0, 0, 0, "gpu:high", NULL),
			 SLURM_ERROR);
}

END_TEST

START_TEST(test_gpu_frequency_default_rejection)
{
	char *saved_gpu_freq_def = slurm_conf.gpu_freq_def;
	int rc, saved_errno;

	_write_conf("# No optional task controls\n");
	slurm_conf.gpu_freq_def = "high";
	rc = init();
	saved_errno = errno;
	ck_assert_int_eq(rc, SLURM_ERROR);
	ck_assert_int_eq(saved_errno, ENOTSUP);
	ck_assert_uint_eq(qualification_calls, 0);
	/* An absent or empty default does not request frequency control. */
	slurm_conf.gpu_freq_def = "";
	ck_assert_int_eq(init(), SLURM_SUCCESS);
	slurm_conf.gpu_freq_def = NULL;
	ck_assert_int_eq(init(), SLURM_SUCCESS);
	slurm_conf.gpu_freq_def = saved_gpu_freq_def;
}

END_TEST

START_TEST(test_initialization_error_survives_cleanup)
{
	const struct {
		const char *configuration;
		uint32_t task_parameters;
		char *gpu_frequency;
		unsigned failed_qualification;
		int expected_errno;
	} cases[] = {
		{ "# No optional controls\n", 0, "high", 0, ENOTSUP },
		{ "# No optional controls\n", CPU_BIND_MASK, NULL, 0, ENOTSUP },
		{ "# No optional controls\n", OOM_KILL_STEP, NULL, 0, ENOTSUP },
		{ "UnrecognizedOption=1\n", 0, NULL, 0, EINVAL },
		{ "PerProcessCPUTimeSeconds=18446744073709551615\n", 0, NULL, 0,
		  EOVERFLOW },
		{ "DenyUnallocatedGPUConnections=yes\n", 0, NULL, 1, ENOTSUP },
	};

	char *saved_gpu_frequency = slurm_conf.gpu_freq_def;
	int rc, saved_errno;

	_write_conf(cases[_i].configuration);
	slurm_conf.task_plugin_param = cases[_i].task_parameters;
	slurm_conf.gpu_freq_def = cases[_i].gpu_frequency;
	qualification_failure = cases[_i].failed_qualification;
	clobber_cleanup_errno = true;
	rc = init();
	saved_errno = errno;
	clobber_cleanup_errno = false;
	slurm_conf.gpu_freq_def = saved_gpu_frequency;
	ck_assert_int_eq(rc, SLURM_ERROR);
	ck_assert_int_eq(saved_errno, cases[_i].expected_errno);
	ck_assert_uint_eq(configuration_cleanups, 1);
}

END_TEST

START_TEST(test_late_frequency_policy_recheck)
{
	stepd_step_rec_t record = { 0 };
	int rc, saved_errno;

	_write_conf("# No optional task controls\n");
	ck_assert_int_eq(init(), SLURM_SUCCESS);
	ck_assert_int_eq(task_p_pre_setuid(&record), SLURM_SUCCESS);
	ck_assert_int_eq(task_p_pre_launch_priv(&record, 0, 0), SLURM_SUCCESS);
	/* The manager may populate this after pre-setuid validation. */
	record.tres_freq = "gpu:high";
	rc = task_p_pre_launch_priv(&record, 0, 0);
	saved_errno = errno;
	ck_assert_int_eq(rc, SLURM_ERROR);
	ck_assert_int_eq(saved_errno, ENOTSUP);
}

END_TEST

START_TEST(test_oom_step_policy_rejection)
{
	batch_job_launch_msg_t batch = { 0 };
	launch_tasks_request_msg_t request = { 0 };
	stepd_step_rec_t record = { 0 };
	char *message = NULL;
	int rc, saved_errno;

	_write_conf("# No optional task controls\n");
	ck_assert_int_eq(init(), SLURM_SUCCESS);
	ck_assert_int_eq(task_p_slurmd_batch_request(&batch), SLURM_SUCCESS);
	batch.oom_kill_step = true;
	rc = task_p_slurmd_batch_request(&batch);
	saved_errno = errno;
	ck_assert_int_eq(rc, SLURM_ERROR);
	ck_assert_int_eq(saved_errno, ENOTSUP);
	ck_assert_int_eq(task_p_slurmd_launch_request(&request, 0, &message),
			 SLURM_SUCCESS);
	ck_assert_ptr_null(message);
	request.oom_kill_step = true;
	rc = task_p_slurmd_launch_request(&request, 0, &message);
	saved_errno = errno;
	ck_assert_int_eq(rc, SLURM_ERROR);
	ck_assert_int_eq(saved_errno, ENOTSUP);
	ck_assert_ptr_nonnull(message);
	xfree(message);
	ck_assert_int_eq(task_p_pre_setuid(&record), SLURM_SUCCESS);
	record.oom_kill_step = true;
	ck_assert_int_eq(task_p_pre_setuid(&record), SLURM_ERROR);
	ck_assert_int_eq(task_p_pre_launch_priv(&record, 0, 0), SLURM_ERROR);
	/* The allocation's OOM flag does not apply to extern steps. */
	request.step_id.step_id = record.step_id.step_id = SLURM_EXTERN_CONT;
	ck_assert_int_eq(task_p_slurmd_launch_request(&request, 0, &message),
			 SLURM_SUCCESS);
	ck_assert_ptr_null(message);
	ck_assert_int_eq(task_p_pre_setuid(&record), SLURM_SUCCESS);
	ck_assert_int_eq(task_p_pre_launch_priv(&record, 0, 0), SLURM_SUCCESS);
}

END_TEST

START_TEST(test_gpu_qualification_failures)
{
	_write_conf("DenyUnallocatedGPUConnections=yes\n");
	qualification_failure = 1;
	ck_assert_int_eq(init(), SLURM_ERROR);
	ck_assert_uint_eq(qualification_calls, 1);
	ck_assert(!require_empty_stack);
	qualification_calls = 0;
	qualification_failure = 2;
	ck_assert_int_eq(init(), SLURM_ERROR);
	ck_assert_uint_eq(qualification_calls, 2);
	qualification_calls = qualification_failure = 0;
	ck_assert_int_eq(init(), SLURM_SUCCESS);
	ck_assert_uint_eq(qualification_calls, 2);
	ck_assert(require_empty_stack);
}

END_TEST

START_TEST(test_authenticated_local_gpu_policy)
{
	stepd_step_rec_t record = { 0 };
	uint64_t count = 1, measured;
	gres_step_state_t allocation = { .node_cnt = 1,
					 .gres_cnt_node_alloc = &count };
	gres_job_state_t job = { .node_cnt = 1, .gres_cnt_node_alloc = &count };
	gres_state_t state = { .plugin_id = gres_build_id("gpu"),
			       .state_type = GRES_STATE_TYPE_STEP,
			       .gres_data = &allocation };
	bitstr_t *bits = bit_alloc(2);
	char *user_env[] = { "SLURM_GPUS=99",
			     "SLURM_METAL_DEVICE_IDS=ffffffffffffffff", NULL };

	_write_conf("DenyUnallocatedGPUConnections=yes\n");
	ck_assert_int_eq(init(), SLURM_SUCCESS);
	record.env = user_env;
	ck_assert_int_eq(task_p_pre_setuid(&record), SLURM_SUCCESS);
	ck_assert(darwin_launch_gpu_denied());
	ck_assert_int_eq(task_p_add_pid(getpid()), SLURM_ERROR);
	spank_loaded = true;
	ck_assert_int_eq(task_p_pre_setuid(&record), SLURM_ERROR);
	spank_loaded = false;
	record.step_gres_list = list_create(NULL);
	list_append(record.step_gres_list, &state);
	ck_assert_int_eq(task_p_pre_setuid(&record), SLURM_SUCCESS);
	ck_assert(!darwin_launch_gpu_denied());
	count = NO_VAL64;
	ck_assert_int_eq(_local_gpu_count(&record, &measured), EINVAL);
	count = 1;
	allocation.node_cnt = 2;
	ck_assert_int_eq(_local_gpu_count(&record, &measured), EINVAL);
	allocation.node_cnt = 1;
	allocation.gres_bit_alloc = &bits;
	ck_assert_int_eq(_local_gpu_count(&record, &measured), EINVAL);
	bit_set(bits, 1);
	ck_assert_int_eq(_local_gpu_count(&record, &measured), 0);
	ck_assert_uint_eq(measured, 1);
	/* A batch step uses the extracted job allocation, not the step list. */
	record.batch = true;
	state.state_type = GRES_STATE_TYPE_JOB;
	state.gres_data = &job;
	record.job_gres_list = record.step_gres_list;
	ck_assert_int_eq(task_p_pre_setuid(&record), SLURM_SUCCESS);
	ck_assert(!darwin_launch_gpu_denied());
	count = 0;
	ck_assert_int_eq(task_p_pre_setuid(&record), SLURM_SUCCESS);
	ck_assert(darwin_launch_gpu_denied());
	FREE_NULL_BITMAP(bits);
	FREE_NULL_LIST(record.step_gres_list);
}

END_TEST

START_TEST(test_gpu_special_step_allocation_scope)
{
	stepd_step_rec_t record = { 0 };
	uint64_t job_count = 2, step_count = 1, measured;
	gres_job_state_t job = { .node_cnt = 1,
				 .gres_cnt_node_alloc = &job_count };
	gres_step_state_t allocation = { .node_cnt = 1,
					 .gres_cnt_node_alloc = &step_count };
	gres_state_t job_state = { .plugin_id = gres_build_id("gpu"),
				   .state_type = GRES_STATE_TYPE_JOB,
				   .gres_data = &job };
	gres_state_t step_state = { .plugin_id = gres_build_id("gpu"),
				    .state_type = GRES_STATE_TYPE_STEP,
				    .gres_data = &allocation };
	const uint32_t special[] = { SLURM_BATCH_SCRIPT, SLURM_EXTERN_CONT,
				     SLURM_INTERACTIVE_STEP };

	_write_conf("DenyUnallocatedGPUConnections=yes\n");
	ck_assert_int_eq(init(), SLURM_SUCCESS);
	record.job_gres_list = list_create(NULL);
	record.step_gres_list = list_create(NULL);
	list_append(record.job_gres_list, &job_state);
	list_append(record.step_gres_list, &step_state);
	for (size_t i = 0; i < sizeof(special) / sizeof(special[0]); i++) {
		record.step_id.step_id = special[i];
		ck_assert_int_eq(_local_gpu_count(&record, &measured), 0);
		ck_assert_uint_eq(measured, 2);
		/* Special steps commonly have no independent step GRES allocation. */
		step_count = 0;
		ck_assert_int_eq(task_p_pre_setuid(&record), SLURM_SUCCESS);
		ck_assert(!darwin_launch_gpu_denied());
		step_count = 1;
	}
	record.step_id.step_id = 13;
	record.flags = LAUNCH_EXT_LAUNCHER;
	ck_assert_int_eq(_local_gpu_count(&record, &measured), 0);
	ck_assert_uint_eq(measured, 2);
	job_count = 0;
	ck_assert_int_eq(task_p_pre_setuid(&record), SLURM_SUCCESS);
	ck_assert(darwin_launch_gpu_denied());
	/* A normal step may use only its own allocation, not every job GPU. */
	record.flags = 0;
	ck_assert_int_eq(_local_gpu_count(&record, &measured), 0);
	ck_assert_uint_eq(measured, 1);
	ck_assert_int_eq(task_p_pre_setuid(&record), SLURM_SUCCESS);
	ck_assert(!darwin_launch_gpu_denied());
	job_count = 2;
	step_count = 0;
	ck_assert_int_eq(task_p_pre_setuid(&record), SLURM_SUCCESS);
	ck_assert(darwin_launch_gpu_denied());
	FREE_NULL_LIST(record.job_gres_list);
	FREE_NULL_LIST(record.step_gres_list);
}

END_TEST

START_TEST(test_propagation_preserves_native_policy_ceiling)
{
	const rlim_t tib = UINT64_C(1024) * 1024 * 1024 * 1024;
	struct rlimit limit = { .rlim_cur = 7 * tib, .rlim_max = 8 * tib };
	stepd_step_rec_t record = { 0 };
	char *empty_environment[] = { NULL };

	/* Check runs this case in its own child; parent limits are untouched. */
	step = &record;
	step->env = empty_environment;
	ck_assert_int_eq(setrlimit(RLIMIT_AS, &limit), 0);
	slurm_conf.vsize_factor = 200;
	step->step_mem = 6 * tib / (1024 * 1024);
	/* The 12 TiB target must not raise the 8 TiB ceiling. */
	set_user_limits(0);
	ck_assert_int_eq(getrlimit(RLIMIT_AS, &limit), 0);
	ck_assert_uint_eq(limit.rlim_max, 8 * tib);
	ck_assert_uint_eq(limit.rlim_cur, 7 * tib);

	step->step_mem = 2 * tib / (1024 * 1024);
	set_user_limits(0); /* A stricter 4 TiB target lowers both limits. */
	ck_assert_int_eq(getrlimit(RLIMIT_AS, &limit), 0);
	ck_assert_uint_eq(limit.rlim_max, 4 * tib);
	ck_assert_uint_eq(limit.rlim_cur, 4 * tib);

	limit.rlim_cur = 2 * tib;
	ck_assert_int_eq(setrlimit(RLIMIT_AS, &limit), 0);
	slurm_conf.vsize_factor = 150;
	/* A 3 TiB hard target preserves the 2 TiB soft limit. */
	set_user_limits(0);
	ck_assert_int_eq(getrlimit(RLIMIT_AS, &limit), 0);
	ck_assert_uint_eq(limit.rlim_max, 3 * tib);
	ck_assert_uint_eq(limit.rlim_cur, 2 * tib);
	step = NULL;
}

END_TEST

START_TEST(test_epilog_per_process_policy)
{
	stepd_step_rec_t record = { 0 };
	struct rlimit cpu_before, address_before, observed;
	const rlim_t address_ceiling = UINT64_C(4) * 1024 * 1024 * 1024 * 1024;
	pid_t child;
	int status;

	_write_conf("PerProcessCPUTimeSeconds=60\n"
		    "PerProcessAddressSpaceMiB=4194304\n"
		    "InitialTaskImageFootprintMiB=32\n");
	ck_assert_int_eq(getrlimit(RLIMIT_CPU, &cpu_before), 0);
	ck_assert_int_eq(getrlimit(RLIMIT_AS, &address_before), 0);
	ck_assert_int_eq(init(), SLURM_SUCCESS);
	ck_assert_int_eq(task_p_pre_setuid(&record), SLURM_SUCCESS);
	ck_assert(!darwin_launch_configured());
	for (int epilog = 0; epilog < 2; epilog++) {
		child = fork();
		ck_assert_int_ge(child, 0);
		if (!child) {
			if (epilog) {
				ck_assert_int_eq(darwin_launch_apply_epilog(),
						 0);
				ck_assert(!darwin_launch_configured());
			} else {
				ck_assert_int_eq(task_p_pre_launch(&record),
						 SLURM_SUCCESS);
				ck_assert(darwin_launch_configured());
			}
			ck_assert_int_eq(getrlimit(RLIMIT_CPU, &observed), 0);
			ck_assert_uint_le(observed.rlim_cur, 60);
			ck_assert_uint_le(observed.rlim_max, 60);
			ck_assert_int_eq(getrlimit(RLIMIT_AS, &observed), 0);
			ck_assert_uint_le(observed.rlim_cur, address_ceiling);
			ck_assert_uint_le(observed.rlim_max, address_ceiling);
			_exit(0);
		}
		ck_assert_int_eq(waitpid(child, &status, 0), child);
		ck_assert_int_eq(status, 0);
	}
	/* Neither sibling launch changes the daemon's limits or image policy. */
	ck_assert_int_eq(getrlimit(RLIMIT_CPU, &observed), 0);
	ck_assert_uint_eq(observed.rlim_cur, cpu_before.rlim_cur);
	ck_assert_uint_eq(observed.rlim_max, cpu_before.rlim_max);
	ck_assert_int_eq(getrlimit(RLIMIT_AS, &observed), 0);
	ck_assert_uint_eq(observed.rlim_cur, address_before.rlim_cur);
	ck_assert_uint_eq(observed.rlim_max, address_before.rlim_max);
	ck_assert(!darwin_launch_configured());
}

END_TEST

int main(void)
{
	Suite *suite = suite_create("Darwin task configuration");
	TCase *test = tcase_create("capabilities");
	SRunner *runner;
	int failed;

	tcase_add_checked_fixture(test, _setup, _teardown);
	tcase_add_test(test, test_required_configuration);
	tcase_add_test(test, test_invalid_configuration);
	tcase_add_test(test, test_nonregular_configuration);
	tcase_add_test(test, test_unreadable_empty_configuration);
	tcase_add_test(test, test_limits_configuration);
	tcase_add_test(test, test_capability_rejection);
	tcase_add_test(test, test_gpu_frequency_default_rejection);
	tcase_add_loop_test(test, test_initialization_error_survives_cleanup, 0,
			    6);
	tcase_add_test(test, test_late_frequency_policy_recheck);
	tcase_add_test(test, test_oom_step_policy_rejection);
	tcase_add_test(test, test_initial_image_runtime_guard);
	tcase_add_test(test, test_gpu_qualification_failures);
	tcase_add_test(test, test_authenticated_local_gpu_policy);
	tcase_add_test(test, test_gpu_special_step_allocation_scope);
	tcase_add_test(test, test_propagation_preserves_native_policy_ceiling);
	tcase_add_test(test, test_epilog_per_process_policy);
	suite_add_tcase(suite, test);
	runner = srunner_create(suite);
	srunner_run_all(runner, CK_ENV);
	failed = srunner_ntests_failed(runner);
	srunner_free(runner);
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
