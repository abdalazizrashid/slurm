/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config.h"
#undef NDEBUG
#include <assert.h>
#include <string.h>
#include <sys/resource.h>

#include "src/common/slurm_rlimits_info.h"

static void _check_limit(const char *name, int resource, int propagate)
{
	slurm_rlimits_info_t *limit = get_slurm_rlimits_info();

	for (; limit->name; limit++) {
		if (strcmp(limit->name, name))
			continue;
		assert(limit->resource == resource);
		assert(limit->propagate_flag == propagate);
		return;
	}
	assert(!"resource limit missing from propagation table");
}

int main(void)
{
	assert(!parse_rlimits("ALL", PROPAGATE_RLIMITS));
#ifdef RLIMIT_AS
	assert(!parse_rlimits("AS", PROPAGATE_RLIMITS));
	_check_limit("AS", RLIMIT_AS, PROPAGATE_RLIMITS);
#endif
#ifdef RLIMIT_RSS
#ifdef __APPLE__
	/* RSS must not expose a second name for Darwin's address-space limit. */
	assert(RLIMIT_RSS == RLIMIT_AS);
	assert(parse_rlimits("RSS", PROPAGATE_RLIMITS) == -1);
	assert(parse_rlimits("RLIMIT_RSS", NO_PROPAGATE_RLIMITS) == -1);
#else
	/* Exercise explicit --propagate=RSS and PropagateResourceLimitsExcept. */
	assert(!parse_rlimits("RSS", PROPAGATE_RLIMITS));
	_check_limit("RSS", RLIMIT_RSS, PROPAGATE_RLIMITS);
#ifdef RLIMIT_AS
	_check_limit("AS", RLIMIT_AS, NO_PROPAGATE_RLIMITS);
#endif
	assert(!parse_rlimits("RLIMIT_RSS", NO_PROPAGATE_RLIMITS));
	_check_limit("RSS", RLIMIT_RSS, NO_PROPAGATE_RLIMITS);
#ifdef RLIMIT_AS
	_check_limit("AS", RLIMIT_AS, PROPAGATE_RLIMITS);
	assert(!parse_rlimits("RSS,AS", PROPAGATE_RLIMITS));
	_check_limit("RSS", RLIMIT_RSS, PROPAGATE_RLIMITS);
	_check_limit("AS", RLIMIT_AS, PROPAGATE_RLIMITS);
#endif
#endif
#endif
	return 0;
}
