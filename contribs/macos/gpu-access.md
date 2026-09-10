# Native GPU connection policy

`DenyUnallocatedGPUConnections=yes` denies new direct GPU connections to tasks
whose authenticated local job/step allocation has no GPU:

```ini
# slurm.conf
TaskPlugin=task/darwin
```

```ini
# darwin.conf
DenyUnallocatedGPUConnections=yes
```

The step daemon decides from its authenticated local GRES records before task
fork. Unknown or malformed GPU counts fail the launch. Workload environment
variables do not authorize access. `SLURM_METAL_DEVICE_IDS` remains an advisory
selection list, and GPU-allocated steps retain access beyond the selected device
IDs. The policy does not enforce per-task binding.

## Installation and startup

The policy requires an empty remote SPANK stack for all steps, including those
with GPUs. The parser rejects all remote plugin records, including optional
records and included files, before path lookup or loading. Otherwise plugins
could acquire GPU capabilities before the task policy is installed.

At initialization, `task/darwin` runs the installed `slurm-darwin-gpu-probe` with
a clean environment, both with and without the policy. Metal computation must
succeed without it; acquisition of any device, including the default, must fail
with it. A missing helper, unavailable GPU, ineffective rule, abnormal exit or
timeout rejects configuration. Drain jobs and repeat these checks after OS or
GPU topology changes.

These checks run in every new step daemon, including for zero-GPU steps.
The baseline compiles and executes a Metal kernel on each enumerated device;
the denial probe checks that fresh acquisition fails. Each probe has a
120-second command deadline. GPU load or driver failure can delay or reject
otherwise CPU-only work when this option is enabled. Repeating qualification
checks the environment of the launching daemon without trusting a cached result
supplied by a workload. Account for this startup cost when enabling the policy.

The probe uses a C launcher that installs a fixed profile before executing a
separate Metal worker and its initializers. The executable path and profile
cannot be selected by a job. Keep the helpers, libraries and configuration
administrator controlled. Running the Metal worker directly does not install
a policy in another process.

## Launch boundary and limitations

The child installs the rule at the start of `exec_task`, before runtime/MPI
initialization, task hooks, prologs and the final image's loader. Job-supplied
`DYLD_*` variables must reach only the final restricted workload, never the
Slurm daemon or policy launcher. The policy survives fork, exec and `setsid`;
an attempt to replace it with an unrestricted sandbox fails.
User and administrator task epilogs fork separately from the step daemon and
install the same prepared policy before their executable or loader runs.

The rule denies IOKit opens matching `IOAccelerator`. Existing connections and
inherited Mach rights remain usable, and closing file descriptors does not
revoke them. An outside IPC broker can perform GPU work for a restricted task.
Mach-port transfers, task-port access, shared IOSurface objects and system
brokers need broader isolation. Administrative plugins and launcher code must
not pass GPU capabilities to a denied task.

This option provides no complete hostile-workload isolation, per-device access
control, GPU time/memory quota or hardware partitioning. It cannot exclude other
host applications from the GPU. Slurm GRES scheduling prevents conflicting
exclusive allocations within Slurm.

## API support and tests

The implementation uses custom Seatbelt profiles through `sandbox_init`, which
Apple marks deprecated/unsupported. Custom profile flag values are reserved;
this is not a stable public Metal enforcement API. The option is therefore
disabled by default and requires the runtime probes above. Chromium's
[GPU profile](https://github.com/chromium/chromium/blob/5ff8cac44033699354e883a18e8dbf32a1fc72be/sandbox/policy/mac/gpu.sb)
also uses the `IOAccelerator` filter.

`darwin-sandbox-test` checks flags and platform handling.
`darwin-gpu-sandbox-test` checks a Metal baseline, connection denial and inherited
restrictions through exec, fork/exec, spawn, session changes and profile reset.
Its children require no root. A machine without a GPU skips this test; enabling
the policy still requires a successful probe. See the
[administration README](README.md#testing) for tests through Slurm jobs.
