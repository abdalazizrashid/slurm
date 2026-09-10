/*****************************************************************************\
 *  metal_probe.m - bounded actual Metal policy qualification.
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

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	bool deny;

	if ((argc != 2) || (strcmp(argv[1], "--expect-allowed") &&
			    strcmp(argv[1], "--expect-denied")))
		return 2;
	deny = !strcmp(argv[1], "--expect-denied");
	setbuf(stdout, NULL);
	alarm(90);
	@autoreleasepool {
		NSArray<id<MTLDevice> > *devices = MTLCopyAllDevices();
		id<MTLDevice> fallback = MTLCreateSystemDefaultDevice();

		if (!devices.count && !fallback) {
			puts("Metal direct GPU acquisition: denied/unavailable");
			/* Baseline must succeed before a deny result qualifies. */
			return deny ? 0 : 77;
		}
		if (deny) {
			fprintf(stderr,
				"GPU policy ineffective: Metal acquired a device\n");
			return 1;
		}
		/* Qualify every enumerated device, not just the default GPU. */
		if (!devices.count)
			devices = @[fallback];
		for (id<MTLDevice> device in devices) {
			NSError *error = nil;
			id<MTLCommandQueue> queue = [device newCommandQueue];
			NSString *source =
				@"#include <metal_stdlib>\n"
				 "using namespace metal;\n"
				 "kernel void add(device uint *v [[buffer(0)]], "
				 "uint i [[thread_position_in_grid]]) { v[i] += 7; }";
			id<MTLLibrary> library =
				[device newLibraryWithSource:source
						     options:nil
						       error:&error];
			id<MTLFunction> function =
				[library newFunctionWithName:@"add"];
			id<MTLComputePipelineState> pipeline =
				function ?
					[device newComputePipelineStateWithFunction:
							function
									      error:&
										    error] :
					nil;
			id<MTLBuffer> buffer = [device
				newBufferWithLength:256 * sizeof(unsigned)
					    options:MTLResourceStorageModeShared];
			id<MTLCommandBuffer> command = [queue commandBuffer];
			id<MTLComputeCommandEncoder> encoder =
				[command computeCommandEncoder];

			if (!queue || !library || !pipeline || !buffer ||
			    !command || !encoder) {
				fprintf(stderr,
					"Metal baseline setup failed: %s\n",
					error ? error.description.UTF8String :
						"resource unavailable");
				return 1;
			}
			unsigned *values = buffer.contents;
			for (unsigned i = 0; i < 256; i++)
				values[i] = i;
			[encoder setComputePipelineState:pipeline];
			[encoder setBuffer:buffer offset:0 atIndex:0];
			[encoder dispatchThreads:MTLSizeMake(256, 1, 1)
				threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
			[encoder endEncoding];
			[command commit];
			[command waitUntilCompleted];
			if (command.status != MTLCommandBufferStatusCompleted) {
				fprintf(stderr,
					"Metal baseline command failed: %s\n",
					command.error.description.UTF8String);
				return 1;
			}
			for (unsigned i = 0; i < 256; i++) {
				if (values[i] != i + 7) {
					fprintf(stderr,
						"Metal baseline result mismatch\n");
					return 1;
				}
			}
			printf("Metal registryID=%016llx compute=PASS values=256\n",
			       (unsigned long long) device.registryID);
		}
	}
	return 0;
}
