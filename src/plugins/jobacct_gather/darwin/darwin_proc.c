/*****************************************************************************\
 *  darwin_proc.c - native process accounting samples.
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

#include <errno.h>
#include <libproc.h>
#include <mach/mach_time.h>
#include <string.h>
#include <sys/proc_info.h>
#include <sys/resource.h>

#include "darwin_proc.h"

extern int darwin_proc_sample(pid_t pid, darwin_proc_sample_t *sample)
{
	struct proc_taskallinfo task;
	struct proc_bsdinfo identity;
	struct rusage_info_v2 usage;
	mach_timebase_info_data_t timebase;
	__uint128_t user_ns, system_ns;
	int size;

	if ((pid <= 0) || !sample)
		return EINVAL;
	memset(&task, 0, sizeof(task));
	memset(&usage, 0, sizeof(usage));
	memset(&identity, 0, sizeof(identity));
	errno = 0;
	size = proc_pidinfo(pid, PROC_PIDTASKALLINFO, 0, &task, sizeof(task));
	if (size != sizeof(task))
		return errno ? errno : ESRCH;
	if (proc_pid_rusage(pid, RUSAGE_INFO_V2, (rusage_info_t *) &usage))
		return errno ? errno : ESRCH;
	/* Reject a PID which was recycled between the two sampling APIs. */
	errno = 0;
	size = proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &identity,
			    sizeof(identity));
	if (size != sizeof(identity))
		return errno ? errno : ESRCH;
	if ((task.pbsd.pbi_pid != identity.pbi_pid) ||
	    (task.pbsd.pbi_start_tvsec != identity.pbi_start_tvsec) ||
	    (task.pbsd.pbi_start_tvusec != identity.pbi_start_tvusec))
		return ESRCH;
	/* rusage_info CPU times use Mach absolute ticks, including on arm64. */
	if ((mach_timebase_info(&timebase) != KERN_SUCCESS) || !timebase.denom)
		return EIO;
	user_ns = (__uint128_t) usage.ri_user_time * timebase.numer /
		  timebase.denom;
	system_ns = (__uint128_t) usage.ri_system_time * timebase.numer /
		    timebase.denom;
	if ((user_ns > UINT64_MAX) || (system_ns > UINT64_MAX))
		return EOVERFLOW;

	*sample = (darwin_proc_sample_t) {
		.pid = pid,
		.ppid = identity.pbi_ppid,
		.start_sec = identity.pbi_start_tvsec,
		.start_usec = identity.pbi_start_tvusec,
		.start_abstime = usage.ri_proc_start_abstime,
		.user_ns = user_ns,
		.system_ns = system_ns,
		.rss = usage.ri_resident_size,
		.vsize = task.ptinfo.pti_virtual_size,
		.footprint = usage.ri_phys_footprint,
		.read_bytes = usage.ri_diskio_bytesread,
		.write_bytes = usage.ri_diskio_byteswritten,
	};
	return 0;
}
