/*****************************************************************************\
 *  task_darwin.c - native macOS per-process resource controls.
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

#include "config.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#include "src/common/darwin_launch.h"
#include "src/common/darwin_limits.h"
#include "src/common/parse_config.h"
#include "src/common/read_config.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"
#include "src/interfaces/runtime.h"
#include "src/interfaces/task.h"

const char plugin_name[] = "Native macOS task resource controls";
const char plugin_type[] = "task/darwin";
const uint32_t plugin_version = SLURM_VERSION_NUMBER;

static uint64_t cpu_seconds, address_mib, footprint_mib;

static int _unsupported(cpu_bind_type_t cpu, mem_bind_type_t mem, uint32_t min,
			uint32_t max, uint32_t gov, const char *tres_freq,
			char **err_msg)
{
	const char *reason = NULL;
	const cpu_bind_type_t cpu_mask =
		CPU_BIND_T_TO_MASK | CPU_BIND_T_AUTO_TO_MASK | CPU_BIND_MAP |
		CPU_BIND_MASK | CPU_BIND_LDRANK | CPU_BIND_LDMAP |
		CPU_BIND_LDMASK | CPU_BIND_ONE_THREAD_PER_CORE;

	if (cpu & cpu_mask)
		reason =
			"macOS does not support restricting execution to a CPU mask";
	else if (mem & ~(MEM_BIND_NONE | MEM_BIND_VERBOSE))
		reason = "macOS does not support NUMA memory binding";
	else if ((min && min != NO_VAL) || (max && max != NO_VAL) ||
		 (gov && gov != NO_VAL))
		reason = "macOS does not support CPU frequency controls";
	else if (tres_freq && strstr(tres_freq, "gpu:"))
		reason = "macOS does not support GPU frequency controls";
	if (!reason)
		return SLURM_SUCCESS;
	error("task/darwin: %s", reason);
	if (err_msg)
		*err_msg = xstrdup(reason);
	errno = ENOTSUP;
	return SLURM_ERROR;
}

extern int init(void)
{
	s_p_options_t options[] = {
		{ "PerProcessCPUTimeSeconds", S_P_UINT64 },
		{ "PerProcessAddressSpaceMiB", S_P_UINT64 },
		{ "InitialTaskImageFootprintMiB", S_P_UINT64 },
		{ NULL }
	};
	s_p_hashtbl_t *table = s_p_hashtbl_create(options);
	char *path = get_extra_conf_path("darwin.conf");
	struct stat st;
	int rc = SLURM_ERROR, native_rc, saved_errno;

	cpu_seconds = address_mib = footprint_mib = 0;
	/* Invalid configuration must not silently disable configured limits. */
	if (!path || stat(path, &st)) {
		error("task/darwin requires a readable darwin.conf: %s", path);
		goto done;
	}
	if (!S_ISREG(st.st_mode)) {
		error("task/darwin requires a regular darwin.conf: %s", path);
		errno = EINVAL;
		goto done;
	}
	if (s_p_parse_file(table, NULL, path, 0, NULL) == SLURM_ERROR)
		goto done;
	s_p_get_uint64(&cpu_seconds, "PerProcessCPUTimeSeconds", table);
	s_p_get_uint64(&address_mib, "PerProcessAddressSpaceMiB", table);
	s_p_get_uint64(&footprint_mib, "InitialTaskImageFootprintMiB", table);
	if (_unsupported(slurm_conf.task_plugin_param, 0, 0, 0, 0, NULL, NULL))
		goto done;
	/* The manager adds this default after task_p_pre_setuid() runs. */
	if (slurm_conf.gpu_freq_def && slurm_conf.gpu_freq_def[0]) {
		error("task/darwin does not support GpuFreqDef");
		errno = ENOTSUP;
		goto done;
	}
	if (slurm_conf.task_plugin_param & OOM_KILL_STEP) {
		error("task/darwin does not support TaskPluginParam=OOMKillStep");
		errno = ENOTSUP;
		goto done;
	}
	native_rc =
		darwin_limits_validate(cpu_seconds, address_mib, footprint_mib);
	if (!native_rc && footprint_mib)
		native_rc = darwin_launch_probe(footprint_mib);
	if (native_rc) {
		error("task/darwin resource configuration unavailable: %s",
		      slurm_strerror(native_rc));
		errno = native_rc;
		goto done;
	}
	rc = SLURM_SUCCESS;
	darwin_launch_prepare_limits(cpu_seconds, address_mib);
	debug("task/darwin loaded: CPU time=%"PRIu64" seconds/process, "
	      "address space=%"PRIu64" MiB/process, "
	      "initial task image footprint=%"PRIu64" MiB (0 disables)",
	      cpu_seconds, address_mib, footprint_mib);
done:
	/* Preserve the selected failure reason across configuration cleanup. */
	saved_errno = errno;
	s_p_hashtbl_destroy(table);
	xfree(path);
	if (rc == SLURM_ERROR)
		errno = saved_errno;
	return rc;
}

extern int fini(void)
{
	cpu_seconds = address_mib = footprint_mib = 0;
	darwin_launch_prepare(0);
	darwin_launch_prepare_limits(0, 0);
	return SLURM_SUCCESS;
}

static int _runtime_supported(void)
{
	if (!footprint_mib || runtime_g_is_none())
		return SLURM_SUCCESS;
	error("InitialTaskImageFootprintMiB requires runtime/none so the final image is launched by Slurm");
	errno = ENOTSUP;
	return SLURM_ERROR;
}

static int _oom_supported(bool oom_kill_step, uint32_t step_id, char **err_msg)
{
	const char *reason = "macOS does not support OOMKillStep";

	/* Extern steps do not apply the allocation's OOM policy. */
	if (!oom_kill_step || step_id == SLURM_EXTERN_CONT)
		return SLURM_SUCCESS;
	error("task/darwin: %s", reason);
	if (err_msg)
		*err_msg = xstrdup(reason);
	errno = ENOTSUP;
	return SLURM_ERROR;
}

extern int task_p_slurmd_batch_request(batch_job_launch_msg_t *req)
{
	return _oom_supported(req->oom_kill_step, SLURM_BATCH_SCRIPT, NULL);
}

extern int task_p_slurmd_launch_request(launch_tasks_request_msg_t *req,
					uint32_t node_id, char **err_msg)
{
	if (_oom_supported(req->oom_kill_step, req->step_id.step_id, err_msg))
		return SLURM_ERROR;
	return _unsupported(req->cpu_bind_type, req->mem_bind_type,
			    req->cpu_freq_min, req->cpu_freq_max,
			    req->cpu_freq_gov, req->tres_freq, err_msg);
}

extern int task_p_pre_setuid(stepd_step_rec_t *step)
{
	if (_runtime_supported())
		return SLURM_ERROR;
	if (_oom_supported(step->oom_kill_step, step->step_id.step_id, NULL))
		return SLURM_ERROR;
	return _unsupported(step->cpu_bind_type, step->mem_bind_type,
			    step->cpu_freq_min, step->cpu_freq_max,
			    step->cpu_freq_gov, step->tres_freq, NULL);
}

extern int task_p_pre_launch_priv(stepd_step_rec_t *step, uint32_t node_tid,
				  uint32_t global_tid)
{
	/* Recheck policy populated after the early hooks, before task release. */
	if (_runtime_supported() ||
	    _oom_supported(step->oom_kill_step, step->step_id.step_id, NULL))
		return SLURM_ERROR;
	return _unsupported(step->cpu_bind_type, step->mem_bind_type,
			    step->cpu_freq_min, step->cpu_freq_max,
			    step->cpu_freq_gov, step->tres_freq, NULL);
}

extern int task_p_pre_launch(stepd_step_rec_t *step)
{
	int rc = darwin_limits_apply(cpu_seconds, address_mib);

	/* Carry policy in process-local state, not a user-editable environment. */
	if (!rc)
		rc = darwin_launch_prepare(footprint_mib);
	if (!rc)
		return SLURM_SUCCESS;
	error("Cannot install per-process resource limits: %s",
	      slurm_strerror(rc));
	errno = rc;
	return SLURM_ERROR;
}

extern int task_p_post_term(stepd_step_rec_t *step,
			    stepd_step_task_info_t *task)
{
	return SLURM_SUCCESS;
}

extern int task_p_post_step(stepd_step_rec_t *step)
{
	return SLURM_SUCCESS;
}

extern int task_p_add_pid(pid_t pid)
{
	/* Public rlimits cannot be imposed remotely on an adopted process. */
	if (cpu_seconds || address_mib || footprint_mib) {
		error("task/darwin cannot apply configured launch limits to an adopted process");
		errno = ENOTSUP;
		return SLURM_ERROR;
	}
	return SLURM_SUCCESS;
}

extern int task_p_update_mem_limit(stepd_step_rec_t *step, uint64_t new_job_mem,
				   uint64_t new_step_mem)
{
	error("task/darwin cannot enforce a running job's aggregate memory reduction");
	errno = ENOTSUP;
	return SLURM_ERROR;
}
