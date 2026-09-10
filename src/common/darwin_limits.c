/*****************************************************************************\
 *  darwin_limits.c - native per-process resource controls.
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

#include "darwin_limits.h"

#include <errno.h>
#include <sys/resource.h>
#include <unistd.h>

#define MIB (UINT64_C(1024) * 1024)

extern int darwin_limits_validate(uint64_t cpu_seconds, uint64_t address_mib)
{
	if ((cpu_seconds >= (uint64_t) RLIM_INFINITY) ||
	    (address_mib > ((uint64_t) RLIM_INFINITY - 1) / MIB))
		return EOVERFLOW;
#ifndef RLIMIT_AS
	if (address_mib)
		return ENOTSUP;
#endif
	return 0;
}

static int _lower_limit(int resource, rlim_t value)
{
	struct rlimit limit;

	if (!value)
		return 0;
	if (getrlimit(resource, &limit))
		return errno;
	if (limit.rlim_max > value)
		limit.rlim_max = value;
	if (limit.rlim_cur > limit.rlim_max)
		limit.rlim_cur = limit.rlim_max;
	if (setrlimit(resource, &limit))
		return errno;
	return 0;
}

extern int darwin_limits_apply(uint64_t cpu_seconds, uint64_t address_mib)
{
	int rc = darwin_limits_validate(cpu_seconds, address_mib);

	if (rc)
		return rc;
	if ((rc = _lower_limit(RLIMIT_CPU, cpu_seconds)))
		return rc;
#ifdef RLIMIT_AS
	return _lower_limit(RLIMIT_AS, address_mib * MIB);
#else
	return 0;
#endif
}
