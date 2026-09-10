/*****************************************************************************\
 * pmi2-smoke.c - exercise native PMI2 initialization and cross-rank KVS.
 *****************************************************************************
 * This file is part of Slurm. See <https://slurm.schedmd.com/>.
 * Distributed under the GNU General Public License, version 2 or later.
\*****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "slurm/pmi2.h"

int main(void)
{
	int spawned, size, rank, appnum, length;
	char key[64], value[64], expected[64];

	alarm(20);
	if (PMI2_Init(&spawned, &size, &rank, &appnum) || (size < 2))
		return EXIT_FAILURE;
	snprintf(key, sizeof(key), "macos-rank-%d", rank);
	snprintf(value, sizeof(value), "value-%d", rank);
	if (PMI2_KVS_Put(key, value) || PMI2_KVS_Fence())
		return EXIT_FAILURE;
	for (int peer = 0; peer < size; peer++) {
		snprintf(key, sizeof(key), "macos-rank-%d", peer);
		snprintf(expected, sizeof(expected), "value-%d", peer);
		if (PMI2_KVS_Get(NULL, peer, key, value, sizeof(value),
				 &length) ||
		    strcmp(value, expected))
			return EXIT_FAILURE;
	}
	if (PMI2_Finalize())
		return EXIT_FAILURE;
	printf("PMI2 rank=%d size=%d KVS=PASS\n", rank, size);
	return EXIT_SUCCESS;
}
