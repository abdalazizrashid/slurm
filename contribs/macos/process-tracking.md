# Native process tracking

`ProctrackType=proctrack/darwin` retains descendants it observes after they
change process group, call `setsid` or become reparented. The default remains
`proctrack/pgid`. Neither backend provides escape-proof containment; use the
Darwin backend for trusted workloads that need detached-descendant tracking.
Concurrent jobs do not require one-job-per-node scheduling.

The backend registers each task before exec release and records its PID and
BSD/Mach birth timestamps. A kqueue monitor watches fork, exec and exit events,
and libproc child enumeration runs every 250 ms. Once discovered, a process
retains its membership independently of later ancestry changes. Slurm uses
that membership for accounting, suspend/resume and cancellation.

Signals target individually verified identities. A stale birth timestamp drops
the recorded process, but Darwin has no public operation that atomically checks
identity and sends a signal. A race remains between the final libproc check and
`kill`, so this is not protection against adversarial PID churn.

## Discovery and recovery limits

A process that forks and exits before enumeration can leave an undiscovered,
reparented child. Ordinary fast double-fork daemonization can trigger this gap
without a detectable error. `NOTE_FORK` events are coalesced and carry no child
PID; macOS does not support `NOTE_TRACK`, `NOTE_TRACKERR` or `NOTE_CHILD`.
A public kqueue watcher cannot subscribe atomically to every future descendant.

Cleanup repeatedly signals observed processes until none remain or a monotonic
timeout expires. Detected libproc/kqueue errors, exhausted discovery bounds and
timeouts fail cleanup and drain the node. A successful cleanup covers observed
members only. `srun --wait-for-children` is rejected because not all descendants
or their exit status can be recovered.

Membership lives in the step daemon. It cannot be fully reconstructed after a
step-daemon crash, replacement or reboot. Plan operational cleanup accordingly.
Attaching an already running external PID to an extern step is unsupported and
returns `ENOTSUP` before tracking/accounting state changes, even when native
task limits are disabled.

## Accounting

Accounting must associate each sampled descendant with a task. A process first
seen after reparenting may lack enough ancestry information for that association;
its task ownership is then unknown. Records for completed direct tasks retain
previously observed descendant ownership until step cleanup, so later descendant
usage can accumulate after the task's profile stream has ended. Registration
and process birth identities distinguish reused PIDs.

The step daemon samples before and during cleanup, then aggregates retained
records when ending accounting. Final direct-child wait usage is retained
alongside independently sampled CPU of descendants still observed after that
wait. For vanished descendants, overlap with wait usage cannot be determined;
reported CPU remains a conservative lower bound.
CPU and I/O after the last successful sample, and entirely unobserved processes,
can be missed. Detected sampling, enumeration or ownership gaps warn and suppress
sampled memory-limit checks for affected polls. These accounting steps do not
change a failed cleanup into success.

## Tests and API references

The native tests cover detached descendants, separate containers, suspend/resume,
cancellation, PID reuse and discovery failures. They also reproduce the missed
fast-descendant case. Test crash recovery, process churn and multi-node cleanup
with the site's workloads before deployment.

See Apple's [kqueue manual](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/kqueue.2.html),
XNU's [process event filter](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_event.c)
and [event flags](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/sys/event.h).
