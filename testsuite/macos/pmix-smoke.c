/*****************************************************************************\
 * pmix-smoke.c - exercise native PMIx initialization and cross-rank exchange.
 *****************************************************************************
 * This file is part of Slurm. See <https://slurm.schedmd.com/>.
 * Distributed under the GNU General Public License, version 2 or later.
\*****************************************************************************/

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pmix.h>

#define REQUIRE(call) \
	do { \
		pmix_status_t rc = (call); \
		if (rc != PMIX_SUCCESS) { \
			fprintf(stderr, "%s: %s\n", #call, \
				PMIx_Error_string(rc)); \
			return EXIT_FAILURE; \
		} \
	} while (0)

int main(void)
{
	pmix_proc_t self, peer;
	pmix_value_t value = PMIX_VALUE_STATIC_INIT, *received = NULL;
	pmix_info_t collect = PMIX_INFO_STATIC_INIT;
	char text[64], expected[64];
	uint32_t size;
	bool yes = true;

	alarm(20);
	REQUIRE(PMIx_Init(&self, NULL, 0));
	PMIX_PROC_LOAD(&peer, self.nspace, PMIX_RANK_WILDCARD);
	REQUIRE(PMIx_Get(&peer, PMIX_JOB_SIZE, NULL, 0, &received));
	if (!received || (received->type != PMIX_UINT32) ||
	    ((size = received->data.uint32) < 2))
		return EXIT_FAILURE;
	PMIX_VALUE_RELEASE(received);
	snprintf(text, sizeof(text), "value-%" PRIu32, self.rank);
	PMIX_VALUE_LOAD(&value, text, PMIX_STRING);
	REQUIRE(PMIx_Put(PMIX_GLOBAL, "slurm-macos-value", &value));
	PMIX_VALUE_DESTRUCT(&value);
	REQUIRE(PMIx_Commit());
	PMIX_INFO_LOAD(&collect, PMIX_COLLECT_DATA, &yes, PMIX_BOOL);
	REQUIRE(PMIx_Fence(NULL, 0, &collect, 1));
	PMIX_INFO_DESTRUCT(&collect);
	for (peer.rank = 0; peer.rank < size; peer.rank++) {
		snprintf(expected, sizeof(expected), "value-%" PRIu32,
			 peer.rank);
		REQUIRE(PMIx_Get(&peer, "slurm-macos-value", NULL, 0,
				 &received));
		if (!received || (received->type != PMIX_STRING) ||
		    !received->data.string ||
		    strcmp(received->data.string, expected))
			return EXIT_FAILURE;
		PMIX_VALUE_RELEASE(received);
	}
	REQUIRE(PMIx_Finalize(NULL, 0));
	printf("PMIx rank=%" PRIu32 " size=%" PRIu32 " KVS=PASS\n", self.rank,
	       size);
	return EXIT_SUCCESS;
}
