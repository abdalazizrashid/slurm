# Native macOS resource controls

`TaskPlugin=task/darwin` requires a readable regular `darwin.conf` beside `slurm.conf`,
including on configless nodes. An empty or comments-only file disables native
policies. Invalid values and unavailable requested controls reject
initialization or launch. Restart daemons after changing the file.

The settings are independent of job CPU/memory requests and time limits.
See `darwin.conf(5)` for parameter syntax, defaults and ranges. For example:

```ini
# slurm.conf
TaskPlugin=task/darwin
DefRuntimePlugin=none
```

```ini
# darwin.conf
PerProcessCPUTimeSeconds=3600
PerProcessAddressSpaceMiB=0
InitialTaskImageFootprintMiB=0
DenyUnallocatedGPUConnections=no
```

This limits each process to 3600 CPU seconds and leaves the other policies
disabled. Hard aggregate CPU/RAM quotas and complete device isolation are
unavailable. For the narrower
GPU connection policy, see [gpu-access.md](gpu-access.md).

## Per-process limits

`PerProcessCPUTimeSeconds` lowers `RLIMIT_CPU`, which counts CPU seconds across
a process's threads. `PerProcessAddressSpaceMiB` lowers `RLIMIT_AS`, which counts
reserved mappings as well as committed memory. Apple Silicon applications can
reserve hundreds of GiB while using much less RAM; do not derive an address-space
limit directly from a job's physical-memory request.

The limits apply before task prologs and exec, preserve stricter existing hard
and soft bounds, and survive fork/exec. Task epilogs receive the same CPU and
address-space limits before their executable or loader runs.
An unprivileged process cannot raise a
lowered hard bound. CPU usage is counted separately for each process, so a
per-process ceiling is not a shared job budget or CPU rate limit.

Darwin aliases `RLIMIT_RSS` to `RLIMIT_AS`. Slurm exposes AS alone and rejects
explicit RSS propagation. `VSizeFactor` scales the requested limit before
comparison with the existing hard bound and preserves a stricter soft bound.

## Initial-image footprint limit

`InitialTaskImageFootprintMiB` uses the private `posix_spawnattr_setjetsam_ext`
API to install fatal active/inactive footprint limits at the final
`POSIX_SPAWN_SETEXEC`. This preserves the task PID, descriptors and step daemon's
wait relationship. The limit covers loading the image and its initializers.
A script's initial image is its interpreter.

This option requires `DefRuntimePlugin=none` and a selected `runtime/none`
context. Other runtimes are rejected before the child is released. Missing
symbols, a disabled high-water switch, spawn-attribute errors and failed exec
fail the launch. The policy is passed in process-local state, not a workload
environment variable.

Ordinary later exec and fork can reset the limit to the kernel default, even
when exec preserves the PID. The option therefore does not limit the whole
task lifetime or its descendants. Physical footprint differs from RSS, virtual
address space and a reservation of shared/kernel memory. Fatal-limit delivery
can allow transient excess. Test `darwin-launch-test` on each deployment OS;
acceptance of private spawn attributes alone does not establish enforcement.

## Accounting and unsupported operations

Several jobs can share a Mac through scheduler allocations. Set application
thread counts to match their allocations and leave OS memory headroom in
`RealMemory`. Hard core-type placement is unavailable. Explicit CPU/NUMA masks,
CPU/GPU frequency controls, `TaskPluginParam=OOMKillStep` and reduction of a
running job's aggregate memory limit are rejected.

Attaching an external PID to an extern step is unsupported, including with
native policies disabled. Enabled launch policies cannot be applied to an
already running process that has passed the required launch boundary.
See [process-tracking.md](process-tracking.md) for normal task descendants.

`JobAcctGatherParams=OverMemoryKill` cancels jobs based on observed usage. Shared
pages can be counted more than once, and processes can allocate between polls.
Missing RSS and virtual-memory values are excluded independently; available
samples exceeding a limit can still cause cancellation. A failed step query
or disabled accounting response does not discard the registered job limit.
The node retains it until the job disappears from the step inventory.
Process-discovery and ownership gaps can also suppress affected checks, as
described in the process-tracking guide.

## Tests and API references

`darwin-limits-test` exercises CPU/address-space limits and inheritance.
`darwin-launch-test` checks bounded child allocations and fork, exec and shell
transitions; it needs no root. `darwin-task-test` covers configuration and
runtime checks. `job-mem-limit-test` covers incomplete node samples and recovery.

The private footprint interface needs testing on each OS and architecture.
Implementation references: XNU [spawn attributes](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/libsyscall/wrappers/spawn/posix_spawn.c)
and [exec handling](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_exec.c).
