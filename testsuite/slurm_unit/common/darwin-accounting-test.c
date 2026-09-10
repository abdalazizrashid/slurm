/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <check.h>

/* Also exercise the real interface's final-poll/removal ordering. */
#define slurm_send_only_controller_msg test_controller_message
#include "src/interfaces/jobacct_gather.c"
#undef slurm_send_only_controller_msg

/* Drive process lifecycle transitions without requiring recycled OS PIDs. */
#define darwin_proc_sample test_proc_sample
#include "src/plugins/jobacct_gather/darwin/jobacct_gather_darwin.c"
#undef darwin_proc_sample

static darwin_proc_sample_t next_sample;
static darwin_proc_sample_t extra_samples[4];
static int extra_sample_count;
static pid_t enumerated_pid;
static int enumeration_error, sampling_error;
static unsigned energy_samples;
static unsigned profile_checks;
static unsigned memory_cancellations;

extern int test_controller_message(slurm_msg_t *msg,
				   slurmdb_cluster_rec_t *cluster)
{
	if (msg->msg_type == REQUEST_CANCEL_JOB_STEP)
		memory_cancellations++;
	else
		ck_assert_int_eq(msg->msg_type, REQUEST_JOB_NOTIFY);
	return SLURM_SUCCESS;
}

extern bool acct_gather_profile_g_is_active(uint32_t type)
{
	profile_checks++;
	return false;
}

extern int acct_gather_energy_g_get_sum(enum acct_energy_type type,
					acct_gather_energy_t *energy)
{
	energy_samples++;
	energy->consumed_energy = 123;
	energy->current_watts = 45;
	return SLURM_SUCCESS;
}

extern int acct_gather_profile_g_get(enum acct_gather_profile_info type,
				     void *data)
{
	*(uint32_t *) data = 0;
	return SLURM_SUCCESS;
}

extern int acct_gather_filesystem_g_get_data(acct_gather_data_t *data)
{
	return SLURM_SUCCESS;
}

extern int acct_gather_interconnect_g_get_data(acct_gather_data_t *data)
{
	return SLURM_SUCCESS;
}

extern int proctrack_g_get_pids(uint64_t cont_id, pid_t **pids, int *npids)
{
	*pids = NULL;
	*npids = 0;
	if (enumerated_pid) {
		*pids = xmalloc(sizeof(pid_t));
		**pids = enumerated_pid;
		*npids = 1;
	}
	errno = enumeration_error;
	return enumeration_error ? SLURM_ERROR : SLURM_SUCCESS;
}

extern int test_proc_sample(pid_t pid, darwin_proc_sample_t *sample)
{
	for (int i = 0; i < extra_sample_count; i++) {
		if (pid == extra_samples[i].pid) {
			*sample = extra_samples[i];
			return 0;
		}
	}
	if (pid != next_sample.pid)
		return ESRCH;
	if (sampling_error)
		return sampling_error;
	*sample = next_sample;
	return 0;
}

static darwin_task_t *_task(pid_t pid, uint64_t birth)
{
	darwin_task_t *task = xmalloc(sizeof(*task));
	task->pid = pid;
	task->record_id = pid;
	task->start_sec = birth;
	task->start_abstime = birth;
	task->identity_valid = true;
	list_append(tasks, task);
	return task;
}

static darwin_process_t *_process(pid_t pid, pid_t ppid, uint64_t birth,
				  uint64_t cpu, uint64_t rss)
{
	darwin_process_t *process = xmalloc(sizeof(*process));
	process->seen = true;
	process->sample = (darwin_proc_sample_t) {
		.pid = pid,
		.ppid = ppid,
		.start_sec = birth,
		.start_abstime = birth,
		.user_ns = cpu,
		.rss = rss,
	};
	list_append(processes, process);
	return process;
}

static void _setup(void)
{
	tasks = list_create(_destroy);
	processes = list_create(_destroy);
	enumerated_pid = 0;
	enumeration_error = sampling_error = 0;
	extra_sample_count = 0;
	incomplete_reported = false;
	energy_samples = 0;
	profile_checks = 0;
	memory_cancellations = 0;
	slurm_conf.job_acct_oom_kill = false;
	jobacct_mem_limit = jobacct_vmem_limit = 0;
	g_tres_count = TRES_ARRAY_TOTAL_CNT;
	cont_id = 1;
}

static void _teardown(void)
{
	FREE_NULL_LIST(processes);
	FREE_NULL_LIST(tasks);
}

START_TEST(test_task_ownership)
{
	darwin_task_t *first = _task(100, 1), *second = _task(200, 1);
	darwin_process_t *grandchild = _process(102, 101, 3, 7, 8);
	darwin_process_t *child = _process(101, 100, 2, 3, 4);
	darwin_process_t *orphan = _process(300, 999, 5, 1000, 1000);

	_process(200, 999, 1, 9, 16);
	_process(100, 999, 1, 2, 2);
	_assign_owners();
	ck_assert_ptr_eq(child->owner, first);
	ck_assert_ptr_eq(grandchild->owner, first);
	ck_assert_ptr_null(orphan->owner);
	_sum_usage();
	ck_assert_uint_eq(first->user_ns, 12);
	ck_assert_uint_eq(first->rss, 14);
	ck_assert_uint_eq(second->user_ns, 9);
	ck_assert_uint_eq(second->rss, 16);
}

END_TEST

START_TEST(test_exit_and_reparent)
{
	darwin_task_t *task = _task(100, 1);
	darwin_process_t *parent = _process(100, 999, 1, 10, 10);
	darwin_process_t *child = _process(101, 100, 2, 20, 20);

	_assign_owners();
	_sum_usage();
	ck_assert_uint_eq(task->user_ns, 30);
	parent->seen = false;
	child->sample.ppid = 1;
	child->sample.user_ns = 25;
	task->rss = task->vsize = 0;
	_assign_owners();
	_sum_usage();
	ck_assert_ptr_eq(child->owner, task);
	ck_assert_uint_eq(task->user_ns, 35);
	ck_assert_uint_eq(task->rss, 20);
	child->seen = false;
	task->rss = task->vsize = 0;
	_sum_usage();
	ck_assert_uint_eq(task->user_ns, 35);
	ck_assert_uint_eq(task->rss, 0);
}

END_TEST

START_TEST(test_pid_reuse)
{
	darwin_task_t *task = _task(100, 1);
	darwin_process_t *process = _process(100, 999, 1, 10, 10);
	pid_t pid = 100;

	_assign_owners();
	_sum_usage();
	process->seen = false;
	next_sample = (darwin_proc_sample_t) {
		.pid = 100,
		.ppid = 999,
		.start_sec = 2,
		.start_abstime = 2,
		.user_ns = 1000,
	};
	_sample_pid(pid, true);
	_assign_owners();
	_sum_usage();
	process = list_find_first(processes, _find_process, &pid);
	ck_assert_ptr_nonnull(process);
	ck_assert_ptr_null(process->owner);
	ck_assert_uint_eq(task->user_ns, 10);
	ck_assert_int_eq(list_count(processes), 1);
}

END_TEST

START_TEST(test_reused_parent)
{
	darwin_task_t *task = _task(100, 5);
	darwin_process_t *child = _process(101, 100, 2, 1000, 1000);

	_process(100, 999, 5, 10, 10);
	_assign_owners();
	_sum_usage();
	ck_assert_ptr_null(child->owner);
	ck_assert_uint_eq(task->user_ns, 10);
}

END_TEST

START_TEST(test_final_rusage)
{
	uint64_t totals[TRES_ARRAY_TOTAL_CNT], maxima[TRES_ARRAY_TOTAL_CNT];
	uint64_t minima[TRES_ARRAY_TOTAL_CNT];
	jobacctinfo_t acct = {
		.user_cpu_sec = 2,
		.user_cpu_usec = 900000,
		.sys_cpu_sec = 1,
		.sys_cpu_usec = 100000,
		.tres_count = TRES_ARRAY_TOTAL_CNT,
		.tres_usage_in_tot = totals,
		.tres_usage_in_max = maxima,
		.tres_usage_in_min = minima,
	};
	struct rusage usage = {
		.ru_utime = { .tv_sec = 2, .tv_usec = 100000 },
		.ru_stime = { .tv_sec = 1, .tv_usec = 800000 },
	};

	for (int i = 0; i < TRES_ARRAY_TOTAL_CNT; i++)
		totals[i] = maxima[i] = minima[i] = INFINITE64;
	ck_assert_int_eq(jobacctinfo_setinfo(&acct, JOBACCT_DATA_RUSAGE, &usage,
					     SLURM_PROTOCOL_VERSION),
			 0);
	ck_assert_uint_eq(acct.user_cpu_usec, 900000);
	ck_assert_uint_eq(acct.sys_cpu_usec, 800000);
	ck_assert_uint_eq(totals[TRES_ARRAY_CPU], 4700);
	ck_assert_uint_eq(maxima[TRES_ARRAY_CPU], 4700);
	ck_assert_uint_eq(minima[TRES_ARRAY_CPU], 4700);
	totals[TRES_ARRAY_CPU] = 6000;
	jobacctinfo_setinfo(&acct, JOBACCT_DATA_RUSAGE, &usage,
			    SLURM_PROTOCOL_VERSION);
	ck_assert_uint_eq(totals[TRES_ARRAY_CPU], 6000);
	ck_assert_uint_eq(maxima[TRES_ARRAY_CPU], 6000);
}

END_TEST

START_TEST(test_unknown_frequency)
{
	jobacctinfo_t dest = { .act_cpufreq = 1000 };
	jobacctinfo_t from = { .pid = 1, .act_cpufreq = NO_VAL };

	jobacctinfo_aggregate(&dest, &from);
	ck_assert_uint_eq(dest.act_cpufreq, NO_VAL);
	from.act_cpufreq = 2000;
	jobacctinfo_aggregate(&dest, &from);
	ck_assert_uint_eq(dest.act_cpufreq, NO_VAL);
}

END_TEST

START_TEST(test_registration_failure)
{
	jobacct_id_t id = { 0 };

	next_sample = (darwin_proc_sample_t) { .pid = 123, .start_abstime = 1 };
	ck_assert_int_eq(jobacct_gather_p_add_task(456, &id), SLURM_ERROR);
	ck_assert_int_eq(list_count(tasks), 0);
	ck_assert_int_eq(jobacct_gather_p_add_task(0, &id), SLURM_SUCCESS);
	ck_assert_int_eq(list_count(tasks), 0);
	ck_assert_int_eq(jobacct_gather_p_add_task(123, &id), SLURM_SUCCESS);
	ck_assert_int_eq(list_count(tasks), 1);
}

END_TEST

static void _poll_final_descendant(list_t *list, uint64_t container,
				   bool profile)
{
	jag_callbacks_t callbacks = {
		.get_precs = _get_precs,
		.get_offspring_data = _get_offspring_data,
	};
	jobacctinfo_t *record;

	/* The parent must still be registered throughout the final native sample. */
	ck_assert_int_eq(list_count(list), 1);
	record = list_peek(list);
	ck_assert_int_eq(record->pid, 100);
	jag_common_poll_data(list, container, &callbacks, profile);
	ck_assert(!callbacks.memory_incomplete);
	ck_assert_int_eq(list_count(prec_list), 1);
}

static void _poll_retained_records(list_t *list, uint64_t container,
				   bool profile)
{
	jag_callbacks_t callbacks = {
		.get_precs = _get_precs,
		.get_offspring_data = _get_offspring_data,
	};
	jag_common_poll_data(list, container, &callbacks, false);
}

START_TEST(test_parent_exit_final_sample_keeps_live_child_usage)
{
	darwin_task_t *owner = _task(100, 1);
	jobacctinfo_t *record, *result;
	jobacct_id_t id = { .taskid = 4, .nodeid = 2 };
	pid_t parent_pid = 100;

	plugin_inited = PLUGIN_INITED;
	jobacct_shutdown = false;
	task_list = list_create(jobacctinfo_destroy);
	jag_common_init(1000000000L);
	record = xmalloc(sizeof(*record));
	_init_tres_usage(record, &id, TRES_ARRAY_TOTAL_CNT);
	record->id = id;
	record->id.record_id = owner->record_id;
	record->pid = parent_pid;
	list_append(task_list, record);

	/* Both identities were observed before the launcher exited. */
	_process(100, 999, 1, 1000000000, 10);
	_process(101, 100, 2, 2000000000, 20);
	_assign_owners();
	_sum_usage();
	ck_assert_uint_eq(owner->user_ns, 3000000000);

	/* Parent is now absent; its child has reparented and accumulated usage. */
	enumerated_pid = 101;
	next_sample = (darwin_proc_sample_t) {
		.pid = 101,
		.ppid = 1,
		.start_sec = 2,
		.start_abstime = 2,
		.user_ns = 2500000000,
		.rss = 20,
		.read_bytes = 4096,
	};
	ops.poll_data = _poll_final_descendant;
	result = jobacct_gather_remove_task(parent_pid);
	ck_assert_ptr_eq(result, record);
	ck_assert_int_eq(list_count(task_list), 0);
	ck_assert_uint_eq(result->user_cpu_sec, 3);
	ck_assert_uint_eq(result->user_cpu_usec, 500000);
	ck_assert_uint_eq(result->tres_usage_in_tot[TRES_ARRAY_FS_DISK], 4096);
	ck_assert_uint_eq(owner->user_ns, 3500000000);
	jobacctinfo_destroy(result);
	FREE_NULL_LIST(task_list);
	FREE_NULL_LIST(prec_list);
}

END_TEST

START_TEST(test_completed_owner_is_retained_until_cleanup)
{
	darwin_task_t *owner = _task(100, 1);
	jobacctinfo_t *record, *copy, *aggregate;
	jobacct_id_t id = { .taskid = 4, .nodeid = 2, .record_id = 100 };
	struct rusage usage = { .ru_utime = { .tv_sec = 1 } };
	list_t *completed, *again;

	plugin_inited = PLUGIN_INITED;
	jobacct_shutdown = false;
	task_list = list_create(jobacctinfo_destroy);
	jag_common_init(1000000000L);
	record = jobacctinfo_create(&id);
	record->pid = 100;
	record->id = id;
	list_append(task_list, record);
	_process(100, 999, 1, 1000000000, 10);
	_process(101, 100, 2, 2000000000, 20);
	_assign_owners();
	_sum_usage();
	enumerated_pid = 101;
	next_sample = (darwin_proc_sample_t) {
		.pid = 101,
		.ppid = 1,
		.start_sec = 2,
		.start_abstime = 2,
		.user_ns = 2500000000,
		.rss = 20,
	};
	ops.poll_data = _poll_final_descendant;
	ck_assert_int_eq(jobacct_gather_complete_task(100, 100, &usage), 0);
	ck_assert(record->task_completed);
	ck_assert(record->final_rusage_valid);
	ck_assert_int_eq(list_count(task_list), 1);
	ck_assert_uint_eq(owner->user_ns, 3500000000);

	/* A later sample still charges the child to the completed owner. */
	next_sample.user_ns = 4000000000;
	copy = jobacct_gather_stat_task(100, true);
	ck_assert_ptr_nonnull(copy);
	ck_assert_uint_eq(copy->user_cpu_sec, 5);
	ck_assert_uint_eq(owner->user_ns, 5000000000);
	jobacctinfo_destroy(copy);

	/* Cleanup transfers records exactly once without querying a dead tracker. */
	ops.poll_data = NULL;
	completed = jobacct_gather_take_tasks();
	ck_assert_int_eq(list_count(completed), 1);
	ck_assert_ptr_eq(list_peek(completed), record);
	again = jobacct_gather_take_tasks();
	ck_assert_int_eq(list_count(again), 0);
	aggregate = jobacctinfo_create(NULL);
	jobacctinfo_aggregate(aggregate, record);
	ck_assert_uint_eq(aggregate->user_cpu_sec, 5);
	jobacctinfo_destroy(aggregate);
	FREE_NULL_LIST(completed);
	FREE_NULL_LIST(again);
	FREE_NULL_LIST(task_list);
	FREE_NULL_LIST(prec_list);
}

END_TEST

START_TEST(test_final_wait_preserves_disjoint_descendants)
{
	jobacct_id_t id = { .taskid = 4, .record_id = 100 };
	jobacctinfo_t *record, *copy;
	struct rusage usage = {
		.ru_utime = { .tv_sec = 3 },
		.ru_stime = { .tv_usec = 750000 },
	};
	uint64_t expected_user = 8, expected_system = 1250000;

	_task(100, 1);
	plugin_inited = PLUGIN_INITED;
	jobacct_shutdown = false;
	task_list = list_create(jobacctinfo_destroy);
	jag_common_init(1000000000L);
	record = jobacctinfo_create(&id);
	record->pid = 100;
	record->id = id;
	list_append(task_list, record);
	_process(100, 999, 1, 1000000000, 10)->sample.system_ns = 250000000;
	_process(101, 100, 2, 5000000000, 20)->sample.system_ns = 500000000;
	if (_i == 1) {
		/* Both surviving generations contribute only their own CPU. */
		_process(102, 101, 3, 2000000000, 30)->sample.system_ns =
			125000000;
		extra_samples[0] = (darwin_proc_sample_t) {
			.pid = 102,
			.ppid = 101,
			.start_sec = 3,
			.start_abstime = 3,
			.user_ns = 2000000000,
			.system_ns = 125000000,
			.rss = 30,
		};
		extra_sample_count = 1;
		expected_user += 2;
		expected_system += 125000;
	} else if (_i == 2) {
		/* A reaped child has itself reaped a grandchild before owner wait. */
		_process(103, 100, 3, 2000000000, 0)->sample.system_ns =
			250000000;
		_process(104, 103, 4, 4000000000, 0)->sample.system_ns =
			125000000;
		usage.ru_utime.tv_sec += 6;
		usage.ru_stime.tv_sec = 1;
		usage.ru_stime.tv_usec = 125000;
		expected_user += 6;
		expected_system += 375000;
	} else if (_i == 3) {
		/*
		 * A surviving child's vanished descendant may not be in owner
		 * wait. Reaping cannot be inferred: retain the larger lower bound
		 * (sampled 10s), rather than adding possibly overlapping history.
		 */
		_process(103, 101, 3, 4000000000, 0);
		expected_user = 10;
	}
	_assign_owners();
	_sum_usage();
	enumerated_pid = 101;
	next_sample = (darwin_proc_sample_t) {
		.pid = 101,
		.ppid = 1,
		.start_sec = 2,
		.start_abstime = 2,
		.user_ns = 5000000000,
		.system_ns = 500000000,
		.rss = 20,
	};
	ops.poll_data = _poll_final_descendant;
	ck_assert_int_eq(jobacct_gather_complete_task(100, 100, &usage), 0);
	/* Completion still takes a final profile sample before marking it done. */
	ck_assert_uint_eq(profile_checks, 1);
	ck_assert_uint_eq(record->user_cpu_sec, expected_user);
	ck_assert_uint_eq(record->sys_cpu_sec, expected_system / 1000000);
	ck_assert_uint_eq(record->sys_cpu_usec, expected_system % 1000000);
	ck_assert_uint_eq(record->tres_usage_in_tot[TRES_ARRAY_CPU],
			  expected_user * CPU_TIME_ADJ +
				  expected_system / 1000);

	/* The final direct increment survives both later growth and exit. */
	next_sample.user_ns += 2000000000;
	expected_user += 2;
	if (_i == 1) {
		extra_samples[0].user_ns += 1000000000;
		expected_user++;
	}
	copy = jobacct_gather_stat_task(100, true);
	ck_assert_uint_eq(copy->user_cpu_sec, expected_user);
	jobacctinfo_destroy(copy);
	next_sample.pid = enumerated_pid = 0;
	extra_sample_count = 0;
	copy = jobacct_gather_stat_task(100, true);
	ck_assert_uint_eq(copy->user_cpu_sec, expected_user);
	ck_assert_uint_eq(copy->tres_usage_in_tot[TRES_ARRAY_CPU],
			  expected_user * CPU_TIME_ADJ +
				  expected_system / 1000);
	ck_assert_int_eq(list_count(processes), 0);
	jobacctinfo_destroy(copy);
	ck_assert_int_eq(jobacct_gather_complete_task(100, 100, &usage), 0);
	ck_assert_uint_eq(record->user_cpu_sec, expected_user);
	ck_assert_uint_eq(profile_checks, 1);
	FREE_NULL_LIST(task_list);
	FREE_NULL_LIST(prec_list);
}

END_TEST

START_TEST(test_retained_owner_cookie_survives_pid_reuse)
{
	darwin_task_t *old = _task(100, 1), *replacement;
	jobacctinfo_t *retired = jobacctinfo_create(NULL), *current;
	jobacct_id_t id = { .taskid = 8, .nodeid = 2 };
	jag_callbacks_t callbacks = { 0 };
	jag_prec_t *prec;
	list_itr_t *itr;
	bool old_seen = false, new_seen = false;

	plugin_inited = PLUGIN_INITED;
	jobacct_shutdown = false;
	task_list = list_create(jobacctinfo_destroy);
	jag_common_init(1000000000L);
	retired->pid = 100;
	retired->id.record_id = old->record_id;
	retired->task_completed = true;
	list_append(task_list, retired);
	old->user_ns = 3000000000;
	_process(100, 999, 1, 1000000000, 0)->owner = old;

	next_sample = (darwin_proc_sample_t) {
		.pid = 100,
		.ppid = 999,
		.start_sec = 9,
		.start_abstime = 9,
		.user_ns = 7000000000,
	};
	ops.add_task = jobacct_gather_p_add_task;
	ck_assert_int_eq(jobacct_gather_add_task(100, &id, 0), 0);
	ck_assert_uint_ne(id.record_id, old->record_id);
	current = list_peek(task_list);
	ck_assert(!current->task_completed);
	replacement = list_find_first(tasks, _find_task_record, &id.record_id);
	ck_assert_ptr_nonnull(replacement);
	/* Explicit registration also admits the new identity without inventory. */
	enumerated_pid = 0;
	_get_precs(task_list, 1, &callbacks);
	ck_assert_int_eq(list_count(tasks), 2);
	ck_assert_uint_eq(old->user_ns, 3000000000);
	ck_assert_uint_eq(replacement->user_ns, 7000000000);
	itr = list_iterator_create(prec_list);
	while ((prec = list_next(itr))) {
		if (prec->record_id == old->record_id) {
			ck_assert_uint_eq(prec->usec, 3000000000);
			old_seen = true;
		} else if (prec->record_id == id.record_id) {
			ck_assert_uint_eq(prec->usec, 7000000000);
			new_seen = true;
		}
	}
	list_iterator_destroy(itr);
	ck_assert(old_seen && new_seen);
	/* A delayed completion may arrive after the numeric PID is reused. */
	ops.poll_data = _poll_retained_records;
	retired->task_completed = false;
	ck_assert_int_eq(jobacct_gather_complete_task(100, old->record_id,
						      NULL),
			 0);
	ck_assert(retired->task_completed);
	ck_assert(!current->task_completed);
	ck_assert_uint_eq(retired->user_cpu_sec, 3);
	ck_assert_uint_eq(current->user_cpu_sec, 7);
	FREE_NULL_LIST(task_list);
	FREE_NULL_LIST(prec_list);
}

END_TEST

START_TEST(test_supplemental_pid_reuse_keeps_memory_complete)
{
	darwin_task_t *retired = _task(100, 1), *active = _task(200, 1);
	jobacctinfo_t *old_record = jobacctinfo_create(NULL);
	jobacctinfo_t *active_record = jobacctinfo_create(NULL);
	list_t *records = list_create(jobacctinfo_destroy);
	jag_callbacks_t callbacks = {
		.get_precs = _get_precs,
		.get_offspring_data = _get_offspring_data,
	};
	pid_t reused = _i ? 101 : 100;

	plugin_inited = PLUGIN_INITED;
	slurm_conf.job_acct_oom_kill = true;
	jobacct_step_id.job_id = 1;
	jobacct_mem_limit = 1024;
	jag_common_init(1000000000L);
	old_record->pid = 100;
	old_record->id.record_id = retired->record_id;
	old_record->task_completed = true;
	active_record->pid = 200;
	active_record->id.record_id = active->record_id;
	list_append(records, old_record);
	list_append(records, active_record);
	_process(100, 999, 1, 1000000000, 0);
	if (_i)
		_process(101, 100, 2, 2000000000, 0);
	_process(200, 999, 1, 3000000000, 4096);
	_assign_owners();
	_sum_usage();

	/* Only the retained numeric PID points at the unrelated replacement. */
	enumerated_pid = 200;
	next_sample = (darwin_proc_sample_t) {
		.pid = reused,
		.ppid = 999,
		.start_sec = 9,
		.start_abstime = 9,
		.user_ns = 9000000000,
		.rss = 65536,
	};
	extra_samples[0] = (darwin_proc_sample_t) {
		.pid = 200,
		.ppid = 999,
		.start_sec = 1,
		.start_abstime = 1,
		.user_ns = 3000000000,
		.rss = 4096,
		.vsize = 8192,
	};
	extra_sample_count = 1;
	for (int poll = 0; poll < 2; poll++) {
		jag_common_poll_data(records, 1, &callbacks, true);
		ck_assert(!callbacks.memory_incomplete);
		ck_assert(!incomplete_reported);
		ck_assert_int_eq(list_count(processes), 1);
		ck_assert_uint_eq(active_record
					  ->tres_usage_in_tot[TRES_ARRAY_MEM],
				  4096);
		ck_assert_uint_eq(active_record
					  ->tres_usage_in_tot[TRES_ARRAY_VMEM],
				  8192);
		ck_assert_uint_eq(old_record->user_cpu_sec, _i ? 3 : 1);
		ck_assert_uint_eq(memory_cancellations, poll + 1);
	}
	ck_assert_uint_eq(profile_checks, 2);

	/* A fresh tracker member with unresolved ancestry still is incomplete. */
	enumerated_pid = reused;
	jag_common_poll_data(records, 1, &callbacks, true);
	ck_assert(callbacks.memory_incomplete);
	ck_assert(incomplete_reported);
	ck_assert_int_eq(list_count(processes), 2);
	ck_assert_uint_eq(active_record->tres_usage_in_tot[TRES_ARRAY_MEM],
			  INFINITE64);
	ck_assert_uint_eq(profile_checks, 2);
	ck_assert_uint_eq(memory_cancellations, 2);
	FREE_NULL_LIST(records);
	FREE_NULL_LIST(prec_list);
}

END_TEST

START_TEST(test_extern_placeholder_has_no_process_identity)
{
	jobacctinfo_t *placeholder = jobacctinfo_create(NULL);
	jag_callbacks_t callbacks = {
		.get_precs = _get_precs,
		.get_offspring_data = _get_offspring_data,
	};
	list_t *records = list_create(jobacctinfo_destroy);

	jag_common_init(1000000000L);
	placeholder->pid = 0;
	list_append(records, placeholder);
	_get_precs(records, 1, &callbacks);
	ck_assert(!callbacks.memory_incomplete);
	ck_assert(!incomplete_reported);
	ck_assert_int_eq(list_count(prec_list), 0);
	jag_common_poll_data(records, 1, &callbacks, false);
	ck_assert_uint_eq(energy_samples, 1);
	ck_assert_uint_eq(placeholder->energy.consumed_energy, 123);
	ck_assert_uint_eq(placeholder->tres_usage_in_tot[TRES_ARRAY_ENERGY],
			  123);
	FREE_NULL_LIST(records);
	FREE_NULL_LIST(prec_list);
}

END_TEST

START_TEST(test_incomplete_measurements_are_reported)
{
	darwin_task_t *owner = _task(100, 1);
	jobacctinfo_t *record = jobacctinfo_create(NULL);
	jag_callbacks_t callbacks = { 0 };
	list_t *records = list_create(jobacctinfo_destroy);

	jag_common_init(1000000000L);
	record->pid = 100;
	record->id.record_id = owner->record_id;
	list_append(records, record);
	/* No remaining processes after the tracker has completed cleanup. */
	next_sample.pid = 0;
	enumeration_error = ESRCH;
	_get_precs(records, 1, &callbacks);
	ck_assert(callbacks.memory_incomplete);
	ck_assert(!incomplete_reported);

	/* Actual tracker failure remains visible even when its list is empty. */
	enumeration_error = EIO;
	_get_precs(records, 1, &callbacks);
	ck_assert(callbacks.memory_incomplete);
	ck_assert(incomplete_reported);

	/* Permission failure cannot turn an unreadable owned process into zero. */
	incomplete_reported = false;
	enumeration_error = 0;
	_process(100, 999, 1, 1000000000, 4096)->owner = owner;
	next_sample.pid = 100;
	sampling_error = EPERM;
	_get_precs(records, 1, &callbacks);
	ck_assert(callbacks.memory_incomplete);
	ck_assert(incomplete_reported);
	ck_assert_uint_eq(((jag_prec_t *) list_peek(prec_list))
				  ->tres_data[TRES_ARRAY_MEM]
				  .size_read,
			  INFINITE64);
	FREE_NULL_LIST(records);
	FREE_NULL_LIST(prec_list);
}

END_TEST

START_TEST(test_incomplete_memory_does_not_export_stale_totals)
{
	darwin_task_t *owner = _task(100, 1);
	jobacctinfo_t *record = jobacctinfo_create(NULL);
	jobacctinfo_t *aggregate;
	jag_callbacks_t callbacks = {
		.get_precs = _get_precs,
		.get_offspring_data = _get_offspring_data,
	};
	list_t *records = list_create(jobacctinfo_destroy);
	uint64_t rss, vsize;

	plugin_inited = PLUGIN_INITED;
	jag_common_init(1000000000L);
	record->pid = 100;
	record->id.record_id = owner->record_id;
	list_append(records, record);
	next_sample = (darwin_proc_sample_t) {
		.pid = 100,
		.ppid = 999,
		.start_sec = 1,
		.start_abstime = 1,
		.user_ns = 1000000000,
		.read_bytes = 4096,
		.rss = 100 * 1024 * 1024,
		.vsize = 200 * 1024 * 1024,
	};
	jag_common_poll_data(records, 1, &callbacks, false);
	ck_assert_uint_eq(record->tres_usage_in_tot[TRES_ARRAY_MEM],
			  100 * 1024 * 1024);

	/* A failed refresh must not expose the previous sample to slurmd. */
	for (int failure = 0; failure < 2; failure++) {
		sampling_error = failure ? 0 : EPERM;
		enumeration_error = failure ? EIO : 0;
		jag_common_poll_data(records, 1, &callbacks, true);
		ck_assert(callbacks.memory_incomplete);
		ck_assert_uint_eq(profile_checks, 0);
		aggregate = jobacctinfo_create(NULL);
		jobacctinfo_aggregate(aggregate, record);
		jobacctinfo_getinfo(aggregate, JOBACCT_DATA_TOT_RSS, &rss,
				    SLURM_PROTOCOL_VERSION);
		jobacctinfo_getinfo(aggregate, JOBACCT_DATA_TOT_VSIZE, &vsize,
				    SLURM_PROTOCOL_VERSION);
		ck_assert_uint_eq(rss, INFINITE64);
		ck_assert_uint_eq(vsize, INFINITE64);
		ck_assert_uint_eq(aggregate->tres_usage_in_tot[TRES_ARRAY_CPU],
				  CPU_TIME_ADJ);
		ck_assert_uint_eq(
			aggregate->tres_usage_in_tot[TRES_ARRAY_FS_DISK], 4096);
		ck_assert_uint_eq(aggregate->tres_usage_in_max[TRES_ARRAY_MEM],
				  100 * 1024 * 1024);
		ck_assert_uint_eq(aggregate->tres_usage_in_max[TRES_ARRAY_VMEM],
				  200 * 1024 * 1024);
		jobacctinfo_destroy(aggregate);
	}

	/* A complete snapshot restores current values without losing peaks. */
	sampling_error = enumeration_error = 0;
	next_sample.rss = 10 * 1024 * 1024;
	next_sample.vsize = 20 * 1024 * 1024;
	jag_common_poll_data(records, 1, &callbacks, true);
	ck_assert(!callbacks.memory_incomplete);
	ck_assert_uint_eq(profile_checks, 1);
	ck_assert_uint_eq(record->tres_usage_in_tot[TRES_ARRAY_MEM],
			  10 * 1024 * 1024);
	ck_assert_uint_eq(record->tres_usage_in_tot[TRES_ARRAY_VMEM],
			  20 * 1024 * 1024);
	ck_assert_uint_eq(record->tres_usage_in_max[TRES_ARRAY_MEM],
			  100 * 1024 * 1024);
	ck_assert_uint_eq(record->tres_usage_in_min[TRES_ARRAY_MEM],
			  100 * 1024 * 1024);
	FREE_NULL_LIST(records);
	FREE_NULL_LIST(prec_list);
}

END_TEST

int main(void)
{
	Suite *suite = suite_create("Darwin accounting");
	TCase *test = tcase_create("process lifetime");
	SRunner *runner;
	int failed;

	tcase_add_checked_fixture(test, _setup, _teardown);
	tcase_add_test(test, test_task_ownership);
	tcase_add_test(test, test_exit_and_reparent);
	tcase_add_test(test, test_pid_reuse);
	tcase_add_test(test, test_reused_parent);
	tcase_add_test(test, test_final_rusage);
	tcase_add_test(test, test_unknown_frequency);
	tcase_add_test(test, test_registration_failure);
	tcase_add_test(test,
		       test_parent_exit_final_sample_keeps_live_child_usage);
	tcase_add_test(test, test_completed_owner_is_retained_until_cleanup);
	tcase_add_loop_test(test,
			    test_final_wait_preserves_disjoint_descendants, 0,
			    4);
	tcase_add_test(test, test_retained_owner_cookie_survives_pid_reuse);
	tcase_add_loop_test(test,
			    test_supplemental_pid_reuse_keeps_memory_complete,
			    0, 2);
	tcase_add_test(test, test_extern_placeholder_has_no_process_identity);
	tcase_add_test(test, test_incomplete_measurements_are_reported);
	tcase_add_test(test,
		       test_incomplete_memory_does_not_export_stale_totals);
	suite_add_tcase(suite, test);
	runner = srunner_create(suite);
	srunner_run_all(runner, CK_ENV);
	failed = srunner_ntests_failed(runner);
	srunner_free(runner);
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
