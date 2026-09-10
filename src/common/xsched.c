/*****************************************************************************\
 *  xsched.c - kernel cpu affinity handlers
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
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "src/common/slurm_protocol_api.h"
#include "src/common/xmalloc.h"
#include "src/common/xsched.h"

#ifdef __APPLE__
#define CPU_WORD_BITS (sizeof(unsigned long) * CHAR_BIT)

extern int xcpuset_count(const xcpuset_t *mask)
{
	int count = 0;

	for (size_t i = 0; i < mask->size / sizeof(unsigned long); i++)
		count += __builtin_popcountl(mask->mask[i]);
	return count;
}

extern void xcpuset_zero(xcpuset_t *mask)
{
	memset(mask->mask, 0, mask->size);
}

extern void xcpuset_set(size_t cpu, xcpuset_t *mask)
{
	if (cpu < mask->max_cpus)
		mask->mask[cpu / CPU_WORD_BITS] |= 1UL << (cpu % CPU_WORD_BITS);
}

extern void xcpuset_clr(size_t cpu, xcpuset_t *mask)
{
	if (cpu < mask->max_cpus)
		mask->mask[cpu / CPU_WORD_BITS] &=
			~(1UL << (cpu % CPU_WORD_BITS));
}

extern int xcpuset_isset(size_t cpu, const xcpuset_t *mask)
{
	return ((cpu < mask->max_cpus) && (mask->mask[cpu / CPU_WORD_BITS] &
					   (1UL << (cpu % CPU_WORD_BITS))));
}
#endif

extern xcpuset_t *xcpuset_alloc(void)
{
#ifdef __APPLE__
	long cpus = sysconf(_SC_NPROCESSORS_CONF);
	size_t max_cpus = (cpus > 1024) ? cpus : 1024;
	size_t words = (max_cpus + CPU_WORD_BITS - 1) / CPU_WORD_BITS;
	xcpuset_t *new = xmalloc(sizeof(*new) + words * sizeof(unsigned long));

	new->max_cpus = words *CPU_WORD_BITS;
	new->size = words * sizeof(unsigned long);
#else
	xcpuset_t *new = xgetaffinity(0);
#endif
	XCPU_ZERO(new);
	return new;
}

extern char *task_cpuset_to_str(const xcpuset_t *mask)
{
	int base;
	bool leading_zeros = true;
	char *str = xmalloc((mask->max_cpus / 4) + 1);
	char *ptr = str;

	for (base = mask->max_cpus - 4; base >= 0; base -= 4) {
		char val = 0;
		if (XCPU_ISSET(base, mask))
			val |= 1;
		if (XCPU_ISSET(base + 1, mask))
			val |= 2;
		if (XCPU_ISSET(base + 2, mask))
			val |= 4;
		if (XCPU_ISSET(base + 3, mask))
			val |= 8;
		/* If it's a leading zero, ignore it */
		if (leading_zeros && !val)
			continue;
		*ptr++ = slurm_hex_to_char(val);
		/* All zeros from here on out will be written */
		leading_zeros = false;
	}
	/* If the bitmask is all 0s, add a single 0 */
	if (leading_zeros)
		*ptr++ = '0';
	return str;
}

extern xcpuset_t *task_str_to_cpuset(const char *str)
{
	xcpuset_t *mask = NULL;
	size_t len;
	size_t base = 0;

	if (!str || !str[0])
		return NULL;
	len = strlen(str);

	/* skip 0x, it's all hex anyway */
	if ((len > 1) && !memcmp(str, "0x", 2L)) {
		str += 2;
		len -= 2;
	}
	if (!len)
		return NULL;

	mask = xcpuset_alloc();

	/* Check that hex chars will fit into the xcpuset_t */
	if (len > (mask->max_cpus / 4)) {
		error("%s: Hex string is too large to convert to CPU mask (length %zu, max_cpus %zu)",
		      __func__, len, mask->max_cpus);
		xfree(mask);
		return NULL;
	}

	for (size_t i = len; i > 0; i--) {
		char val = slurm_char_to_hex(str[i - 1]);
		if (val == (char) -1) {
			xfree(mask);
			break;
		}
		if (val & 1)
			XCPU_SET(base, mask);
		if (val & 2)
			XCPU_SET(base + 1, mask);
		if (val & 4)
			XCPU_SET(base + 2, mask);
		if (val & 8)
			XCPU_SET(base + 3, mask);
		base += 4;
	}

	return mask;
}

extern int xsetaffinity(pid_t pid, xcpuset_t *mask)
{
	int rval;

#if defined(__APPLE__)
	/* THREAD_AFFINITY_POLICY is a cache-sharing hint, not CPU binding. */
	errno = ENOTSUP;
	rval = -1;
#elif defined(__FreeBSD__)
	rval = cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, pid,
				  mask->size, &mask->mask);
#else
	rval = sched_setaffinity(pid, mask->size, &mask->mask);
#endif
	if (rval) {
		int save_errno = errno;
		char *mstr = task_cpuset_to_str(mask);
		verbose("sched_setaffinity(%d,%zu,0x%s) failed: %m",
			pid, mask->size, mstr);
		xfree(mstr);
		errno = save_errno;
	}
	return rval;
}

#ifndef __APPLE__
static int _getaffinity(pid_t pid, xcpuset_t *mask)
{
	errno = 0;
	/*
	 * The FreeBSD cpuset API is a superset of the Linux API.
	 * In addition to PIDs, it supports threads, interrupts,
	 * jails, and potentially other objects.  The first two arguments
	 * to cpuset_*etaffinity() below indicate that the third argument
	 * is a PID.  -1 indicates the PID of the calling process.
	 * Linux sched_*etaffinity() uses 0 for this.
	 */
#ifdef __FreeBSD__
	return cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, pid,
				  mask->size, &mask->mask);
#else
	return sched_getaffinity(pid, mask->size, &mask->mask);
#endif
}
#endif

extern xcpuset_t *xgetaffinity(pid_t pid)
{
#ifdef __APPLE__
	errno = ENOTSUP;
	return NULL;
#else
	int rval;
	static size_t max_cpus = CPU_SETSIZE;
	xcpuset_t *mask = NULL;

	while (true) {
		xrealloc(mask, (2 * sizeof(size_t)) + CPU_ALLOC_SIZE(max_cpus));
		mask->max_cpus = max_cpus;
		mask->size = CPU_ALLOC_SIZE(max_cpus);

		rval = _getaffinity(pid, mask);

		if ((rval < 0) && (errno == EINVAL)) {
			max_cpus *= 2;
			continue;
		}

		break;
	}

	if (rval) {
		verbose("sched_getaffinity(%d,%zu) failed with status %d",
			pid, mask->size, rval);
	} else if (get_log_level() >= LOG_LEVEL_DEBUG3) {
		char *mstr = task_cpuset_to_str(mask);
		debug3("sched_getaffinity(%d) = 0x%s", pid, mstr);
		xfree(mstr);
	}

	return mask;
#endif
}

extern int get_assigned_cpu_count(void)
{
#ifdef __APPLE__
	long count = sysconf(_SC_NPROCESSORS_ONLN);

	return ((count > 0) && (count <= INT_MAX)) ? count : 0;
#else
	int count = -1;
	xcpuset_t *mask = xgetaffinity(0);

	count = XCPU_COUNT(mask);

	xfree(mask);
	return count;
#endif
}
