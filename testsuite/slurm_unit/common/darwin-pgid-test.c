/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <errno.h>
#include <libproc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/proc.h>
#include <unistd.h>

enum {
	EMPTY,
	FILTERED,
	MEMBER,
	FAILED,
	NATIVE
};

static int scenario;
static const pid_t test_pgid = 12345;

static int _listpids(uint32_t type, uint32_t typeinfo, void *buffer, int size)
{
	pid_t *pids = buffer;

	if (scenario == NATIVE)
		return proc_listpids(type, typeinfo, buffer, size);
	if (!buffer)
		return 2 * sizeof(*pids);
	if (scenario == FAILED) {
		errno = EIO;
		return -1;
	}
	if (scenario == EMPTY)
		return 0;
	if (size < 2 * sizeof(*pids))
		return -1;
	pids[0] = 101;
	pids[1] = 102;
	return 2 * sizeof(*pids);
}

static int _pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int size)
{
	struct proc_bsdinfo *info = buffer;

	if (scenario == NATIVE)
		return proc_pidinfo(pid, flavor, arg, buffer, size);
	if (size != sizeof(*info))
		return -1;
	memset(info, 0, sizeof(*info));
	info->pbi_pgid = (scenario == MEMBER) ? test_pgid : test_pgid + 1;
	info->pbi_status = (pid == 101) ? SRUN : SZOMB;
	return sizeof(*info);
}

#define proc_listpids _listpids
#define proc_pidinfo _pidinfo
#include "src/plugins/proctrack/pgid/proctrack_pgid.c"
#undef proc_pidinfo
#undef proc_listpids

#define REQUIRE(condition) \
	do { \
		if (!(condition)) { \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, \
				#condition); \
			return 1; \
		} \
	} while (0)

int main(void)
{
	pid_t *pids = NULL;
	int count, rc, saved_errno;
	bool found = false;

	for (scenario = EMPTY; scenario <= FILTERED; scenario++) {
		REQUIRE(proctrack_p_get_pids(test_pgid, &pids, &count) == 0);
		REQUIRE(count == 0);
		REQUIRE(pids == NULL);
	}
	scenario = MEMBER;
	REQUIRE(proctrack_p_get_pids(test_pgid, &pids, &count) == 0);
	REQUIRE(count == 1);
	REQUIRE(pids[0] == 101);
	xfree(pids);
	scenario = FAILED;
	rc = proctrack_p_get_pids(test_pgid, &pids, &count);
	saved_errno = errno;
	REQUIRE(rc == SLURM_ERROR);
	REQUIRE(saved_errno == EIO);
	REQUIRE(count == 0);
	REQUIRE(pids == NULL);
	scenario = NATIVE;
	REQUIRE(proctrack_p_get_pids(getpgid(0), &pids, &count) == 0);
	for (int i = 0; i < count; i++)
		found |= (pids[i] == getpid());
	xfree(pids);
	REQUIRE(found);
	return 0;
}
