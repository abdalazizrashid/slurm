/*****************************************************************************\
 *  proctrack_darwin.c - native observed-lineage process tracking.
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

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <sys/event.h>
#include <sys/proc.h>
#include <time.h>
#include <unistd.h>

#include "src/common/log.h"
#include "src/common/read_config.h"
#include "src/common/xmalloc.h"
#include "src/interfaces/proctrack.h"

const char plugin_name[] = "Native macOS observed-lineage process tracking";
const char plugin_type[] = "proctrack/darwin";
const uint32_t plugin_version = SLURM_VERSION_NUMBER;

/*
 * NOTE_TRACK/NOTE_CHILD are unsupported on Darwin. Fork notifications wake a
 * public libproc child scan; they neither name nor atomically register children.
 * A child that loses its parent before observation can escape this tracker.
 */
typedef struct {
	pid_t pid, ppid;
	uint64_t sec, usec, abstime;
} darwin_identity_t;

typedef struct tracked_process {
	struct tracked_process *next;
	darwin_identity_t identity;
	uintptr_t token;
	bool live, watched;
} tracked_process_t;

typedef struct tracked_container {
	struct tracked_container *next;
	tracked_process_t *processes;
	uint64_t id;
	bool incomplete;
} tracked_container_t;

static pthread_mutex_t tracking_lock = PTHREAD_MUTEX_INITIALIZER;
static tracked_container_t *containers;
static pthread_t monitor_thread;
static int event_fd = -1;
static bool stopping;
static uintptr_t next_token;
static uint32_t next_container;

static int _identity(pid_t pid, darwin_identity_t *identity)
{
	struct proc_bsdinfo before, after;
	struct rusage_info_v2 usage;
	int rc;

	if (pid <= 1 || pid == getpid())
		return EINVAL;
	errno = 0;
	rc = proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &before, sizeof(before));
	if (rc != sizeof(before))
		return errno ? errno : ESRCH;
	if (before.pbi_status == SZOMB)
		return ESRCH;
	if (proc_pid_rusage(pid, RUSAGE_INFO_V2, (rusage_info_t *) &usage))
		return errno ? errno : EIO;
	errno = 0;
	rc = proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &after, sizeof(after));
	if (rc != sizeof(after))
		return errno ? errno : ESRCH;
	if ((before.pbi_start_tvsec != after.pbi_start_tvsec) ||
	    (before.pbi_start_tvusec != after.pbi_start_tvusec))
		return ESRCH;
	*identity = (darwin_identity_t) {
		.pid = pid,
		.ppid = after.pbi_ppid,
		.sec = after.pbi_start_tvsec,
		.usec = after.pbi_start_tvusec,
		.abstime = usage.ri_proc_start_abstime,
	};
	return 0;
}

static bool _same(darwin_identity_t *a, darwin_identity_t *b)
{
	return a->pid == b->pid && a->sec == b->sec && a->usec == b->usec &&
	       a->abstime == b->abstime;
}

static tracked_container_t *_container(uint64_t id)
{
	tracked_container_t *container;
	for (container = containers; container; container = container->next)
		if (container->id == id)
			return container;
	return NULL;
}

static tracked_process_t *_find(tracked_container_t *container, pid_t pid)
{
	tracked_process_t *process;
	for (process = container->processes; process; process = process->next)
		if (process->live && process->identity.pid == pid)
			return process;
	return NULL;
}

static void _unwatch(tracked_process_t *process)
{
	struct kevent event;
	if (!process->watched)
		return;
	EV_SET(&event, process->identity.pid, EVFILT_PROC, EV_DELETE, 0, 0,
	       NULL);
	kevent(event_fd, &event, 1, NULL, 0, NULL);
	process->watched = false;
}

static int _add(tracked_container_t *container, darwin_identity_t *identity)
{
	tracked_process_t *process = _find(container, identity->pid);
	tracked_container_t *other;
	struct kevent event;
	darwin_identity_t verified;
	int rc;

	if (process && _same(&process->identity, identity))
		return 0;
	for (other = containers; other; other = other->next) {
		process = _find(other, identity->pid);
		if (!process)
			continue;
		if (_same(&process->identity, identity))
			return EEXIST;
		_unwatch(process);
		process->live = false;
	}
	if (next_token == UINTPTR_MAX)
		return EOVERFLOW;
	process = xmalloc(sizeof(*process));
	process->identity = *identity;
	process->token = ++next_token;
	EV_SET(&event, identity->pid, EVFILT_PROC, EV_ADD | EV_CLEAR,
	       NOTE_FORK | NOTE_EXEC | NOTE_EXIT, 0, (void *) process->token);
	if (kevent(event_fd, &event, 1, NULL, 0, NULL) < 0) {
		rc = errno;
		xfree(process);
		return rc;
	}
	process->watched = true;
	rc = _identity(identity->pid, &verified);
	if (rc || !_same(identity, &verified)) {
		_unwatch(process);
		xfree(process);
		return rc ? rc : ESRCH;
	}
	process->live = true;
	process->next = container->processes;
	container->processes = process;
	return 0;
}

static int _children(pid_t pid, pid_t **children, int *count)
{
	int capacity = 64 * sizeof(pid_t), bytes;
	pid_t *pids = NULL;

	for (int attempt = 0; attempt < 8; attempt++) {
		xrealloc(pids, capacity);
		errno = 0;
		bytes = proc_listpids(PROC_PPID_ONLY, pid, pids, capacity);
		if (bytes < 0 || (!bytes && errno)) {
			int rc = errno ? errno : EIO;
			xfree(pids);
			return rc;
		}
		if (bytes < capacity) {
			*children = pids;
			*count = bytes / sizeof(pid_t);
			return 0;
		}
		if (capacity > INT_MAX / 2)
			break;
		capacity *= 2;
	}
	xfree(pids);
	return EOVERFLOW;
}

static void _refresh(tracked_container_t *container)
{
	tracked_process_t *process;
	tracked_process_t **link;
	/* Bound work even if the workload keeps forking during discovery. */
	int budget = 4096;
	bool added;

	do {
		added = false;
		for (process = container->processes; process;
		     process = process->next) {
			darwin_identity_t current, child;
			pid_t *children = NULL;
			int count, rc;

			if (!process->live)
				continue;
			rc = _identity(process->identity.pid, &current);
			if (rc || !_same(&current, &process->identity)) {
				if (rc && rc != ESRCH)
					container->incomplete = true;
				if (!rc || rc == ESRCH) {
					_unwatch(process);
					process->live = false;
				}
				continue;
			}
			rc = _children(current.pid, &children, &count);
			if (rc) {
				if (rc != ESRCH)
					container->incomplete = true;
				continue;
			}
			for (int i = 0; i < count; i++) {
				if (_find(container, children[i]))
					continue;
				if (--budget < 0) {
					container->incomplete = true;
					break;
				}
				rc = _identity(children[i], &child);
				if (!rc && child.ppid == current.pid &&
				    child.abstime >= current.abstime) {
					/* Reject a recycled parent after the child snapshot. */
					darwin_identity_t parent;
					rc = _identity(current.pid, &parent);
					if (!rc && _same(&parent, &current)) {
						rc = _add(container, &child);
						if (!rc)
							added = true;
					}
				}
				if (rc && rc != ESRCH)
					container->incomplete = true;
			}
			xfree(children);
			if (budget < 0)
				return;
		}
	} while (added);
	/* Pending events carry numeric tokens, so dead records can be freed. */
	link = &container->processes;
	while ((process = *link)) {
		if (process->live) {
			link = &process->next;
			continue;
		}
		*link = process->next;
		xfree(process);
	}
}

static void _events(struct kevent *events, int count)
{
	tracked_container_t *container;
	tracked_process_t *process;
	for (int i = 0; i < count; i++) {
		if (events[i].filter != EVFILT_PROC)
			continue;
		for (container = containers; container;
		     container = container->next) {
			for (process = container->processes; process;
			     process = process->next) {
				if (process->token !=
				    (uintptr_t) events[i].udata)
					continue;
				if (events[i].flags & EV_ERROR)
					container->incomplete = true;
				/*
				 * Exit events are wakeups, not the authoritative PID
				 * identity; _refresh checks process generation again.
				 */
			}
		}
	}
}

static void *_monitor(void *unused)
{
	struct kevent events[64];
	const struct timespec interval = { .tv_nsec = 250000000 };
	tracked_container_t *container;
	int count, event_error;

	while (true) {
		count = kevent(event_fd, NULL, 0, events, 64, &interval);
		event_error = errno;
		pthread_mutex_lock(&tracking_lock);
		if (stopping) {
			pthread_mutex_unlock(&tracking_lock);
			return NULL;
		}
		if (count >= 0)
			_events(events, count);
		for (container = containers; container;
		     container = container->next) {
			if (count < 0 && event_error != EINTR)
				container->incomplete = true;
			_refresh(container);
		}
		pthread_mutex_unlock(&tracking_lock);
		if (count < 0 && event_error != EINTR)
			nanosleep(&interval, NULL);
	}
}

static void _destroy(tracked_container_t *container)
{
	tracked_container_t **link = &containers;
	tracked_process_t *process;
	while (*link != container)
		link = &(*link)->next;
	*link = container->next;
	while ((process = container->processes)) {
		container->processes = process->next;
		_unwatch(process);
		xfree(process);
	}
	xfree(container);
}

extern int init(void)
{
	return SLURM_SUCCESS;
}

extern void fini(void)
{
	struct kevent event;
	pthread_mutex_lock(&tracking_lock);
	if (event_fd < 0) {
		pthread_mutex_unlock(&tracking_lock);
		return;
	}
	stopping = true;
	EV_SET(&event, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
	kevent(event_fd, &event, 1, NULL, 0, NULL);
	pthread_mutex_unlock(&tracking_lock);
	pthread_join(monitor_thread, NULL);
	pthread_mutex_lock(&tracking_lock);
	while (containers)
		_destroy(containers);
	close(event_fd);
	event_fd = -1;
	pthread_mutex_unlock(&tracking_lock);
}

extern int proctrack_p_create(stepd_step_rec_t *step)
{
	tracked_container_t *container;
	struct kevent event;
	int rc = 0;

	if (step->flags & LAUNCH_WAIT_FOR_CHILDREN) {
		errno = ENOTSUP;
		return SLURM_ERROR;
	}
	pthread_mutex_lock(&tracking_lock);
	if (event_fd < 0) {
		event_fd = kqueue();
		if (event_fd < 0) {
			rc = errno;
			goto done;
		}
		/* kqueue descriptors are not inherited by forked children. */
		if (fcntl(event_fd, F_SETFD, FD_CLOEXEC) < 0) {
			rc = errno;
			goto close_fd;
		}
		EV_SET(&event, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
		if (kevent(event_fd, &event, 1, NULL, 0, NULL) < 0) {
			rc = errno;
			goto close_fd;
		}
		stopping = false;
		rc = pthread_create(&monitor_thread, NULL, _monitor, NULL);
		if (rc)
			goto close_fd;
	}
	if (next_container == UINT32_MAX) {
		rc = EOVERFLOW;
		goto done;
	}
	container = xmalloc(sizeof(*container));
	container->id = ((uint64_t) getpid() << 32) | ++next_container;
	container->next = containers;
	containers = container;
	step->cont_id = container->id;
	goto done;
close_fd:
	close(event_fd);
	event_fd = -1;
done:
	pthread_mutex_unlock(&tracking_lock);
	errno = rc;
	return rc ? SLURM_ERROR : SLURM_SUCCESS;
}

extern int proctrack_p_add(stepd_step_rec_t *step, pid_t pid)
{
	tracked_container_t *container;
	darwin_identity_t identity;
	int rc = _identity(pid, &identity);

	if (rc) {
		errno = rc;
		return SLURM_ERROR;
	}
	pthread_mutex_lock(&tracking_lock);
	container = _container(step->cont_id);
	rc = container ? _add(container, &identity) : ESRCH;
	pthread_mutex_unlock(&tracking_lock);
	errno = rc;
	return rc ? SLURM_ERROR : SLURM_SUCCESS;
}

static int _signal(tracked_container_t *container, int signal)
{
	tracked_process_t *process;
	int rc = 0;

	_refresh(container);
	for (process = container->processes; process; process = process->next) {
		darwin_identity_t current;
		int sample_rc;
		if (!process->live)
			continue;
		sample_rc = _identity(process->identity.pid, &current);
		if (sample_rc || !_same(&current, &process->identity)) {
			if (sample_rc && sample_rc != ESRCH)
				rc = sample_rc;
			continue;
		}
		/* Darwin has no public pidfd signal API; a narrow race remains. */
		if (kill(current.pid, signal) && errno != ESRCH)
			rc = errno;
	}
	if (rc)
		container->incomplete = true;
	if (container->incomplete && !rc)
		rc = EIO;
	return rc;
}

extern int proctrack_p_signal(uint64_t id, int signal)
{
	tracked_container_t *container;
	int rc;
	pthread_mutex_lock(&tracking_lock);
	container = _container(id);
	rc = container ? _signal(container, signal) : ESRCH;
	pthread_mutex_unlock(&tracking_lock);
	errno = rc;
	return rc ? SLURM_ERROR : SLURM_SUCCESS;
}

extern int proctrack_p_destroy(uint64_t id)
{
	tracked_container_t *container;
	pthread_mutex_lock(&tracking_lock);
	if ((container = _container(id)))
		_destroy(container);
	pthread_mutex_unlock(&tracking_lock);
	return SLURM_SUCCESS;
}

extern uint64_t proctrack_p_find(pid_t pid)
{
	tracked_container_t *container;
	uint64_t id = 0;
	pthread_mutex_lock(&tracking_lock);
	for (container = containers; container; container = container->next) {
		darwin_identity_t current;
		tracked_process_t *process;

		_refresh(container);
		process = _find(container, pid);
		if (process && !_identity(pid, &current) &&
		    _same(&process->identity, &current)) {
			id = container->id;
			break;
		}
	}
	pthread_mutex_unlock(&tracking_lock);
	return id;
}

extern bool proctrack_p_has_pid(uint64_t id, pid_t pid)
{
	return proctrack_p_find(pid) == id && id != 0;
}

extern int proctrack_p_get_pids(uint64_t id, pid_t **pids, int *npids)
{
	tracked_container_t *container;
	tracked_process_t *process;
	int rc = 0;

	*pids = NULL;
	*npids = 0;
	pthread_mutex_lock(&tracking_lock);
	if (!(container = _container(id))) {
		rc = ESRCH;
		goto done;
	}
	_refresh(container);
	for (process = container->processes; process; process = process->next) {
		if (process->live)
			(*npids)++;
	}
	*pids = xcalloc(*npids, sizeof(pid_t));
	int index = 0;
	for (process = container->processes; process; process = process->next) {
		if (process->live)
			(*pids)[index++] = process->identity.pid;
	}
	if (container->incomplete)
		rc = EIO;
done:
	pthread_mutex_unlock(&tracking_lock);
	errno = rc;
	return rc ? SLURM_ERROR : SLURM_SUCCESS;
}

extern int proctrack_p_wait(uint64_t id)
{
	struct timespec start, now, pause = { .tv_nsec = 100000000 };
	int rc = 0;

	clock_gettime(CLOCK_MONOTONIC, &start);
	while (true) {
		tracked_container_t *container;
		tracked_process_t *process;
		bool live = false;

		pthread_mutex_lock(&tracking_lock);
		container = _container(id);
		if (!container) {
			rc = ESRCH;
			pthread_mutex_unlock(&tracking_lock);
			break;
		}
		rc = _signal(container, SIGKILL);
		_refresh(container);
		for (process = container->processes; process;
		     process = process->next)
			live |= process->live;
		if (!live) {
			if (!rc)
				_destroy(container);
			pthread_mutex_unlock(&tracking_lock);
			break;
		}
		pthread_mutex_unlock(&tracking_lock);
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec - start.tv_sec >=
		    slurm_conf.unkillable_timeout) {
			rc = ETIMEDOUT;
			break;
		}
		nanosleep(&pause, NULL);
	}
	if (rc)
		error("Native process tracker cannot confirm cleanup of %"PRIu64": %s",
		      id, slurm_strerror(rc));
	errno = rc;
	return rc ? SLURM_ERROR : SLURM_SUCCESS;
}

extern int proctrack_p_wait_for_any_task(stepd_step_rec_t *step,
					 stepd_step_task_info_t **task,
					 bool block)
{
	errno = ENOTSUP;
	return SLURM_ERROR;
}
