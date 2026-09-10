/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A bounded initial-image workload: no launcher, fork or subsequent exec. */
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
	const size_t bytes = 384 * 1024 * 1024;
	volatile unsigned char *memory;

	alarm(10);
	puts("allocation-start");
	fflush(stdout);
	memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANON, -1, 0);
	if (memory == MAP_FAILED)
		return 2;
	for (size_t offset = 0; offset < bytes; offset += 4096)
		memory[offset] = 1;
	puts("allocation-complete");
	fflush(stdout);
	return munmap((void *) memory, bytes) ? 3 : 0;
}
