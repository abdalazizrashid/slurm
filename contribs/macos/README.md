# Native macOS administration

These examples accompany the [macOS guide](https://slurm.schedmd.com/macos.html).
`make -C contribs/macos install` copies them to `share/slurm/macos`.
Account creation, service activation, power settings and PAM configuration are
separate administration tasks.

Use the macOS guide for build dependencies, cluster configuration, and the
differences between allocation, accounting, and enforcement. Consult
[resource-limits.md](resource-limits.md) before enabling `task/darwin`.
The optional `proctrack/darwin` backend and its discovery/recovery limitations
are described in [process-tracking.md](process-tracking.md).

## Render launchd files

Choose persistent, absolute paths and administrator-controlled binaries and
configuration for a privileged `slurmd`. Render and check the three service files:

```sh
python3 render-launchd.py \
  --prefix /opt/slurm \
  --config-dir /opt/slurm/etc \
  --state-dir /var/db/slurm \
  --slurm-user slurm \
  --output-dir ./launchd-example
plutil -lint ./launchd-example/*.plist
```

Only install the daemons that this machine will run. A compute node normally
needs `slurmd`; a controller normally needs `slurmctld`; `slurmdbd` also requires
a configured, reachable database. The templates use foreground mode (`-D`) so
launchd supervises the actual daemon. They set `SLURM_CONF` explicitly and do not
read interactive shell initialization files. `slurmdbd.conf` belongs beside the
selected `slurm.conf`.

Provision the selected accounts and directories before loading services:

| Item | Required ownership/access |
| --- | --- |
| Slurm binaries, libraries and plugin directory | Administrator controlled; executable/readable by their users |
| `slurm.conf`, `gres.conf`, `darwin.conf` | Administrator controlled; readable by the daemons and relevant clients |
| `slurmdbd.conf` and authentication secrets | Restricted to the configured service account; follow their existing Slurm/MUNGE documentation |
| State root and `log` directory | Searchable by daemon users; create each log with appropriate ownership |
| `StateSaveLocation` | Writable by `SlurmUser` (`slurmctld`) |
| `SlurmdSpoolDir` | Owned by root for normal multiuser `slurmd`; writable only as required by Slurm |

The example `state-dir` only controls working-directory and launchd log paths.
Set `StateSaveLocation`, `SlurmdSpoolDir`, PID files and daemon log paths separately
in `slurm.conf`, and the database log/PID paths in `slurmdbd.conf`. Match them to
the provisioned directories. Keep controller and compute-node state distinct.

The `slurmd` template runs as root, as required for ordinary jobs under different
user identities. Controller and database templates run as the chosen `SlurmUser`.
Use the same numeric user/group identities and supplementary groups on all
participating hosts. Configure MUNGE with the site's shared key and ensure it is
ready before starting Slurm. `KeepAlive` retries failures; it does not establish
a service dependency or repair authentication/configuration errors.

## Install and stop services

After provisioning the accounts, configuration and directories, install the
services the host needs. For example, on a compute node:

```sh
sudo install -o root -g wheel -m 644 ./launchd-example/org.slurm.slurmd.plist \
  /Library/LaunchDaemons/org.slurm.slurmd.plist
sudo launchctl bootstrap system /Library/LaunchDaemons/org.slurm.slurmd.plist
sudo launchctl print system/org.slurm.slurmd
```

Inspect the configured Slurm log and `log/slurmd-launchd.log` for failures. The templates
rate-limit restarts to 30 seconds and allow 60 seconds between termination and
forced kill. `slurmd` uses `AbandonProcessGroup=true`, avoiding launchd's default
cleanup of the daemon's remaining process group on daemon exit. Detached
descendants may still escape tracking; test restart and recovery with the
intended workloads.

Before stopping or upgrading a compute node, drain it and wait for jobs to end:

```sh
scontrol update NodeName=mac01 State=DRAIN Reason=maintenance
squeue -w mac01
sudo launchctl bootout system/org.slurm.slurmd
```

Once no jobs remain, removal of the installed plist makes that service's removal
persistent. Preserve configuration/state for rollback. After an upgrade, reload
the reviewed plist with `bootstrap`, verify registration and logs, then resume
the node with `scontrol update NodeName=mac01 State=RESUME`. Replacing a binary
alone does not update an already running process.

## Testing

From a source checkout, check an installed prefix without starting daemons:

```sh
python3 testsuite/macos/install-smoke.py --prefix /opt/slurm \
  --report ./install-smoke.json
```

`testsuite/macos/job-smoke.py` runs a temporary same-user Slurm cluster and
MUNGE instance, retaining logs and state for diagnosis. `privileged-smoke.py`
uses an existing nonroot worker account to test identity changes, credentials,
file ownership and cross-user job authorization. Both require the local
hostname's first IPv4 address to resolve to loopback; they refuse to run
otherwise and do not change network settings. Read each script's `--help`
before running it. Neither configures accounts, services or PAM.

To test the optional footprint and GPU connection policies through Slurm,
build the probes and provide a previously built `metal-smoke`:

```sh
clang -fobjc-arc -dynamiclib -framework Foundation -framework Metal \
  testsuite/macos/gpu-loader-probe.m -o ./gpu-loader-probe.dylib
clang testsuite/macos/gpu-loader-host.c -o ./gpu-loader-host
clang testsuite/macos/footprint-workload.c -o ./footprint-workload
python3 testsuite/macos/job-smoke.py --prefix /opt/slurm \
  --munge-bin /path/to/munge-tools \
  --native-enforcement --task-plugin darwin --proctrack-plugin darwin \
  --metal-workload /path/to/metal-smoke \
  --footprint-workload ./footprint-workload \
  --gpu-loader-probe ./gpu-loader-probe.dylib \
  --gpu-loader-host ./gpu-loader-host
```

The loader probe attempts Metal work in a dynamic-library constructor, checking
that the policy applies before workload initialization. Its `DYLD_*` settings
belong only to the workload, never the Slurm daemons. This test requires working
Metal hardware. See [resource-limits.md](resource-limits.md) and
[gpu-access.md](gpu-access.md) for the policies' scope.

`privileged-smoke.py` requires root and an explicitly selected worker account.
Review the selected binaries, account and scratch paths first. Its `--self-check`
mode validates inputs and exits 77 without starting daemons or changing identity.
For `--native-enforcement`, supply `--footprint-workload` from the command above
and `--metal-worker` pointing to the installed
`libexec/slurm/slurm-darwin-metal-probe`. The harness checks footprint termination
and reset on later exec, and compares allocated and unallocated GPU access.

Portability CI builds and runs selected tests on Linux and macOS. The standalone
Metal test skips when no device is available. Same-user and hosted CI runs do not
exercise production privileged launches, PAM sessions, cross-host authentication
or sleep/reboot recovery; test those on the intended deployment.
