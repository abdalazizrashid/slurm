/*****************************************************************************\
 *  gpu_metal.m - Discover Metal GPU devices on macOS.
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

#include <errno.h>

#include "src/common/log.h"
#include "src/common/slurm_xlator.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"
#include "src/interfaces/gpu.h"
#include "src/plugins/gpu/common/gpu_common.h"

const char plugin_name[] = "GPU Metal plugin";
const char plugin_type[] = "gpu/metal";
const uint32_t plugin_version = SLURM_VERSION_NUMBER;

/* Registry IDs are shared across processes, unlike enumeration order. */
static NSArray<id<MTLDevice> > *_devices(void)
{
	return [MTLCopyAllDevices()
		sortedArrayUsingComparator:^NSComparisonResult(id<MTLDevice> a,
							       id<MTLDevice>
								       b) {
		  if (a.registryID < b.registryID)
			  return NSOrderedAscending;
		  if (a.registryID > b.registryID)
			  return NSOrderedDescending;
		  return NSOrderedSame;
		}];
}

extern int init(void)
{
	debug("%s: loaded %s", __func__, plugin_name);
	return SLURM_SUCCESS;
}

extern void fini(void)
{
	debug("%s: unloading %s", __func__, plugin_name);
}

extern list_t *gpu_p_get_system_gpu_list(node_config_load_t *node_conf)
{
	list_t *devices = list_create(destroy_gres_slurmd_conf);

	@autoreleasepool {
		for (id<MTLDevice> device in _devices()) {
			gres_slurmd_conf_t conf = {
				.config_flags = GRES_CONF_AUTODETECT |
						GRES_CONF_HAS_ID |
						GRES_CONF_ENV_METAL,
				.count = 1,
				.cpu_cnt = node_conf->cpu_cnt,
				.name = "gpu",
			};

			conf.type_name = xstrdup(device.name.UTF8String);
			gpu_common_underscorify_tolower(conf.type_name);
			conf.unique_id = xstrdup_printf("%016" PRIx64,
							device.registryID);
			/*
			 * Metal exposes neither a device file nor CPU affinity.
			 * The working-set recommendation is not dedicated VRAM,
			 * an enforced memory limit, or per-job GPU usage.
			 */
			info("Metal GPU %s registryID=%s unified_memory=%s recommended_working_set=%"PRIu64" bytes",
			     device.name.UTF8String, conf.unique_id,
			     device.hasUnifiedMemory ? "yes" : "no",
			     device.recommendedMaxWorkingSetSize);
			add_gres_to_list(devices, &conf);
			xfree(conf.type_name);
			xfree(conf.unique_id);
		}
	}
	return devices;
}

extern void gpu_p_get_device_count(uint32_t *device_count)
{
	@autoreleasepool {
		*device_count = (uint32_t) _devices().count;
	}
}

extern void gpu_p_step_hardware_init(bitstr_t *usable_gpus, char *tres_freq)
{
	if (usable_gpus && bit_set_count(usable_gpus) && tres_freq &&
	    strstr(tres_freq, "gpu:")) {
		/* This plugin interface cannot return a launch error. */
		error("Metal does not support GPU frequency control");
		fprintf(stderr, "GpuFreq=control_disabled (Metal)\n");
	}
}

extern void gpu_p_step_hardware_fini(void) {}

extern char *gpu_p_test_cpu_conv(char *cpu_range)
{
	errno = ENOTSUP;
	return NULL;
}

extern int gpu_p_energy_read(uint32_t device_index, gpu_status_t *gpu)
{
	gpu->last_update_watt = NO_VAL;
	gpu->energy.ave_watts = NO_VAL;
	gpu->energy.current_watts = NO_VAL;
	gpu->energy.base_consumed_energy = NO_VAL64;
	gpu->energy.consumed_energy = NO_VAL64;
	gpu->energy.previous_consumed_energy = NO_VAL64;
	gpu->energy.last_adjustment = NO_VAL64;
	return ENOTSUP;
}

extern int gpu_p_usage_read(pid_t pid, acct_gather_data_t *data)
{
	int memory, utilization;

	gpu_get_tres_pos(&memory, &utilization);
	/* Public Metal APIs do not sample another process's GPU usage. */
	if (memory >= 0)
		data[memory].size_read = data[memory].size_write = INFINITE64;
	if (utilization >= 0)
		data[utilization].size_read = data[utilization].size_write =
			INFINITE64;
	return ENOTSUP;
}
