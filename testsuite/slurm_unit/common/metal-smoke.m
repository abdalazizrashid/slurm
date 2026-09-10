/*****************************************************************************\
 * metal-smoke.m - bounded native GPU compute, optionally inside a Slurm step.
 * This file is part of Slurm. Distributed under GPL version 2 or later.
\*****************************************************************************/

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(expr) \
	do { \
		if (!(expr)) { \
			fprintf(stderr, "%s:%d: %s failed\n", __FILE__, \
				__LINE__, #expr); \
			return EXIT_FAILURE; \
		} \
	} while (0)

int main(void)
{
	alarm(30);
	@autoreleasepool {
		const char *assigned = getenv("SLURM_METAL_DEVICE_IDS");
		uint64_t wanted = 0;
		id<MTLDevice> device = nil;
		NSArray<id<MTLDevice> > *devices = MTLCopyAllDevices();
		NSError *error = nil;
		const unsigned count = 4096;

		if (assigned) {
			char *end;
			errno = 0;
			wanted = strtoull(assigned, &end, 16);
			CHECK(!errno && end != assigned &&
			      (!*end || *end == ','));
		} else if (getenv("SLURM_JOB_ID")) {
			fprintf(stderr,
				"Slurm job has no Metal GPU assignment\n");
			return EXIT_FAILURE;
		}
		for (id<MTLDevice> candidate in devices) {
			if (!assigned || candidate.registryID == wanted) {
				device = candidate;
				break;
			}
		}
		if (!device) {
			fprintf(stderr, "No %sMetal device available\n",
				assigned ? "assigned " : "");
			return assigned ? EXIT_FAILURE : 77;
		}
		NSString *source =
			@"#include <metal_stdlib>\n"
			 "using namespace metal;\n"
			 "kernel void transform(device uint *a [[buffer(0)]], "
			 "uint i [[thread_position_in_grid]]) { a[i] = 2*a[i]+1; }";
		id<MTLLibrary> library = [device newLibraryWithSource:source
							      options:nil
								error:&error];
		if (!library)
			fprintf(stderr, "%s\n",
				error.localizedDescription.UTF8String);
		CHECK(library);
		id<MTLFunction> function =
			[library newFunctionWithName:@"transform"];
		CHECK(function);
		id<MTLComputePipelineState> pipeline =
			[device newComputePipelineStateWithFunction:function
							      error:&error];
		CHECK(pipeline);
		id<MTLBuffer> buffer = [device
			newBufferWithLength:count * sizeof(unsigned)
				    options:MTLResourceStorageModeShared];
		CHECK(buffer);
		unsigned *values = buffer.contents;
		for (unsigned i = 0; i < count; i++)
			values[i] = i;
		id<MTLCommandQueue> queue = [device newCommandQueue];
		CHECK(queue);
		id<MTLCommandBuffer> command = [queue commandBuffer];
		CHECK(command);
		id<MTLComputeCommandEncoder> encoder =
			[command computeCommandEncoder];
		CHECK(encoder);
		[encoder setComputePipelineState:pipeline];
		[encoder setBuffer:buffer offset:0 atIndex:0];
		NSUInteger width = MIN((NSUInteger) 64,
				       pipeline.maxTotalThreadsPerThreadgroup);
		[encoder dispatchThreads:MTLSizeMake(count, 1, 1)
			threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
		[encoder endEncoding];
		[command commit];
		[command waitUntilCompleted];
		CHECK(command.status == MTLCommandBufferStatusCompleted);
		for (unsigned i = 0; i < count; i++)
			CHECK(values[i] == 2 * i + 1);
		printf("Metal compute passed: %s registryID=%016" PRIx64
		       " unified_memory=%s values=%u assigned=%s\n",
		       device.name.UTF8String, device.registryID,
		       device.hasUnifiedMemory ? "yes" : "no", count,
		       assigned ? assigned : "standalone");
	}
	return EXIT_SUCCESS;
}
