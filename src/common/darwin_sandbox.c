/*****************************************************************************\
 *  darwin_sandbox.c - native child-process access restrictions.
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
#include <stddef.h>

#include "src/common/darwin_sandbox.h"

#if defined(__APPLE__) && defined(HAVE_SANDBOX_INIT)
#include <sandbox.h>
#endif

extern int darwin_sandbox_apply(uint32_t flags)
{
	const uint32_t valid = DARWIN_SANDBOX_DENY_GPU_OPEN;

	if (flags & ~valid)
		return EINVAL;
	if (!flags)
		return 0;
#if defined(__APPLE__) && defined(HAVE_SANDBOX_INIT)
	/* The profile is fixed, never user-provided SBPL. */
	const char profile[] =
		"(version 1)(allow default)"
		"(deny iokit-open (iokit-connection \"IOAccelerator\"))";
	char *message = NULL;
	int rc, saved_errno;

	/*
	 * Custom SBPL is a deprecated/private compatibility interface. Callers
	 * must qualify its behavior and fail an explicitly requested policy if
	 * it is unavailable; compilation alone is not driver qualification.
	 */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	errno = 0;
	rc = sandbox_init(profile, 0, &message);
	saved_errno = errno;
	sandbox_free_error(message);
#pragma clang diagnostic pop
	return rc ? (saved_errno ? saved_errno : ENOTSUP) : 0;
#else
	return ENOTSUP;
#endif
}
