/*****************************************************************************\
 *  privileges.c
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

#define _GNU_SOURCE

#include <grp.h>
#include <pwd.h>
#include <sys/types.h>

#include "slurm/slurm_errno.h"

#include "src/common/log.h"
#include "src/common/strlcpy.h"
#include "src/common/xmalloc.h"
#include "src/interfaces/auth.h"
#include "src/slurmd/common/privileges.h"
#include "src/slurmd/slurmstepd/slurmstepd_job.h"

/*
 * If get_list is false, gid_list must already be initialized. The historical
 * flag permits releasing a previous snapshot; every successful drop now keeps
 * a complete snapshot because callers may need to reclaim the group list.
 * Success holds the auth setuid lock until reclaim_privileges(). Failure
 * restores the snapshot and releases the lock before returning.
 */
extern int drop_privileges(stepd_step_rec_t *step, bool do_setuid,
			   struct priv_state *ps, bool get_list)
{
	int saved_errno;

	auth_setuid_lock();
	ps->saved_uid = getuid();
	ps->saved_gid = getegid();
	ps->groups_changed = false;
	if (!get_list)
		xfree(ps->gid_list);
	ps->gid_list = NULL;

	ps->ngids = getgroups(0, NULL);
	if (ps->ngids == -1) {
		error("%s: getgroups(): %m", __func__);
		goto fail;
	}
	ps->gid_list = xcalloc(ps->ngids, sizeof(gid_t));
	if (ps->ngids && getgroups(ps->ngids, ps->gid_list) < 0) {
		error("%s: couldn't get %d groups: %m", __func__, ps->ngids);
		goto fail;
	}

	/* No need to drop privileges if we're not running as root. */
	if (getuid())
		return SLURM_SUCCESS;
	if (setegid(step->gid) < 0) {
		error("setegid: %m");
		goto fail;
	}
	ps->groups_changed = true;
	if (setgroups(step->ngids, step->gids) < 0) {
		error("setgroups: %m");
		goto fail;
	}
	if (do_setuid && seteuid(step->uid) < 0) {
		error("seteuid: %m");
		goto fail;
	}
	return SLURM_SUCCESS;
fail:
	saved_errno = errno;
	if (reclaim_privileges(ps) != SLURM_SUCCESS)
		error("%s: could not restore privileges after failed drop", __func__);
	errno = saved_errno;
	return SLURM_ERROR;
}

extern int reclaim_privileges(struct priv_state *ps)
{
	int rc = SLURM_SUCCESS;

	if ((geteuid() != ps->saved_uid) && seteuid(ps->saved_uid) < 0) {
		error("seteuid: %m");
		rc = SLURM_ERROR;
	} else if (ps->groups_changed) {
		/* GIDs may have changed even when do_setuid was false. */
		if (setegid(ps->saved_gid) < 0) {
			error("setegid: %m");
			rc = SLURM_ERROR;
		}
		if (setgroups(ps->ngids, ps->gid_list) < 0) {
			error("setgroups: %m");
			rc = SLURM_ERROR;
		}
	}
	auth_setuid_unlock();
	xfree(ps->gid_list);
	ps->groups_changed = false;
	return rc;
}
