/*****************************************************************************\
 *  darwin_launch.c - native macOS launch/resource controls.
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

#include "darwin_launch.h"

#include <errno.h>
#include <limits.h>
#include <unistd.h>

#include "darwin_limits.h"
#include "darwin_sandbox.h"

static bool deny_gpu;
static bool early_applied;
static uint64_t epilog_cpu_seconds, epilog_address_mib;

#ifdef __APPLE__
#include <dlfcn.h>
#include <spawn.h>
#include <sys/sysctl.h>

/*
 * Private ABI from Apple XNU f6217f891ac0bb64f3d375211650a4c1ff8ca1ea,
 * libsyscall/wrappers/spawn/posix_spawn.c and bsd/sys/spawn_internal.h.
 * Capability failure is an error when explicitly requested, never a fallback.
 */
typedef int (*jetsam_fn_t)(posix_spawnattr_t *, short, int, int, int);
#define JETSAM_FATAL_ACTIVE 0x04
#define JETSAM_FATAL_INACTIVE 0x08

static uint64_t footprint_mib;

static int _attributes(posix_spawnattr_t *attr, uint64_t mib)
{
	jetsam_fn_t jetsam;
	int enabled = 0, rc;
	size_t size = sizeof(enabled);

	if (!mib || mib > INT32_MAX)
		return EINVAL;
	jetsam = (jetsam_fn_t) dlsym(RTLD_DEFAULT,
				     "posix_spawnattr_setjetsam_ext");
	if (!jetsam)
		return ENOTSUP;
	/* Some development kernels expose an explicit disable switch. */
	if (!sysctlbyname("kern.memorystatus_highwater_enabled", &enabled,
			  &size, NULL, 0)) {
		if (size != sizeof(enabled) || !enabled)
			return ENOTSUP;
	} else if (errno != ENOENT) {
		return errno;
	}
	if ((rc = posix_spawnattr_init(attr)))
		return rc;
	if (!(rc = posix_spawnattr_setflags(attr, POSIX_SPAWN_SETEXEC)))
		/* -1 selects the kernel's default jetsam priority. */
		rc = jetsam(attr, JETSAM_FATAL_ACTIVE | JETSAM_FATAL_INACTIVE,
			    -1, (int) mib, (int) mib);
	if (rc)
		posix_spawnattr_destroy(attr);
	return rc;
}
#endif

extern int darwin_launch_probe(uint64_t initial_image_mib)
{
	if (!initial_image_mib)
		return 0;
	if (initial_image_mib > INT32_MAX)
		return EOVERFLOW;
#ifdef __APPLE__
	posix_spawnattr_t attr;
	int rc = _attributes(&attr, initial_image_mib);

	if (!rc)
		posix_spawnattr_destroy(&attr);
	return rc;
#else
	return ENOTSUP;
#endif
}

extern int darwin_launch_prepare(uint64_t initial_image_mib)
{
	int rc = darwin_launch_probe(initial_image_mib);

#ifdef __APPLE__
	if (!rc)
		footprint_mib = initial_image_mib;
#endif
	return rc;
}

extern bool darwin_launch_configured(void)
{
#ifdef __APPLE__
	return footprint_mib != 0;
#else
	return false;
#endif
}

extern void darwin_launch_prepare_gpu(bool deny_fresh_connections)
{
	deny_gpu = deny_fresh_connections;
	early_applied = false;
}

extern bool darwin_launch_gpu_denied(void)
{
	return deny_gpu;
}

extern int darwin_launch_apply_early(void)
{
	int rc;

	if (!deny_gpu || early_applied)
		return 0;
	rc = darwin_sandbox_apply(DARWIN_SANDBOX_DENY_GPU_OPEN);
	if (!rc)
		early_applied = true;
	return rc;
}

extern void darwin_launch_prepare_limits(uint64_t cpu_seconds,
					 uint64_t address_mib)
{
	epilog_cpu_seconds = cpu_seconds;
	epilog_address_mib = address_mib;
}

extern int darwin_launch_apply_epilog(void)
{
	int rc = darwin_launch_apply_early();

	if (rc)
		return rc;
	return darwin_limits_apply(epilog_cpu_seconds, epilog_address_mib);
}

extern int darwin_launch_exec(const char *path, char *const argv[],
			      char *const env[])
{
	if (!path || !argv || !argv[0] || !env)
		return EINVAL;
#ifdef __APPLE__
	if (footprint_mib) {
		posix_spawnattr_t attr;
		pid_t unexpected_child = 0;
		int rc = _attributes(&attr, footprint_mib);

		if (rc)
			return rc;
		rc = posix_spawn(&unexpected_child, path, NULL, &attr, argv,
				 env);
		posix_spawnattr_destroy(&attr);
		/* SETEXEC never returns on success. */
		return rc ? rc : EIO;
	}
#endif
	execve(path, argv, env);
	return errno;
}
