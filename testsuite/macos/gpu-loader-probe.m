/* SPDX-License-Identifier: GPL-2.0-or-later
 * Expectation-only Metal checks before the host's main(). No sandbox is applied.
 * Inject this library into the test job only, never into Slurm daemons.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool compute(id<MTLDevice> device)
{
	NSError *error = nil;
	id<MTLCommandQueue> queue = [device newCommandQueue];
	NSString *source =
		@"#include <metal_stdlib>\n"
		 "using namespace metal;\n"
		 "kernel void add(device uint *v [[buffer(0)]], "
		 "uint i [[thread_position_in_grid]]) { v[i] += 3; }";
	id<MTLLibrary> library = [device newLibraryWithSource:source
						      options:nil
							error:&error];
	id<MTLFunction> function = [library newFunctionWithName:@"add"];
	id<MTLComputePipelineState> pipeline =
		function ? [device newComputePipelineStateWithFunction:function
								 error:&error] :
			   nil;
	id<MTLBuffer> buffer =
		[device newBufferWithLength:16 * sizeof(unsigned)
				    options:MTLResourceStorageModeShared];
	id<MTLCommandBuffer> command = [queue commandBuffer];
	id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];

	if (!queue || !library || !pipeline || !buffer || !command || !encoder)
		return false;
	unsigned *values = buffer.contents;
	for (unsigned i = 0; i < 16; i++)
		values[i] = i;
	[encoder setComputePipelineState:pipeline];
	[encoder setBuffer:buffer offset:0 atIndex:0];
	[encoder dispatchThreads:MTLSizeMake(16, 1, 1)
		threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
	[encoder endEncoding];
	[command commit];
	[command waitUntilCompleted];
	if (command.status != MTLCommandBufferStatusCompleted)
		return false;
	for (unsigned i = 0; i < 16; i++)
		if (values[i] != i + 3)
			return false;
	return true;
}

__attribute__((constructor)) static void check_before_main(void)
{
	const char *expect = getenv("SLURM_GPU_LOADER_EXPECT");
	const char *marker = getenv("SLURM_GPU_LOADER_MARKER");
	bool deny, matched, computed = false;

	alarm(60);
	if (!expect ||
	    (strcmp(expect, "denied") && strcmp(expect, "allowed")) ||
	    !marker || marker[0] != '/') {
		fputs("GPU loader probe requires an expectation and absolute marker path\n",
		      stderr);
		_exit(90);
	}
	deny = !strcmp(expect, "denied");
	@autoreleasepool {
		NSArray<id<MTLDevice>> *devices = MTLCopyAllDevices();
		id<MTLDevice> fallback = MTLCreateSystemDefaultDevice();
		bool present = devices.count || fallback;

		if (!deny && present) {
			computed = true;
			if (!devices.count)
				computed = compute(fallback);
			for (id<MTLDevice> device in devices)
				computed = compute(device) && computed;
		}
		matched = deny ? !present : present && computed;
		int fd = open(marker,
			      O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW |
				      O_CLOEXEC,
			      0600);
		if (fd < 0) {
			perror("GPU loader marker");
			_exit(91);
		}
		int rc = dprintf(
			fd,
			"{\"constructor_ran\":true,\"expected\":\"%s\","
			"\"devices\":%lu,\"default_device\":%s,\"computed\":%s,"
			"\"matched\":%s}\n",
			expect, (unsigned long) devices.count,
			fallback ? "true" : "false",
			computed ? "true" : "false",
			matched ? "true" : "false");
		if (close(fd) || rc < 0)
			_exit(92);
	}
	if (!matched)
		_exit(93);
	/* The separate host now enters main; the harness verifies both markers. */
}
