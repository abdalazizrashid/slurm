/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "slurm/pmi.h"

int main(void)
{
	int spawned, size, rank, length;
	char *name;
	char key[64], value[64], expected[64];

	alarm(30);
	if (PMI_Init(&spawned) || PMI_Get_size(&size) || PMI_Get_rank(&rank) ||
	    size < 2 || PMI_KVS_Get_name_length_max(&length) || length <= 0)
		return EXIT_FAILURE;
	name = malloc(length);
	if (!name || PMI_KVS_Get_my_name(name, length))
		return EXIT_FAILURE;
	snprintf(key, sizeof(key), "macos-rank-%d", rank);
	snprintf(value, sizeof(value), "value-%d", rank);
	if (PMI_KVS_Put(name, key, value) || PMI_KVS_Commit(name) ||
	    PMI_Barrier())
		return EXIT_FAILURE;
	for (int peer = 0; peer < size; peer++) {
		snprintf(key, sizeof(key), "macos-rank-%d", peer);
		snprintf(expected, sizeof(expected), "value-%d", peer);
		if (PMI_KVS_Get(name, key, value, sizeof(value)) ||
		    strcmp(value, expected))
			return EXIT_FAILURE;
	}
	free(name);
	if (PMI_Finalize())
		return EXIT_FAILURE;
	printf("PMI1 rank=%d size=%d KVS=PASS\n", rank, size);
	return EXIT_SUCCESS;
}
