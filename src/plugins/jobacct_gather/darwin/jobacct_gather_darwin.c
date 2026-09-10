/*****************************************************************************\
 *  jobacct_gather_darwin.c - native macOS process accounting.
 *****************************************************************************
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include "darwin_proc.h"
#include "src/common/assoc_mgr.h"
#include "src/common/list.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurm_xlator.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"
#include "src/interfaces/acct_gather_energy.h"
#include "src/interfaces/jobacct_gather.h"
#include "src/interfaces/proctrack.h"

#include "../common/common_jag.h"

const char plugin_name[] = "Job accounting gather macOS plugin";
const char plugin_type[] = "jobacct_gather/darwin";
const uint32_t plugin_version = SLURM_VERSION_NUMBER;

typedef struct {
	pid_t pid;
	uint64_t record_id;
	uint64_t start_sec, start_usec;
	uint64_t start_abstime;
	bool identity_valid;
	bool registered;
	bool memory_unknown;
	bool final_usage_applied;
	uint64_t user_ns, system_ns;
	uint64_t read_bytes, write_bytes;
	uint64_t rss, vsize;
} darwin_task_t;

typedef struct {
	darwin_proc_sample_t sample;
	darwin_task_t *owner;
	bool seen;
	bool gone;
	uint64_t user_ns, system_ns;
	/* This identity's own CPU, including any observed counter resets. */
	uint64_t accounted_user_ns, accounted_system_ns;
	uint64_t read_bytes, write_bytes;
} darwin_process_t;

static list_t *tasks;
static list_t *processes;
static bool sample_failed;
static bool incomplete_reported;

static void _destroy(void *ptr)
{
	xfree(ptr);
}

static int _find_task_record(void *ptr, void *key)
{
	darwin_task_t *task = ptr;
	return task->record_id == *(uint64_t *) key;
}

static int _find_process(void *ptr, void *key)
{
	darwin_process_t *process = ptr;
	return process->sample.pid == *(pid_t *) key;
}

static int _find_owner(void *ptr, void *key)
{
	darwin_process_t *process = ptr;
	return process->owner == key;
}

static int _find_gone(void *ptr, void *key)
{
	darwin_process_t *process = ptr;
	return process->gone;
}

static bool _same_process(darwin_proc_sample_t *a, darwin_proc_sample_t *b)
{
	return ((a->pid == b->pid) && (a->start_sec == b->start_sec) &&
		(a->start_usec == b->start_usec) &&
		(a->start_abstime == b->start_abstime));
}

static bool _task_identity(darwin_task_t *task, darwin_proc_sample_t *sample)
{
	return (task->identity_valid && (task->pid == sample->pid) &&
		(task->start_sec == sample->start_sec) &&
		(task->start_usec == sample->start_usec) &&
		(task->start_abstime == sample->start_abstime));
}

static int _find_task_identity(void *ptr, void *key)
{
	return _task_identity(ptr, key);
}

static void _sample_pid(pid_t pid, bool member)
{
	darwin_proc_sample_t sample;
	darwin_process_t *process;
	int rc;

	if (pid <= 0)
		return;
	process = list_find_first(processes, _find_process, &pid);
	if (process && process->seen)
		return;
	if ((rc = darwin_proc_sample(pid, &sample))) {
		if ((rc == ESRCH) && process) {
			process->gone = true;
		} else if (rc != ESRCH) {
			sample_failed = true;
			if (process && process->owner)
				process->owner->memory_unknown = true;
			log_flag(JAG, "Cannot sample pid %d: %s", pid,
				 slurm_strerror(rc));
		}
		return;
	}
	if (process && !_same_process(&process->sample, &sample)) {
		/* Previously observed usage is already in the original task. */
		_destroy(list_remove_first(processes, _find_process, &pid));
		process = NULL;
	}
	if (!process) {
		/*
		 * Supplemental PIDs only keep a previously owned identity alive.
		 * A recycled PID is a new member only if the current inventory or
		 * an explicit task registration independently identifies it.
		 */
		if (!member &&
		    !list_find_first(tasks, _find_task_identity, &sample))
			return;
		process = xmalloc(sizeof(*process));
		list_append(processes, process);
	}
	process->sample = sample;
	process->seen = true;
	process->gone = false;
}

static void _assign_owners(void)
{
	bool progress;
	list_itr_t *itr = list_iterator_create(processes);
	darwin_process_t *process, *parent;
	darwin_task_t *task;

	/* Parent order is arbitrary in the kernel process enumeration. */
	do {
		progress = false;
		list_iterator_reset(itr);
		while ((process = list_next(itr))) {
			if (!process->seen || process->owner)
				continue;
			task = list_find_first(tasks, _find_task_identity,
					       &process->sample);
			if (task) {
				process->owner = task;
				progress = true;
				continue;
			}
			parent = list_find_first(processes, _find_process,
						 &process->sample.ppid);
			if (!parent || !parent->seen || !parent->owner)
				continue;
			/* A newly reused parent PID must not acquire older children. */
			if (parent->sample.start_abstime >
			    process->sample.start_abstime)
				continue;
			process->owner = parent->owner;
			progress = true;
		}
	} while (progress);
	list_iterator_destroy(itr);
}

/* Some counters reset across exec. Never subtract unsigned counters blindly. */
static uint64_t _delta(uint64_t value, uint64_t *previous)
{
	uint64_t delta = (value >= *previous) ? value - *previous : value;
	*previous = value;
	return delta;
}

static void _sum_usage(void)
{
	list_itr_t *itr = list_iterator_create(processes);
	darwin_process_t *process;

	while ((process = list_next(itr))) {
		darwin_proc_sample_t *sample = &process->sample;
		darwin_task_t *task = process->owner;
		uint64_t user_ns, system_ns;

		if (!process->seen || !task)
			continue;
		user_ns = _delta(sample->user_ns, &process->user_ns);
		system_ns = _delta(sample->system_ns, &process->system_ns);
		process->accounted_user_ns += user_ns;
		process->accounted_system_ns += system_ns;
		task->user_ns += user_ns;
		task->system_ns += system_ns;
		task->read_bytes +=
			_delta(sample->read_bytes, &process->read_bytes);
		task->write_bytes +=
			_delta(sample->write_bytes, &process->write_bytes);
		task->rss += sample->rss;
		task->vsize += sample->vsize;
	}
	list_iterator_destroy(itr);
}

static void _apply_final_usage(darwin_task_t *task, jobacctinfo_t *jobacct)
{
	list_itr_t *itr;
	darwin_process_t *process;
	struct rusage *usage = &jobacct->final_rusage;
	uint64_t live_user_ns = 0, live_system_ns = 0;
	uint64_t user_ns, system_ns;

	if (!jobacct->final_rusage_valid || task->final_usage_applied)
		return;

	/*
	 * wait4 includes descendants reaped through the direct child's ancestry.
	 * CPU belonging to descendants still observed after that wait is disjoint
	 * from it. Everything else may overlap, so take its maximum with wait
	 * usage instead of adding the entire wait value to the sampled tree.
	 */
	itr = list_iterator_create(processes);
	while ((process = list_next(itr))) {
		if ((process->owner != task) || !process->seen ||
		    _task_identity(task, &process->sample))
			continue;
		live_user_ns += process->accounted_user_ns;
		live_system_ns += process->accounted_system_ns;
	}
	list_iterator_destroy(itr);
	user_ns = (uint64_t) usage->ru_utime.tv_sec * 1000000000 +
		  (uint64_t) usage->ru_utime.tv_usec * 1000;
	system_ns = (uint64_t) usage->ru_stime.tv_sec * 1000000000 +
		    (uint64_t) usage->ru_stime.tv_usec * 1000;
	task->user_ns = MAX(task->user_ns, user_ns + live_user_ns);
	task->system_ns = MAX(task->system_ns, system_ns + live_system_ns);
	/* Keep the correction when these descendants grow or disappear later. */
	task->final_usage_applied = true;
}

/* Records already contain per-task totals; common_jag must not sum them again. */
static void _get_offspring_data(list_t *list, jag_prec_t *ancestor, pid_t pid,
				jag_prec_t *permanent_ancestor)
{
}

static list_t *_get_precs(list_t *task_list, uint64_t cont_id,
			  jag_callbacks_t *callbacks)
{
	list_itr_t *itr;
	darwin_task_t *task;
	darwin_process_t *process;
	jobacctinfo_t *jobacct;
	pid_t *pids = NULL;
	int npids = 0, count;
	bool enumeration_failed, any_seen = false, unresolved = false;
	int enumeration_errno;

	list_flush(prec_list);
	sample_failed = false;
	itr = list_iterator_create(tasks);
	while ((task = list_next(itr))) {
		task->registered = false;
		task->memory_unknown = false;
		task->rss = task->vsize = 0;
	}
	list_iterator_destroy(itr);
	itr = list_iterator_create(task_list);
	while ((jobacct = list_next(itr))) {
		task = list_find_first(tasks, _find_task_record,
				       &jobacct->id.record_id);
		if (task)
			task->registered = true;
	}
	list_iterator_destroy(itr);
	itr = list_iterator_create(tasks);
	while ((task = list_next(itr))) {
		if (!task->registered) {
			list_delete_all(processes, _find_owner, task);
			list_delete_item(itr);
		}
	}
	list_iterator_destroy(itr);

	errno = 0;
	enumeration_failed =
		(proctrack_g_get_pids(cont_id, &pids, &npids) != SLURM_SUCCESS);
	enumeration_errno = errno;
	if (npids < 0)
		npids = 0;
	count = npids;
	xrealloc(pids, sizeof(*pids) * (count + list_count(processes) +
					list_count(tasks) + 1));
	itr = list_iterator_create(processes);
	while ((process = list_next(itr))) {
		process->seen = false;
		/* Continue observing known descendants after reparenting/setsid. */
		pids[count++] = process->sample.pid;
	}
	list_iterator_destroy(itr);
	itr = list_iterator_create(tasks);
	while ((task = list_next(itr)))
		pids[count++] = task->pid;
	list_iterator_destroy(itr);
	for (int i = 0; i < count; i++) {
		if (pids[i] != getpid())
			_sample_pid(pids[i], i < npids);
	}
	xfree(pids);
	_assign_owners();
	_sum_usage();
	/* Cumulative counters live in the task after a sampled process exits. */
	list_delete_all(processes, _find_gone, NULL);
	callbacks->memory_incomplete = enumeration_failed || sample_failed;
	itr = list_iterator_create(processes);
	while ((process = list_next(itr))) {
		any_seen |= process->seen;
		if (process->seen && !process->owner) {
			callbacks->memory_incomplete = true;
			unresolved = true;
			log_flag(JAG, "No accounting task ancestry for pid %d",
				 process->sample.pid);
		}
	}
	list_iterator_destroy(itr);

	itr = list_iterator_create(task_list);
	while ((jobacct = list_next(itr))) {
		jag_prec_t *prec;

		/* PID zero is the extern energy/statistics placeholder. */
		if (!jobacct->pid)
			continue;
		task = list_find_first(tasks, _find_task_record,
				       &jobacct->id.record_id);
		if (!task || !task->identity_valid) {
			callbacks->memory_incomplete = true;
			unresolved = true;
			continue;
		}
		if (task->memory_unknown)
			callbacks->memory_incomplete = true;
		_apply_final_usage(task, jobacct);
		prec = xmalloc(sizeof(*prec));
		prec->pid = task->pid;
		prec->record_id = task->record_id;
		prec->usec = task->user_ns;
		prec->ssec = task->system_ns;
		prec->tres_count = jobacct->tres_count;
		prec->tres_data =
			xcalloc(prec->tres_count, sizeof(*prec->tres_data));
		for (int i = 0; i < prec->tres_count; i++) {
			prec->tres_data[i].num_reads = INFINITE64;
			prec->tres_data[i].num_writes = INFINITE64;
			prec->tres_data[i].size_read = INFINITE64;
			prec->tres_data[i].size_write = INFINITE64;
		}
		if (!enumeration_failed && !task->memory_unknown) {
			prec->tres_data[TRES_ARRAY_MEM].size_read = task->rss;
			prec->tres_data[TRES_ARRAY_VMEM].size_read =
				task->vsize;
		}
		prec->tres_data[TRES_ARRAY_FS_DISK].size_read =
			task->read_bytes;
		prec->tres_data[TRES_ARRAY_FS_DISK].size_write =
			task->write_bytes;
		list_append(prec_list, prec);
	}
	list_iterator_destroy(itr);
	/* A successful cleanup can remove the container before polling stops. */
	if (!incomplete_reported &&
	    (sample_failed || unresolved ||
	     (enumeration_failed &&
	      (enumeration_errno != ESRCH || any_seen)))) {
		warning("Incomplete Darwin task accounting; skipping memory-limit checks");
		incomplete_reported = true;
	}
	return prec_list;
}

extern int init(void)
{
	if (running_in_slurmstepd()) {
		incomplete_reported = false;
		tasks = list_create(_destroy);
		processes = list_create(_destroy);
		/* darwin_proc_sample normalizes Mach CPU ticks to nanoseconds. */
		jag_common_init(1000000000L);
	}
	debug("%s loaded", plugin_name);
	return SLURM_SUCCESS;
}

extern void fini(void)
{
	if (running_in_slurmstepd()) {
		acct_gather_energy_fini();
		FREE_NULL_LIST(processes);
		FREE_NULL_LIST(tasks);
		jag_common_fini();
	}
}

extern void jobacct_gather_p_poll_data(list_t *task_list, int64_t cont_id,
				       bool profile)
{
	jag_callbacks_t callbacks = {
		.get_precs = _get_precs,
		.get_offspring_data = _get_offspring_data,
	};

	xassert(running_in_slurmstepd());
	if (task_list)
		jag_common_poll_data(task_list, cont_id, &callbacks, profile);
}

extern int jobacct_gather_p_endpoll(void)
{
	FREE_NULL_LIST(processes);
	FREE_NULL_LIST(tasks);
	jag_common_fini();
	return SLURM_SUCCESS;
}

extern int jobacct_gather_p_add_task(pid_t pid, jobacct_id_t *jobacct_id)
{
	darwin_proc_sample_t sample;
	darwin_task_t *task;
	int rc;

	/* The extern step uses PID zero only as an aggregate stats record. */
	if (!pid)
		return SLURM_SUCCESS;
	if ((rc = darwin_proc_sample(pid, &sample))) {
		error("Cannot establish accounting identity for pid %d: %s", pid,
		      slurm_strerror(rc));
		errno = rc;
		return SLURM_ERROR;
	}
	task = xmalloc(sizeof(*task));
	task->pid = pid;
	task->record_id = jobacct_id->record_id;
	task->start_sec = sample.start_sec;
	task->start_usec = sample.start_usec;
	task->start_abstime = sample.start_abstime;
	task->identity_valid = true;
	list_append(tasks, task);
	return SLURM_SUCCESS;
}

extern void jobacct_gather_p_stat_job(jobacctinfo_t *jobacct) {}
