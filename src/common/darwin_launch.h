/*****************************************************************************\
 *  darwin_launch.h - native macOS launch/resource controls.
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

#ifndef _DARWIN_LAUNCH_H
#define _DARWIN_LAUNCH_H

#include <stdbool.h>
#include <stdint.h>

/* Internal task-child state; never populated from a workload environment. */
/* All helpers return zero or an errno value. Zero MiB disables the policy. */
extern int darwin_launch_probe(uint64_t initial_image_mib);
extern int darwin_launch_prepare(uint64_t initial_image_mib);
extern bool darwin_launch_configured(void);
/* Store validated per-process ceilings in the parent, without applying them. */
extern void darwin_launch_prepare_limits(uint64_t cpu_seconds,
					 uint64_t address_mib);
/* Epilog child only, before exec; does not prepare an initial-image limit. */
extern int darwin_launch_apply_epilog(void);
/*
 * Replaces this process, preserving PID and the existing stepd wait contract.
 * Only returns an errno on failure. The footprint policy covers this image:
 * ordinary exec and fork can reset it. It is not a process-tree RAM budget.
 */
extern int darwin_launch_exec(const char *path, char *const argv[],
			      char *const env[]);

#endif
