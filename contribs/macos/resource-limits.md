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
```

```ini
# darwin.conf
PerProcessCPUTimeSeconds=3600
PerProcessAddressSpaceMiB=0
```

This limits each process to 3600 CPU seconds and leaves the address-space limit
disabled. Hard aggregate CPU/RAM quotas and complete device isolation are
unavailable.

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

## Tests

`darwin-limits-test` exercises CPU/address-space limits and inheritance.
`darwin-task-test` covers configuration and unsupported-control checks.
`job-mem-limit-test` covers incomplete node samples and recovery.
