/*****************************************************************************\
 *  signal.c - signals for connection manager
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

#include <signal.h>

#include "src/common/fd.h"
#include "src/common/macros.h"
#include "src/common/proc_args.h"
#include "src/common/read_config.h"
#include "src/common/xmalloc.h"

#include "src/conmgr/conmgr.h"
#include "src/conmgr/delayed.h"
#include "src/conmgr/mgr.h"

#define SIGNAL_FD_FAILED -250

typedef struct {
#define MAGIC_SIGNAL_HANDLER 0xC20A444A
	int magic; /* MAGIC_SIGNAL_HANDLER */
	struct sigaction prior;
	struct sigaction new;
	int signal;
} signal_handler_t;

/* protects all of the static variables here */
pthread_rwlock_t lock = PTHREAD_RWLOCK_INITIALIZER;

/* protected by lock */
static bool one_time_init = false;

/* list of all registered signal handlers */
static signal_handler_t *signal_handlers = NULL;
static int signal_handler_count = 0;
/* Never log or acquire lock while holding this fork-snapshot mutex. */
static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;

/* list of all registered signal work */
static work_t **signal_work = NULL;
static int signal_work_count = 0;

/* interrupt handler (_signal_handler()) will send signal to this fd */
static volatile sig_atomic_t signal_fd = -1;
static conmgr_fd_t *signal_con = NULL;

static void _signal_handler(int signo)
{
	/*
	 * Per the sigaction man page:
	 * 	A child created via fork(2) inherits a copy of its parent's
	 * 	signal dispositions.
	 *
	 * Signal handler registration survives fork() but the signal_mgr()
	 * thread will be lost. Gracefully ignore signals when signal_fd_send is
	 * -1 to avoid trying to write a non-existent file descriptor.
	 */
	if (signal_fd < 0)
		return;

try_again:
	if (write(signal_fd, &signo, sizeof(signo)) != sizeof(signo)) {
		if ((errno == EPIPE) || (errno == EBADF)) {
			/*
			 * write() after conmgr shutdown before reading that
			 * signal_fd was closed. Ignoring this race condition
			 * entirely. Set signal_fd to an invalid value that is
			 * not -1 to distinguish it from the normal "unset"
			 * state.
			 */
			signal_fd = SIGNAL_FD_FAILED;
			return;
		}

		if (errno == EINTR)
			goto try_again;

		/*
		 * Drop signal as the buffer is already full which means
		 * something bad has already happened and having the exact
		 * signal numbers isn't going to make much difference.
		 */
		if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
			return;

		/* TODO: replace with signal_safe_fatal() */
		fatal_abort("%s: unable to signal connection manager: %m",
			    __func__);
	}
}

/* Unlike a signal handler, a timer thread may lock against fd teardown. */
extern int signal_mgr_notify(int signo)
{
	int rc = EAGAIN;

	slurm_rwlock_rdlock(&lock);
	if (signal_fd >= 0) {
		ssize_t bytes;

		do {
			bytes = write(signal_fd, &signo, sizeof(signo));
		} while ((bytes < 0) && (errno == EINTR));
		rc = (bytes == sizeof(signo)) ? 0 : ((bytes < 0) ? errno : EIO);
	}
	slurm_rwlock_unlock(&lock);
	return rc;
}

/* caller must hold write lock */
static void _register_signal_handler(int signal)
{
	signal_handler_t *handler;

	for (int i = 0; i < signal_handler_count; i++) {
		xassert(signal_handlers[i].magic == MAGIC_SIGNAL_HANDLER);

		if (signal_handlers[i].signal == signal)
			return;
	}

	pthread_mutex_lock(&registry_mutex);
	xrecalloc(signal_handlers, (signal_handler_count + 1),
		  sizeof(*signal_handlers));

	handler = &signal_handlers[signal_handler_count];
	handler->magic = MAGIC_SIGNAL_HANDLER;
	handler->signal = signal;
	handler->new.sa_handler = _signal_handler;

	if (sigaction(signal, &handler->new, &handler->prior)) {
		pthread_mutex_unlock(&registry_mutex);
		fatal("%s: unable to catch %s: %m",
		      __func__, strsignal(signal));
	}
	signal_handler_count++;
	pthread_mutex_unlock(&registry_mutex);

	if (slurm_conf.debug_flags & DEBUG_FLAG_CONMGR) {
		char *signame = sig_num2name(handler->signal);

		log_flag(CONMGR, "%s: installed signal %s[%d] handler: Prior=0x%"PRIxPTR" is now replaced with New=0x%"PRIxPTR,
			 __func__, signame, signal,
			 (uintptr_t) handler->prior.sa_handler,
			 (uintptr_t) handler->new.sa_handler);
		xfree(signame);
	}
}

/* Caller holds lock for writing; fork must see the matching dispositions. */
static void _reset_all_signal_handlers(void)
{
	signal_handler_t *handlers;
	int failed_signal = 0, rc = 0;

	pthread_mutex_lock(&registry_mutex);
	for (int i = 0; i < signal_handler_count; i++) {
		signal_handler_t *handler = &signal_handlers[i];

		if (sigaction(handler->signal, &handler->prior, NULL)) {
			rc = errno;
			failed_signal = handler->signal;
			break;
		}
	}
	handlers = signal_handlers;
	signal_handlers = NULL;
	signal_handler_count = 0;
	pthread_mutex_unlock(&registry_mutex);
	if (rc)
		fatal("%s: unable to revert %s: %s", __func__,
		      strsignal(failed_signal), slurm_strerror(rc));
	xfree(handlers);
}

/* caller must hold write lock */
static void _init_signal_handler(void)
{
	if (signal_handlers)
		return;

	for (int i = 0; i < signal_work_count; i++) {
		work_t *work = signal_work[i];
		xassert(work->magic == MAGIC_WORK);

		_register_signal_handler(work->control.on_signal_number);
	}
}

/* mgr.mutex should be locked */
static void _on_signal(int signal)
{
	bool matched = false;

	slurm_rwlock_rdlock(&lock);

	if (slurm_conf.debug_flags & DEBUG_FLAG_CONMGR) {
		char *str = sig_num2name(signal);
		log_flag(CONMGR, "%s: [%s] got signal: %s(%d)",
			 __func__, (signal_con ? signal_con->name : "INVALID"),
			 str, signal);
		xfree(str);
	}

	for (int i = 0; i < signal_work_count; i++) {
		work_t *work = signal_work[i];

		xassert(work->magic == MAGIC_WORK);

		if (work->control.on_signal_number != signal)
			continue;

		matched = true;
		add_work(true, NULL, work->callback, work->control,
			 ~CONMGR_WORK_DEP_SIGNAL, __func__);
	}

	slurm_rwlock_unlock(&lock);

	if (!matched)
		warning("%s: caught and ignoring signal %s",
			__func__, strsignal(signal));
}

extern void add_work_signal(work_t *work)
{
	xassert(!work->ref);
	xassert(work->control.depend_type & CONMGR_WORK_DEP_SIGNAL);
	xassert(work->control.on_signal_number > 0);

	slurm_rwlock_wrlock(&lock);

	xrecalloc(signal_work, (signal_work_count + 1), sizeof(*signal_work));

	signal_work[signal_work_count] = work;
	signal_work_count++;

	/*
	 * Directly register new signal handler since connection already started
	 * and init_signal_handler() already ran
	 */
	if (signal_con)
		_register_signal_handler(work->control.on_signal_number);

	slurm_rwlock_unlock(&lock);
}

static void *_on_connection(conmgr_callback_args_t conmgr_args, void *arg)
{
	conmgr_fd_t *con = conmgr_args.con;

	slurm_rwlock_wrlock(&lock);

	_init_signal_handler();
	signal_con = con;

	log_flag(CONMGR, "%s: [%s] registering signal pipe",
			 __func__, conmgr_con_get_name(conmgr_args.ref));

	slurm_rwlock_unlock(&lock);

	return con;
}

static int _on_data(conmgr_callback_args_t conmgr_args, void *arg)
{
	const void *data = NULL;
	conmgr_fd_t *con = conmgr_args.con;
	size_t bytes = 0, read = 0;
	int signo;

	xassert(arg == con);

	conmgr_fd_get_in_buffer(con, &data, &bytes);

	slurm_mutex_lock(&mgr.mutex);
	while ((read + sizeof(signo)) <= bytes) {
		signo = *(int *) (data + read);

		_on_signal(signo);

		read += sizeof(signo);
	}
	slurm_mutex_unlock(&mgr.mutex);

	conmgr_fd_mark_consumed_in_buffer(con, read);

	return SLURM_SUCCESS;
}

static void _on_finish(conmgr_callback_args_t conmgr_args, void *arg)
{
	work_t **cancel_work = NULL;
	int cancel_work_count = 0;
	int fd = SIGNAL_FD_FAILED;

	xassert(arg == conmgr_args.con);

	stop_delayed_work();
	slurm_rwlock_wrlock(&lock);

	SWAP(fd, signal_fd);

	xassert(fd != -1);
	fd_close(&fd);

	xassert(conmgr_args.con == signal_con);
	signal_con = NULL;

	/*
	 * Detach pending signal work under the lock so cancellation can run
	 * outside. handle_work() takes mgr.mutex, and the rest of this file
	 * uses mgr.mutex -> signals.lock; calling handle_work() under
	 * signals.lock would invert the order against the watch thread.
	 */
	SWAP(signal_work_count, cancel_work_count);
	SWAP(signal_work, cancel_work);

	_reset_all_signal_handlers();

	log_flag(CONMGR, "%s: [%s] closed signal pipe",
			 __func__, conmgr_con_get_name(conmgr_args.ref));

	slurm_rwlock_unlock(&lock);

	for (int i = 0; i < cancel_work_count; i++) {
		work_t *work = cancel_work[i];

		xassert(work->magic == MAGIC_WORK);

		work->status = CONMGR_WORK_STATUS_CANCELLED;
		handle_work(false, work);
	}

	xfree(cancel_work);
}

static void _atfork_prepare(void)
{
	/*
	 * Avoid lock: its owners may log, and logging has its own fork handler.
	 * The registry mutex protects only array mutation and never logs.
	 */
	pthread_mutex_lock(&registry_mutex);
}

static void _atfork_parent(void)
{
	pthread_mutex_unlock(&registry_mutex);
}

static void _atfork_child(void)
{
	/*
	 * The prepare handler makes this snapshot consistent. Restore signal
	 * dispositions without logging or freeing inherited heap allocations:
	 * only async-signal-safe operations are available after fork(). The
	 * vanished worker threads cannot release the inherited rwlock, so reset
	 * it along with the unusable connection-manager state.
	 */
	for (int i = 0; i < signal_handler_count; i++) {
		if (sigaction(signal_handlers[i].signal,
			      &signal_handlers[i].prior, NULL))
			_exit(127);
	}

	lock = (pthread_rwlock_t) PTHREAD_RWLOCK_INITIALIZER;
	registry_mutex = (pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER;
	one_time_init = false;
	signal_handlers = NULL;
	signal_handler_count = 0;
	/* coverity[missing_lock] */
	signal_work = NULL;
	signal_work_count = 0;
	signal_fd = -1;
	signal_con = NULL;
}

extern void signal_mgr_start(conmgr_callback_args_t conmgr_args, void *arg)
{
	static const conmgr_events_t events = {
		.on_connection = _on_connection,
		.on_data = _on_data,
		.on_finish = _on_finish,
	};
	int fd[2] = { -1, -1 };
	int rc;

	if (conmgr_args.status == CONMGR_WORK_STATUS_CANCELLED)
		return;

	slurm_rwlock_wrlock(&lock);

	if (signal_fd != -1) {
		slurm_rwlock_unlock(&lock);
		log_flag(CONMGR, "%s: skipping - already initialized",
			 __func__);
		return;
	}

	if (slurm_pipe(fd, O_CLOEXEC))
		fatal_abort("%s: pipe() failed: %m", __func__);

	if (!one_time_init) {
		if ((rc = pthread_atfork(_atfork_prepare, _atfork_parent,
					 _atfork_child)))
			fatal_abort("%s: pthread_atfork() failed: %s",
				    __func__, slurm_strerror(rc));
		one_time_init = true;
	}

	xassert(signal_fd == -1);
	xassert(!signal_con);

	fd_set_nonblocking(fd[1]);
	signal_fd = fd[1];

	slurm_rwlock_unlock(&lock);

	if (add_connection(CON_TYPE_RAW, &conmgr_timeouts_disabled, NULL, fd[0],
			   -1, &events, CON_FLAG_NONE, NULL, 0, false, NULL,
			   NULL, NULL, NULL)) {
		fatal_abort("%s: [fd:%d] unable to a register new connection",
			    __func__, fd[0]);
	}
}

extern void signal_mgr_stop(void)
{
	int fd = -1;

	stop_delayed_work();
	slurm_rwlock_wrlock(&lock);

	if (signal_con)
		close_con(true, signal_con);

	fd = signal_fd;
	signal_fd = SIGNAL_FD_FAILED;

	slurm_rwlock_unlock(&lock);

	(void) close(fd);
}

extern void signal_mgr_fini(void)
{
	work_t **cancel_work = NULL;
	int cancel_work_count = 0;
	int fd = SIGNAL_FD_FAILED;

	signal_mgr_stop();

	slurm_rwlock_wrlock(&lock);

	if (!one_time_init) {
		slurm_rwlock_unlock(&lock);
		return;
	}

	/*
	 * Fallback in case _on_finish() did not run before fini (e.g., the
	 * conmgr watch loop already exited before processing close_con).
	 * The cleanup is idempotent: if _on_finish() ran first, the SWAPs
	 * just produce NULL/0 locals and the loops below run zero times.
	 */

	SWAP(fd, signal_fd);
	if (fd != SIGNAL_FD_FAILED)
		fd_close(&fd);

	signal_con = NULL;

	SWAP(signal_work_count, cancel_work_count);
	SWAP(signal_work, cancel_work);

	_reset_all_signal_handlers();

	slurm_rwlock_unlock(&lock);

	for (int i = 0; i < cancel_work_count; i++) {
		work_t *work = cancel_work[i];

		xassert(work->magic == MAGIC_WORK);

		work->status = CONMGR_WORK_STATUS_CANCELLED;
		handle_work(true, work);
	}

	xfree(cancel_work);
}

extern bool is_signal_connection(conmgr_fd_t *con)
{
	bool match;

	slurm_rwlock_rdlock(&lock);
	match = (signal_con == con);
	slurm_rwlock_unlock(&lock);

	return match;
}

extern bool signal_mgr_has_incoming(void)
{
	bool has_data = false;

	slurm_rwlock_rdlock(&lock);

	if (signal_con) {
		if (signal_con->input_fd >= 0) {
			int readable = -1;

			/*
			 * Force (blocking) check of signal_fd since poll() may
			 * have not run yet but need to make sure to catch any
			 * pending data if possible.
			 *
			 * Ignore failure of FIONREAD here as we only care if
			 * there is pending data
			 */
			(void) fd_get_readable_bytes(signal_con->input_fd,
						     &readable,
						     signal_con->name);

			if (readable > 0)
				has_data = true;
		}

		if (!has_data)
			has_data =
				con_flag(signal_con, FLAG_CAN_READ) ||
				(signal_con->in &&
				 get_buf_offset(signal_con->in)) ||
				(signal_con->work &&
				 !list_is_empty(signal_con->work)) ||
				(signal_con->write_complete_work &&
				 !list_is_empty(signal_con->write_complete_work));
	}

	slurm_rwlock_unlock(&lock);

	return has_data;
}
