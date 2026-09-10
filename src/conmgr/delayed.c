/*****************************************************************************\
 *  delayed.c - definitions for delayed work in connection manager
 *****************************************************************************
 *  Copyright (C) SchedMD LLC.
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

#include <stdlib.h>
#include <time.h>
#ifdef __APPLE__
#include <fcntl.h>
#include <sys/event.h>
#include <unistd.h>
#endif

#include "src/common/macros.h"
#include "src/common/read_config.h"
#include "src/common/slurm_time.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#include "src/conmgr/conmgr.h"
#include "src/conmgr/delayed.h"
#include "src/conmgr/mgr.h"
#include "src/conmgr/signals.h"

#define CTIME_STR_LEN 72

typedef struct {
#define MAGIC_FOREACH_DELAYED_WORK 0xB233443A
	int magic; /* MAGIC_FOREACH_DELAYED_WORK */
	work_t *shortest;
} foreach_delayed_work_t;

#define MAGIC_FOREACH_CANCEL_WORK 0xA238483A

typedef struct {
	int magic; /* MAGIC_FOREACH_CANCEL_WORK */
	bool connections_only;
} foreach_cancel_work_t;

/* Timer state is independent of mgr.mutex: shutdown joins the timer thread. */
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static bool timer_initialized;
#ifdef __APPLE__
static int timer_fd = -1;
static pthread_t timer_thread;
static pthread_once_t timer_once = PTHREAD_ONCE_INIT;
static int timer_init_error;
static bool timer_running, timer_armed, timer_stop;

#else
/* timer to trigger SIGALRM */
static timer_t timer = {0};
#endif

static int _inspect_work(void *x, void *key);
static void _update_timer(work_t *shortest);
static bool _work_clear_time_delay(work_t *work);

static timespec_t _delay_now(void)
{
#ifdef __APPLE__
	timespec_t now;

	if (clock_gettime(CLOCK_MONOTONIC, &now))
		fatal("%s: clock_gettime failed: %m", __func__);
	return now;
#else
	return timespec_now();
#endif
}

static timespec_t _deadline(const work_t *work)
{
#ifdef __APPLE__
	return work->time_deadline;
#else
	return work->control.time_begin;
#endif
}

/*
 * Remove delay dependency and release work back into work queue
 *
 * WARNING: caller must hold mgr.mutex
 * IN x - work to release back to normal work handling.
 *	takes ownership of pointer.
 */
static void _release_work(void *x)
{
	work_t *work = x;
	xassert(work->magic == MAGIC_WORK);

	(void) _work_clear_time_delay(work);
	handle_work(true, work);
}

static int _cancel_work(void *x, void *key)
{
	work_t *work = x;
	foreach_cancel_work_t *args = key;

	xassert(work->magic == MAGIC_WORK);
	xassert(args->magic == MAGIC_FOREACH_CANCEL_WORK);

	if (args->connections_only && !work->ref)
		return 0;

	work->status = CONMGR_WORK_STATUS_CANCELLED;
	return 1;
}

extern void cancel_delayed_work(bool connections_only)
{
	foreach_cancel_work_t args = {
		.magic = MAGIC_FOREACH_CANCEL_WORK,
		.connections_only = connections_only,
	};

	if (!mgr.delayed_work || list_is_empty(mgr.delayed_work))
		return;

	log_flag(CONMGR, "%s: cancelling%s %d delayed work",
		 __func__, (connections_only ? " connection" : ""),
		 list_count(mgr.delayed_work));

	/* run everything immediately but with cancelled status */
	(void) list_delete_all(mgr.delayed_work, _cancel_work, &args);
}

static void _inspect(void)
{
	int count, total;
	foreach_delayed_work_t dargs = {
		.magic = MAGIC_FOREACH_DELAYED_WORK,
	};

	total = list_count(mgr.delayed_work);
	count = list_delete_all(mgr.delayed_work, _inspect_work, &dargs);
	_update_timer(dargs.shortest);

	log_flag(CONMGR, "%s: checked all timers and triggered %d/%d delayed work",
		 __func__, count, total);
}

#ifdef __APPLE__
/* Replace the one-shot timer. The relative interval includes system sleep. */
static int _arm_timer(timespec_t remaining)
{
	struct kevent change;
	intptr_t ns;
	int rc;

	if (remaining.tv_sec >= (INTPTR_MAX / NSEC_IN_SEC))
		ns = INTPTR_MAX;
	else
		ns = MAX(1, remaining.tv_sec * NSEC_IN_SEC + remaining.tv_nsec);

	EV_SET(&change, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT,
	       NOTE_NSECONDS | NOTE_MACH_CONTINUOUS_TIME, ns, NULL);
	do {
		rc = kevent(timer_fd, &change, 1, NULL, 0, NULL);
	} while ((rc < 0) && (errno == EINTR));
	return (rc < 0) ? errno : 0;
}

/* The timer thread never takes mgr.mutex or keeps a pointer to queued work. */
static void *_timer_wait(void *arg)
{
	(void) arg;
	while (true) {
		struct kevent event;
		int rc;

		rc = kevent(timer_fd, NULL, 0, &event, 1, NULL);
		if ((rc < 0) && (errno == EINTR))
			continue;
		if (rc != 1)
			fatal("%s: kevent timer wait failed: %m", __func__);

		pthread_mutex_lock(&mutex);
		if (timer_stop) {
			pthread_mutex_unlock(&mutex);
			return NULL;
		}
		if (timer_armed && (event.filter == EVFILT_TIMER)) {
			/* No pending process-wide SIGALRM can outlive shutdown. */
			if (signal_mgr_notify(SIGALRM) &&
			    (rc = _arm_timer((timespec_t) {
				     .tv_nsec = 10000000 })))
				fatal("%s: timer retry failed: %s", __func__,
				      slurm_strerror(rc));
		}
		pthread_mutex_unlock(&mutex);
	}
}

static void _timer_fork_child(void)
{
	/* No timer thread survives fork; conmgr's child handler disables conmgr. */
	timer_running = timer_armed = timer_initialized = false;
	timer_stop = true;
	/* Only the calling thread survives, so discard inherited wait state. */
	mutex = (pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER;
	/* kqueue descriptors are not inherited by fork children on Darwin. */
	timer_fd = -1;
}

static void _timer_register_fork(void)
{
	timer_init_error = pthread_atfork(NULL, NULL, _timer_fork_child);
}
#endif

static void _update_timer(work_t *shortest)
{
	int rc = 0;
	timespec_t begin = { 0 };

	if (shortest) {
		char str[CTIME_STR_LEN];

		begin = _deadline(shortest);
		timespec_ctime(shortest->control.time_begin, true, str,
			       sizeof(str));
		log_flag(CONMGR, "%s: setting conmgr timer for %s for %s()",
			 __func__, str, shortest->callback.func_name);
	} else {
		log_flag(CONMGR, "%s: disabling conmgr timer", __func__);
	}

	slurm_mutex_lock(&mutex);
	if (!timer_initialized) {
		slurm_mutex_unlock(&mutex);
		return;
	}
#ifdef __APPLE__
	timer_armed = (shortest != NULL);
	if (timer_armed) {
		const timespec_t now = _delay_now();
		timespec_t remaining = { 0 };

		if (timespec_is_after(begin, now))
			remaining = timespec_rem(begin, now);
		rc = _arm_timer(remaining);
	} else {
		struct kevent change;

		EV_SET(&change, 1, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
		if ((kevent(timer_fd, &change, 1, NULL, 0, NULL) < 0) &&
		    (errno != ENOENT))
			rc = errno;
	}
#else
	struct itimerspec spec = { .it_value = begin };

	rc = timer_settime(timer, TIMER_ABSTIME, &spec, NULL);
#endif
	slurm_mutex_unlock(&mutex);

	if (rc) {
		if ((rc == -1) && errno)
			rc = errno;
		error("%s: setting delayed-work timer failed: %s", __func__,
		      slurm_strerror(rc));
	}
}

/* check begin times to see if the work delay has elapsed */
static int _inspect_work(void *x, void *key)
{
	work_t *work = x;
	const timespec_t begin = _deadline(work);
	foreach_delayed_work_t *args = key;
	const timespec_t now = _delay_now();
	const bool trigger = !timespec_is_after(begin, now);

	xassert(args->magic == MAGIC_FOREACH_DELAYED_WORK);
	xassert(work->magic == MAGIC_WORK);

	if (slurm_conf.debug_flags & DEBUG_FLAG_CONMGR) {
		const timespec_diff_ns_t diff = timespec_diff_ns(begin, now);
		char str[CTIME_STR_LEN];

		timespec_ctime(diff.diff, false, str, sizeof(str));

		log_flag(CONMGR, "%s: %s delayed work ETA %s for %s@0x%"PRIxPTR,
			 __func__, (trigger ? "triggering" : "deferring"),
			 str, work->callback.func_name,
			 (uintptr_t) work->callback.func);
	}

	/* list_delete_all releases triggered work before _update_timer runs. */
	if (trigger)
		return 1;

	if (!args->shortest)
		args->shortest = work;
	else if (timespec_is_after(_deadline(args->shortest), begin))
		args->shortest = work;

	return 0;
}

extern timespec_t conmgr_calc_work_time_delay(
	time_t delay_seconds,
	long delay_nanoseconds)
{
	/*
	 * Renormalize ns into seconds to only have partial seconds in
	 * nanoseconds. Nanoseconds won't matter with a larger number of
	 * seconds.
	 */

	return timespec_normalize(timespec_add((timespec_t) {
		.tv_sec = delay_seconds,
		.tv_nsec = delay_nanoseconds,
	}, timespec_now()));
}

#ifdef __APPLE__
static void _start_timer(void)
{
	int rc;

	if ((rc = pthread_once(&timer_once, _timer_register_fork)) ||
	    (rc = timer_init_error))
		fatal("%s: registering timer fork handlers failed: %s", __func__,
		      slurm_strerror(rc));
	pthread_mutex_lock(&mutex);
	if (timer_initialized) {
		pthread_mutex_unlock(&mutex);
		return;
	}
	/* kqueues are excluded from fork, so setting CLOEXEC has no fork race. */
	if (((timer_fd = kqueue()) < 0) ||
	    (fcntl(timer_fd, F_SETFD, FD_CLOEXEC) < 0))
		fatal("%s: creating timer kqueue failed: %m", __func__);
	{
		struct kevent change;

		EV_SET(&change, 2, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
		if (kevent(timer_fd, &change, 1, NULL, 0, NULL) < 0)
			fatal("%s: registering timer shutdown event failed: %m",
			      __func__);
	}
	timer_stop = timer_armed = false;
	rc = pthread_create(&timer_thread, NULL, _timer_wait, NULL);
	timer_running = timer_initialized = !rc;
	pthread_mutex_unlock(&mutex);
	if (rc)
		fatal("%s: creating timer thread failed: %s", __func__,
		      slurm_strerror(rc));
}
#endif

extern void init_delayed_work(void)
{
#ifndef __APPLE__
	int rc;
#endif

	mgr.delayed_work = list_create(_release_work);

#ifdef __APPLE__
	_start_timer();
#else
again:
	slurm_mutex_lock(&mutex);
	{
		struct sigevent sevp = {
			.sigev_notify = SIGEV_SIGNAL,
			.sigev_signo = SIGALRM,
			.sigev_value.sival_ptr = &timer,
		};

		rc = timer_create(TIMESPEC_CLOCK_TYPE, &sevp, &timer);
		timer_initialized = !rc;
	}
	slurm_mutex_unlock(&mutex);

	if (!rc)
		return;

	if ((rc == -1) && errno)
		rc = errno;

	if (rc == EAGAIN)
		goto again;
	else if (rc)
		fatal("%s: timer_create() failed: %s",
		      __func__, slurm_strerror(rc));
#endif
}

extern void stop_delayed_work(void)
{
	int rc;

	slurm_mutex_lock(&mutex);
	if (!timer_initialized) {
		slurm_mutex_unlock(&mutex);
		return;
	}
	timer_initialized = false;
#ifdef __APPLE__
	timer_stop = true;
	timer_armed = false;
	{
		struct kevent change;

		EV_SET(&change, 2, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
		if (kevent(timer_fd, &change, 1, NULL, 0, NULL) < 0)
			fatal("%s: waking timer for shutdown failed: %m", __func__);
	}
	slurm_mutex_unlock(&mutex);
	/* No signal may be emitted after this join returns. */
	if (timer_running && (rc = pthread_join(timer_thread, NULL)))
		fatal("%s: joining timer thread failed: %s", __func__,
		      slurm_strerror(rc));
	timer_running = false;
	close(timer_fd);
	timer_fd = -1;
#else
	rc = timer_delete(timer);
	slurm_mutex_unlock(&mutex);
	if (rc)
		fatal("%s: timer_delete() failed: %m", __func__);
#endif
}

extern void pause_delayed_work(void)
{
#ifdef __APPLE__
	/* Quiesce permits closing descriptors before an in-process exec. */
	stop_delayed_work();
#endif
}

extern void resume_delayed_work(void)
{
#ifdef __APPLE__
	/* Preserve queued work and its original monotonic deadlines. */
	_start_timer();
	_inspect();
#endif
}

extern void free_delayed_work(void)
{
	if (!mgr.delayed_work)
		return;

	stop_delayed_work();
	FREE_NULL_LIST(mgr.delayed_work);
}

static void _update_delayed_work(bool locked)
{
	if (!locked)
		slurm_mutex_lock(&mgr.mutex);

	_inspect();

	if (!locked)
		slurm_mutex_unlock(&mgr.mutex);
}

extern void on_signal_alarm(conmgr_callback_args_t conmgr_args, void *arg)
{
	if (conmgr_args.status == CONMGR_WORK_STATUS_CANCELLED)
		return;

	log_flag(CONMGR, "%s: caught SIGALRM", __func__);
	_update_delayed_work(false);
}

/*
 * Clear time delay dependency from work
 * IN work - work to remove CONMGR_WORK_DEP_TIME_DELAY flag
 * NOTE: caller must call update_timer() after to cause work to requeue
 * NOTE: caller must hold mgr.mutex lock
 * RET True if time delay removed
 */
static bool _work_clear_time_delay(work_t *work)
{
	xassert(work->magic == MAGIC_WORK);

	if (work->status != CONMGR_WORK_STATUS_PENDING)
		return false;

	if (!(work->control.depend_type & CONMGR_WORK_DEP_TIME_DELAY))
		return false;

#ifndef NDEBUG
	work->control.time_begin = (timespec_t) {0};
#endif /* !NDEBUG */
	work_mask_depend(work, ~CONMGR_WORK_DEP_TIME_DELAY);

	return true;
}

extern void add_work_delayed(work_t *work)
{
#ifdef __APPLE__
	const timespec_t now = timespec_now();
	const timespec_t begin = work->control.time_begin;
	timespec_t remaining = { 0 };

	/*
	 * Convert once: later wall-clock corrections must not lengthen or
	 * shorten an already queued delay. Keep the original time for logging.
	 */
	if (timespec_is_after(begin, now))
		remaining = timespec_rem(begin, now);
	work->time_deadline =
		timespec_normalize(timespec_add(_delay_now(), remaining));
#endif
	list_append(mgr.delayed_work, work);
	_update_delayed_work(true);
}

extern char *work_delayed_to_str(work_t *work)
{
	char *delay = NULL, str[CTIME_STR_LEN];

	if (!(work->control.depend_type & CONMGR_WORK_DEP_TIME_DELAY))
		return NULL;

	timespec_ctime(work->control.time_begin, true, str, sizeof(str));
	xstrfmtcat(delay, " time_begin=%s", str);

	return delay;
}
