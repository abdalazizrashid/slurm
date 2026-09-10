#!/usr/bin/env python3
"""Opt-in identity/permission checks on a private native Slurm cluster.

Requires root and an explicitly selected existing nonroot worker account. Never
creates accounts, installs services, or changes system configuration. All jobs,
keys, sockets, configuration and daemon process groups belong to this invocation.
--self-check validates inputs only and exits 77 without privileged execution.
Distributed under Slurm's GNU GPL version 2 or later.
"""

import argparse
import ipaddress
import json
import os
import pwd
import re
import selectors
import shlex
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path

PROBE = r"""import json, os, resource, sys
from pathlib import Path
result, forbidden = map(Path, sys.argv[1:])
try:
    forbidden.read_bytes()
except PermissionError:
    forbidden_denied = True
else:
    forbidden_denied = False
regain_denied = {}
for operation in ("setuid", "setgid"):
    try:
        getattr(os, operation)(0)
    except PermissionError:
        regain_denied[operation] = True
    else:
        regain_denied[operation] = False
os.umask(0o077)
created = Path.cwd() / (result.stem + ".created")
with created.open("x") as stream:
    stream.write("written by the Slurm task\n")
record = {
    "uid": os.getuid(), "euid": os.geteuid(),
    "gid": os.getgid(), "egid": os.getegid(),
    "groups": sorted(set(os.getgroups())), "cwd": str(Path.cwd()),
    "root_file_denied": forbidden_denied,
    "root_regain_denied": regain_denied,
    "cpu_limit": list(resource.getrlimit(resource.RLIMIT_CPU)),
    "job_id": os.environ.get("SLURM_JOB_ID"),
}
with result.open("x") as stream:
    json.dump(record, stream)
print("PRIVILEGED_SMOKE_JOB=" + str(record["job_id"]), flush=True)
print("PRIVILEGED_SMOKE_STDERR", file=sys.stderr, flush=True)
"""


CANARY = r"""import json, os, select, signal, sys, time
watchdog = int(sys.argv[1]) if len(sys.argv) > 1 else 720
interval = float(sys.argv[2]) if len(sys.argv) > 2 else 0.1
signal.alarm(watchdog)
print(json.dumps({"ready": True, "pid": os.getpid(), "uid": os.getuid(),
                  "gid": os.getgid()}), flush=True)
# An inherited anonymous pipe is the only control input. No job can open it by
# pathname; EOF requests normal exit. The watchdog bounds an abandoned helper.
end = time.monotonic() + watchdog
count = 0
while time.monotonic() < end:
    readable, _, _ = select.select([sys.stdin], [], [], interval)
    if readable:
        if sys.stdin.read(1) == "":
            raise SystemExit(0)
        raise SystemExit(2)
    count += 1
    print(json.dumps({"heartbeat": count}), flush=True)
raise SystemExit(3)
"""

ACCOUNTING_PROBE = r"""import time
memory = bytearray(32 * 1024 * 1024)
for index in range(0, len(memory), 4096):
    memory[index] = 1
end = time.process_time() + 1.2
while time.process_time() < end:
    sum(range(2000))
print("ACCOUNTING_READY", flush=True)
time.sleep(100)
"""


EPILOG_PROBE = r"""import json, os, resource, subprocess, sys
from pathlib import Path
metal, expectation, destination = sys.argv[1:]
result = subprocess.run([metal, expectation], capture_output=True, text=True,
                        timeout=4)
record = {
    "uid": os.getuid(), "euid": os.geteuid(),
    "gid": os.getgid(), "egid": os.getegid(),
    "groups": sorted(set(os.getgroups())), "cwd": str(Path.cwd()),
    "context": os.environ.get("SLURM_SCRIPT_CONTEXT"),
    "job_id": os.environ.get("SLURM_JOB_ID"),
    "cpu_limit": list(resource.getrlimit(resource.RLIMIT_CPU)),
    "expectation": expectation, "returncode": result.returncode,
    "stdout": result.stdout, "stderr": result.stderr,
}
os.umask(0o077)
with Path(destination).open("x") as stream:
    json.dump(record, stream)
raise SystemExit(result.returncode)
"""


JOB_CANARY = r"""import json, os, signal, time
from pathlib import Path
signal.alarm(100)
for count in range(1000):
    candidate = Path("job-canary-heartbeat.new")
    with candidate.open("w") as stream:
        json.dump({"count": count, "uid": os.getuid(), "pid": os.getpid()}, stream)
    candidate.replace("job-canary-heartbeat.json")
    time.sleep(0.1)
"""


class CanaryMonitor:
    """Require fresh heartbeats from the unrelated worker process."""

    def __init__(self, process, selector, uid, gid):
        self.process = process
        self.selector = selector
        self.uid = uid
        self.gid = gid
        self.buffer = b""
        self.heartbeat = 0

    def _alive(self, record):
        # A pipe may retain a final heartbeat after its writer has died. The
        # canary has no children; poll may safely reap it, and cleanup will never
        # signal that reaped PID or process group.
        if self.process.poll() is not None:
            raise RuntimeError("Unrelated worker canary is no longer alive")
        return record

    def progress(self, *, ready=False, timeout=5):
        deadline = time.monotonic() + timeout
        previous = self.heartbeat
        while time.monotonic() < deadline:
            for key, _ in self.selector.select(timeout=0.2):
                chunk = os.read(key.fd, 4096)
                if not chunk:
                    raise RuntimeError("Unrelated worker canary exited unexpectedly")
                self.buffer += chunk
                if len(self.buffer) > 65536:
                    raise RuntimeError("Unexpected canary output size")
                while b"\n" in self.buffer:
                    line, self.buffer = self.buffer.split(b"\n", 1)
                    record = json.loads(line)
                    if record.get("ready") is True:
                        if record != {
                            "ready": True,
                            "pid": self.process.pid,
                            "uid": self.uid,
                            "gid": self.gid,
                        }:
                            raise RuntimeError(
                                "Unrelated worker canary identity mismatch"
                            )
                        if ready:
                            return self._alive(record)
                    elif type(record.get("heartbeat")) is int:
                        if record["heartbeat"] <= self.heartbeat:
                            raise RuntimeError(
                                "Unrelated worker canary counter went backwards"
                            )
                        self.heartbeat = record["heartbeat"]
                    else:
                        raise RuntimeError("Invalid unrelated worker canary record")
            # Drain the pipe first, then require an additional observation so a
            # backlog accumulated during a job cannot impersonate current life.
            if self.heartbeat > previous:
                if not ready:
                    previous = self.heartbeat
                    if not self.selector.select(timeout=0):
                        break
        else:
            raise RuntimeError("Unrelated worker canary did not advance")
        while time.monotonic() < deadline:
            if self.selector.select(timeout=0.2):
                chunk = os.read(self.process.stdout.fileno(), 4096)
                if not chunk:
                    raise RuntimeError("Unrelated worker canary exited unexpectedly")
                self.buffer += chunk
                while b"\n" in self.buffer:
                    line, self.buffer = self.buffer.split(b"\n", 1)
                    record = json.loads(line)
                    count = record.get("heartbeat")
                    if type(count) is not int or count <= self.heartbeat:
                        raise RuntimeError("Invalid current canary heartbeat")
                    self.heartbeat = count
                if self.heartbeat > previous:
                    return self._alive(
                        {"pid": self.process.pid, "heartbeat": self.heartbeat}
                    )
        raise RuntimeError("Unrelated worker canary lacks a current heartbeat")


def read_worker_file(path, uid, *, read=False):
    # A concurrent worker process must not redirect root's diagnostic reads.
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        status = os.fstat(fd)
        if not stat.S_ISREG(status.st_mode) or status.st_uid != uid:
            raise RuntimeError(f"Unexpected owner/type for worker output {path}")
        if status.st_size > 1024 * 1024:
            raise RuntimeError(f"Unexpectedly large worker output {path}")
        text = os.read(fd, 1024 * 1024).decode() if read else None
        return status, text
    finally:
        os.close(fd)


def verify_epilog_observation(record, uid, gid, groups, work, expectation):
    if (
        (record.get("uid"), record.get("euid")) != (uid, uid)
        or (record.get("gid"), record.get("egid")) != (gid, gid)
        or set(record.get("groups", [])) | {gid} != set(groups) | {gid}
        or record.get("cwd") != str(work)
        or record.get("context") != "epilog_task"
        or not str(record.get("job_id", "")).isdigit()
        or record.get("cpu_limit") != [30, 30]
        or record.get("expectation") != expectation
        or record.get("returncode") != 0
    ):
        raise ValueError(f"Task epilog identity or policy failed: {record}")
    marker = (
        "denied/unavailable" if expectation == "--expect-denied" else "compute=PASS"
    )
    if marker not in record.get("stdout", ""):
        raise ValueError(f"Task epilog lacks the Metal observation: {record}")


def stop_group(process):
    # An unreaped child pins its PID. Never signal a numeric process group
    # after wait()/poll()/communicate() may have released that identity.
    status = process.returncode
    timed_out = False
    if status is None:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            status = process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            timed_out = True
            if process.returncode is None:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                status = process.wait(timeout=3)
            else:
                status = process.returncode
    failure = None
    if timed_out:
        failure = f"Process group {process.pid} shutdown timed out after SIGTERM"
    elif status not in (0, -signal.SIGTERM):
        failure = f"Process group {process.pid} shutdown exited {status}"
    # Signal 0 is only an existence probe. A surviving/reused group after
    # reaping cannot be safely killed; report incomplete cleanup instead.
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        try:
            os.killpg(process.pid, 0)
        except ProcessLookupError:
            if failure:
                raise RuntimeError(failure)
            return
        time.sleep(0.1)
    raise RuntimeError(
        f"Process group {process.pid} remains after its leader was reaped; "
        "refusing to signal an unverified identity"
    )


def parse_accounting_sample(text, job):
    """Accept one named live sample with positive memory and CPU accounting."""
    lines = [line for line in text.splitlines() if line.strip()]
    if len(lines) != 1:
        raise ValueError("Expected exactly one live accounting row")
    fields = lines[0].split("|")
    if len(fields) != 3 or fields[0] != job + ".batch":
        raise ValueError("Unexpected live accounting identity or columns")
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)?", fields[1]) or float(fields[1]) <= 0:
        raise ValueError("Missing positive native RSS sample")
    match = re.fullmatch(r"(?:(\d+)-)?(\d+):(\d{2}):(\d{2})(?:\.(\d+))?", fields[2])
    if not match:
        raise ValueError("Invalid native CPU time")
    days, hours, minutes, seconds, fraction = match.groups()
    if int(minutes) >= 60 or int(seconds) >= 60:
        raise ValueError("Invalid native CPU time range")
    cpu = int(days or 0) * 86400 + int(hours) * 3600 + int(minutes) * 60 + int(seconds)
    cpu += float("0." + fraction) if fraction else 0
    if cpu <= 0:
        raise ValueError("Missing positive native CPU sample")
    return {
        "job_id": fields[0],
        "max_rss_bytes": float(fields[1]),
        "average_cpu_seconds": cpu,
    }


def configuration(
    runtime,
    prefix,
    host,
    node,
    ctld_port,
    node_port,
    cpu_count,
    proctrack="darwin",
    native_enforcement=False,
):
    return (
        f"ClusterName={node}\nSlurmctldHost={host}(127.0.0.1)\n"
        f"SlurmctldAddr=127.0.0.1\nSlurmctldPort={ctld_port}\n"
        f"SlurmdPort={node_port}\nSlurmUser=root\nSlurmdUser=root\n"
        f"AuthType=auth/munge\nCredType=cred/munge\n"
        f"AuthInfo=socket={runtime / 'munge.sock'}\n"
        f"PluginDir={prefix / 'lib/slurm'}\n"
        f"SlurmctldPidFile={runtime / 'slurmctld.pid'}\n"
        f"SlurmdPidFile={runtime / 'slurmd.pid'}\n"
        f"StateSaveLocation={runtime / 'state'}\n"
        f"SlurmdSpoolDir={runtime / 'spool'}\n"
        "CommunicationParameters=NoCtldInAddrAny,NoInAddrAny\n"
        f"ProctrackType=proctrack/{proctrack}\nTaskPlugin=task/darwin\n"
        "DefRuntimePlugin=none\n"
        + ("GresTypes=gpu\n" if native_enforcement else "")
        + "NamespaceType=namespace/none\nJobAcctGatherType=jobacct_gather/darwin\n"
        "JobAcctGatherFrequency=task=1\nSelectType=select/cons_tres\n"
        "SelectTypeParameters=CR_CPU_Memory\nSchedulerType=sched/backfill\n"
        "ReturnToService=2\nSlurmdTimeout=30\nMessageTimeout=5\n"
        "KillWait=2\nMinJobAge=300\nSlurmctldDebug=debug\nSlurmdDebug=debug\n"
        f"NodeName={node} NodeHostname={host} NodeAddr=127.0.0.1 "
        f"CPUs={cpu_count} RealMemory=1024 State=UNKNOWN"
        + (" Gres=gpu:1" if native_enforcement else "")
        + "\n"
        f"PartitionName=test Nodes={node} Default=YES MaxTime=2 State=UP\n"
    )


def validate(args):
    try:
        worker = pwd.getpwnam(args.job_user)
    except KeyError as error:
        raise ValueError("--job-user must name an existing account") from error
    if worker.pw_uid in (0, 65534, 4294967294) or worker.pw_name == "nobody":
        raise ValueError("--job-user must be a nonroot account other than nobody")
    prefix, munge_bin = args.prefix.resolve(), args.munge_bin.resolve()
    for directory, names in (
        (prefix / "sbin", ("slurmctld", "slurmd", "slurmstepd")),
        (
            prefix / "bin",
            ("srun", "sbatch", "scontrol", "sinfo", "squeue", "scancel", "sstat"),
        ),
        (munge_bin, ("munged", "munge", "unmunge")),
    ):
        for name in names:
            path = directory / name
            if not path.is_file() or not os.access(path, os.X_OK):
                raise ValueError(f"Missing executable: {path}")
    for name in (
        "auth_munge",
        "cred_munge",
        "proctrack_" + args.proctrack_plugin,
        "task_darwin",
        "jobacct_gather_darwin",
    ):
        if not (prefix / "lib/slurm" / (name + ".so")).is_file():
            raise ValueError(f"Missing required plugin: {name}")
    if args.native_enforcement:
        for option in ("footprint_workload", "metal_worker"):
            path = getattr(args, option)
            if not path or not path.is_file() or not os.access(path, os.X_OK):
                raise ValueError(
                    f"--native-enforcement requires executable --{option.replace('_', '-')}"
                )
        if not (prefix / "lib/slurm/gpu_metal.so").is_file():
            raise ValueError("--native-enforcement requires the Metal plugin")
    for path in (prefix, args.runtime_parent.resolve()):
        if any(char.isspace() or char in "#=" for char in str(path)):
            raise ValueError("Configuration paths must not contain whitespace, # or =")
    if args.report and (args.report.exists() or args.report.is_symlink()):
        raise ValueError(
            "--report must be a new file; existing paths are never overwritten"
        )
    groups = sorted(set(os.getgrouplist(worker.pw_name, worker.pw_gid)))
    if not shutil.rmtree.avoids_symlink_attacks:
        raise ValueError("Python must provide descriptor-based safe directory cleanup")
    if not os.access("/usr/sbin/lsof", os.X_OK):
        raise ValueError("lsof is required to verify actual daemon listener addresses")
    loopback_preflight()
    return worker, prefix, munge_bin, groups


def loopback_preflight():
    # NoInAddrAny uses the local hostname, not NodeAddr/SlurmctldAddr.
    addresses = socket.getaddrinfo(
        socket.gethostname(), None, socket.AF_INET, socket.SOCK_STREAM
    )
    if not addresses or not ipaddress.ip_address(addresses[0][4][0]).is_loopback:
        raise ValueError("NoInAddrAny would bind a non-loopback hostname address")


def drain_private_jobs(run, prefix, node, jobs, timeout=10):
    """Cancel only recorded/named jobs and require a successful empty query."""
    deadline = time.monotonic() + timeout
    cancelled, attempts = set(), []
    while True:
        query_ok, active = False, set()
        try:
            queued = run(
                [prefix / "bin/squeue", "--noheader", "--format=%i|%j"],
                timeout=5,
                check=False,
            )
            if queued.returncode:
                raise RuntimeError(
                    f"Cleanup queue query exited {queued.returncode}: {queued.stderr}"
                )
            for line in queued.stdout.splitlines():
                job, separator, name = line.partition("|")
                if not separator or not job.strip().isdigit():
                    raise RuntimeError(f"Invalid cleanup queue row: {line!r}")
                job = job.strip()
                if job not in jobs and not name.startswith(node + "-"):
                    raise RuntimeError(f"Unrecognized job in private queue: {line!r}")
                active.add(job)
            jobs.update(active)
            query_ok = True
        except Exception as error:
            attempts.append({"queue_error": str(error)})
        if query_ok and not active:
            return attempts
        pending = (active if query_ok else jobs) - cancelled
        if pending:
            for controller_only in (False, True):
                command = [prefix / "bin/scancel"]
                if controller_only:
                    command.append("--ctld")
                try:
                    result = run([*command, *sorted(pending)], timeout=8, check=False)
                    attempts.append(
                        {
                            "cancel_ctld": controller_only,
                            "returncode": result.returncode,
                            "stderr": result.stderr,
                        }
                    )
                    if not result.returncode:
                        cancelled.update(pending)
                        break
                except Exception as error:
                    attempts.append(
                        {"cancel_ctld": controller_only, "error": str(error)}
                    )
            else:
                raise RuntimeError(f"Private job cancellation failed: {attempts}")
        if time.monotonic() >= deadline:
            raise RuntimeError(f"Private queue did not verifiably drain: {attempts}")
        time.sleep(0.2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--job-user", required=True)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument(
        "--munge-bin",
        type=Path,
        required=True,
        help="Directory containing munged, munge and unmunge",
    )
    parser.add_argument("--runtime-parent", type=Path, default=Path("/tmp"))
    parser.add_argument("--proctrack-plugin", choices=("darwin",), default="darwin")
    parser.add_argument("--native-enforcement", action="store_true")
    parser.add_argument("--footprint-workload", type=Path)
    parser.add_argument(
        "--metal-worker",
        type=Path,
        help="Compiled expectation-only slurm-darwin-metal-probe",
    )
    parser.add_argument("--report", type=Path)
    parser.add_argument(
        "--keep-runtime",
        action="store_true",
        help="Retain private logs/state/output, never the MUNGE key or seed",
    )
    parser.add_argument("--self-check", action="store_true")
    args = parser.parse_args()
    if not __debug__:
        parser.error("run the validation harness without Python -O")
    try:
        worker, prefix, munge_bin, groups = validate(args)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    if args.self_check:
        rendered = configuration(
            Path("/tmp/self-check"),
            prefix,
            "localhost",
            "self-check",
            19001,
            19002,
            1,
            args.proctrack_plugin,
            args.native_enforcement,
        )
        assert "SlurmUser=root\nSlurmdUser=root\n" in rendered
        assert "AuthType=auth/munge\n" in rendered
        compile(PROBE, "job-probe", "exec")
        compile(EPILOG_PROBE, "epilog-probe", "exec")
        print(
            json.dumps(
                {
                    "status": "SKIP",
                    "inputs_validated": True,
                    "privileged_execution": False,
                    "job_uid": worker.pw_uid,
                    "job_gid": worker.pw_gid,
                    "expected_groups": groups,
                }
            )
        )
        return 77
    if os.geteuid() != 0:
        parser.error(
            "privileged execution requires effective UID 0; use --self-check to skip"
        )
    if sys.platform != "darwin":
        parser.error("this privileged harness requires macOS")
    if signal.getsignal(signal.SIGCHLD) != signal.SIG_DFL:
        parser.error(
            "the harness requires default SIGCHLD handling to retain child identities"
        )

    runtime = Path(
        tempfile.mkdtemp(prefix="slurm-priv-", dir=args.runtime_parent)
    ).resolve()
    runtime.chmod(0o711)  # MUNGE clients and the job may traverse known paths.
    work = runtime / "work"
    node = runtime.name
    host = socket.gethostname().split(".")[0]
    results = {
        "success": False,
        "privileged_execution": True,
        "runtime": str(runtime),
        "worker": worker.pw_name,
        "uid": worker.pw_uid,
        "gid": worker.pw_gid,
        "expected_groups": groups,
        "checks": [],
        "cleanup_errors": [],
        "not_covered": [
            *([] if args.native_enforcement else ["footprint and GPU controls"]),
            "privileged remote CPU-rate API",
            "kernel-enforced descendant containment",
            "cross-host identity mapping",
            "PAM sessions",
            "escaped hostile descendants",
        ],
    }
    daemons, log_streams, jobs = [], [], set()
    canary = None
    canary_selector = selectors.DefaultSelector()
    canary_monitor = None
    env = {
        "PATH": f"{prefix / 'bin'}:/usr/bin:/bin:/usr/sbin:/sbin",
        "LANG": "C",
        "LC_ALL": "C",
        "SLURM_CONF": str(runtime / "slurm.conf"),
    }

    def interrupted(number, frame):
        raise RuntimeError(f"Privileged harness interrupted by signal {number}")

    previous_term = signal.signal(signal.SIGTERM, interrupted)
    previous_alarm = signal.signal(signal.SIGALRM, interrupted)
    # Bound the experiment separately from cleanup. The outside helper has a
    # longer independent watchdog, including when its output pipe fills.
    signal.alarm(450)

    def event(name, **values):
        record = {"check": name, **values}
        results["checks"].append(record)
        print(json.dumps(record), flush=True)

    def run(argv, *, as_worker=False, check=True, timeout=25, input=None):
        child_env = dict(env)
        identity = {}
        if as_worker:
            identity = dict(
                user=worker.pw_uid, group=worker.pw_gid, extra_groups=groups
            )
            child_env.update(
                HOME=worker.pw_dir, USER=worker.pw_name, LOGNAME=worker.pw_name
            )
        process = subprocess.Popen(
            [str(arg) for arg in argv],
            env=child_env,
            cwd=work if as_worker else runtime,
            text=True,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=True,
            **identity,
        )
        try:
            stdout, stderr = process.communicate(input=input, timeout=timeout)
        except BaseException:
            stop_group(process)
            raise
        result = subprocess.CompletedProcess(argv, process.returncode, stdout, stderr)
        if check and result.returncode:
            raise RuntimeError(
                f"{Path(argv[0]).name} exited {result.returncode}: {stdout} {stderr}"
            )
        return result

    def launch(name, argv):
        log = (runtime / (name + ".log")).open("x")
        os.chmod(log.name, 0o600)
        log_streams.append(log)
        process = subprocess.Popen(
            [str(arg) for arg in argv],
            env=env,
            cwd=runtime,
            stdout=log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        daemons.append((name, process))
        return process

    def loopback_listener(process, expected_port):
        output = run(
            [
                "/usr/sbin/lsof",
                "-nP",
                "-a",
                "-p",
                str(process.pid),
                "-iTCP",
                "-sTCP:LISTEN",
                "-Fn",
            ],
            check=False,
        )
        if output.returncode not in (0, 1):
            raise RuntimeError(f"Cannot inspect owned listener: {output.stderr}")
        endpoints = [
            line[1:] for line in output.stdout.splitlines() if line.startswith("n")
        ]
        for endpoint in endpoints:
            address, _, port = endpoint.rpartition(":")
            if not ipaddress.ip_address(address.strip("[]")).is_loopback:
                raise RuntimeError(
                    f"Owned daemon opened non-loopback listener: {endpoint}"
                )
            if int(port) != expected_port:
                raise RuntimeError(
                    f"Owned daemon opened unexpected listener: {endpoint}"
                )
        return bool(endpoints)

    def wait_for(predicate, description, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for name, process in daemons:
                if process.poll() is not None:
                    raise RuntimeError(
                        f"{name} exited {process.returncode}: "
                        + (runtime / (name + ".log")).read_text()[-8000:]
                    )
            if predicate():
                return
            time.sleep(0.2)
        raise RuntimeError(f"Timeout waiting for {description}")

    def state(job):
        text = run([prefix / "bin/scontrol", "show", "job", job, "-o"]).stdout
        fields = dict(token.split("=", 1) for token in text.split() if "=" in token)
        return fields.get("JobState", "UNKNOWN"), fields

    def submit(script, *, as_worker, output, job_name, held=False):
        argv = [
            prefix / "bin/sbatch",
            "--parsable",
            "--nodes=1",
            "--ntasks=1",
            "--mem=128",
            "--time=1",
            "--job-name",
            job_name,
            "--chdir",
            work,
            "--output",
            output,
            "--error",
            str(output) + ".err",
        ]
        if held:
            argv.append("--hold")
        result = run(argv, as_worker=as_worker, input=script)
        job = result.stdout.strip().split(";")[0]
        if not job.isdigit():
            raise RuntimeError(f"Invalid submitted job ID: {result.stdout}")
        jobs.add(job)
        return job

    def worker_file(name, *, read=False):
        return read_worker_file(work / name, worker.pw_uid, read=read)

    def verify_job(kind):
        record = json.loads(worker_file(kind + ".json", read=True)[1])
        assert (record["uid"], record["euid"]) == (worker.pw_uid, worker.pw_uid), record
        assert (record["gid"], record["egid"]) == (worker.pw_gid, worker.pw_gid), record
        assert set(record["groups"]) | {worker.pw_gid} == set(groups) | {
            worker.pw_gid
        }, record
        assert record["cwd"] == str(work), record
        assert record["root_file_denied"], record
        assert record["root_regain_denied"] == {"setuid": True, "setgid": True}, record
        assert record["cpu_limit"] == [30, 30], record
        assert str(record["job_id"]).isdigit(), record
        for name in (
            kind + ".json",
            kind + ".created",
            kind + ".out",
            kind + ".out.err",
        ):
            status, _ = worker_file(name)
            assert (status.st_uid, status.st_gid) == (
                worker.pw_uid,
                worker.pw_gid,
            ), name
        assert worker_file(kind + ".created")[0].st_mode & 0o777 == 0o600
        assert "PRIVILEGED_SMOKE_JOB=" in worker_file(kind + ".out", read=True)[1]
        assert "PRIVILEGED_SMOKE_STDERR" in worker_file(kind + ".out.err", read=True)[1]
        jobs.add(str(record["job_id"]))
        event(kind + "_identity_permissions", passed=True, observed=record)

    try:
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", host):
            raise RuntimeError(
                "Hostname cannot be represented safely in test configuration"
            )
        work.mkdir(mode=0o700)
        os.chown(work, worker.pw_uid, worker.pw_gid)
        canary = subprocess.Popen(
            [sys.executable, "-I", "-c", CANARY],
            env={"PATH": "/usr/bin:/bin", "LANG": "C", "LC_ALL": "C"},
            cwd="/",
            user=worker.pw_uid,
            group=worker.pw_gid,
            extra_groups=groups,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        canary_selector.register(canary.stdout, selectors.EVENT_READ)
        canary_monitor = CanaryMonitor(
            canary, canary_selector, worker.pw_uid, worker.pw_gid
        )
        event(
            "unrelated_worker_canary_started",
            passed=True,
            observed=canary_monitor.progress(ready=True),
        )
        for name in ("state", "spool", "root-only"):
            (runtime / name).mkdir(mode=0o700)
        forbidden = runtime / "root-only/canary"
        forbidden.write_text("only this invocation's root process may read this file\n")
        forbidden.chmod(0o600)
        probe = runtime / "job-probe.py"
        probe.write_text(PROBE)
        probe.chmod(0o644)
        key = runtime / "munge.key"
        fd = os.open(key, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, "wb") as stream:
            stream.write(os.urandom(128))
        # Reserve both candidate ports together; a later bind conflict fails startup.
        with socket.socket() as controller_socket, socket.socket() as node_socket:
            controller_socket.bind(("127.0.0.1", 0))
            node_socket.bind(("127.0.0.1", 0))
            ports = controller_socket.getsockname()[1], node_socket.getsockname()[1]
        config = runtime / "slurm.conf"
        config.write_text(
            configuration(
                runtime,
                prefix,
                host,
                node,
                *ports,
                os.cpu_count() or 1,
                args.proctrack_plugin,
                args.native_enforcement,
            )
        )
        config.chmod(0o644)  # No secrets: the worker's clients must read configuration.
        policy = "PerProcessCPUTimeSeconds=30\n"
        if args.native_enforcement:
            policy += (
                "InitialTaskImageFootprintMiB=256\nDenyUnallocatedGPUConnections=yes\n"
            )
            (runtime / "gres.conf").write_text("AutoDetect=metal\n")
            (runtime / "gres.conf").chmod(0o644)
        (runtime / "darwin.conf").write_text(policy)
        (runtime / "darwin.conf").chmod(0o644)
        launch(
            "munged",
            [
                munge_bin / "munged",
                "--foreground",
                "--socket",
                runtime / "munge.sock",
                "--key-file",
                key,
                "--pid-file",
                runtime / "munged.pid",
                "--seed-file",
                runtime / "munge.seed",
                "--log-file",
                runtime / "munged-daemon.log",
                "--origin",
                "127.0.0.1",
            ],
        )
        wait_for(lambda: (runtime / "munge.sock").exists(), "private MUNGE socket")
        token = run(
            [munge_bin / "munge", "--socket", runtime / "munge.sock", "-n"],
            as_worker=True,
        ).stdout
        decoded = run(
            [munge_bin / "unmunge", "--socket", runtime / "munge.sock"], input=token
        )
        if "SUCCESS" not in decoded.stdout.upper():
            raise RuntimeError("Worker MUNGE credential did not decode successfully")
        match = re.search(r"^UID:\s+.*\((\d+)\)", decoded.stdout, re.MULTILINE)
        if not match or int(match.group(1)) != worker.pw_uid:
            raise RuntimeError("Worker MUNGE credential UID mismatch")
        event("worker_authentication", passed=True)
        loopback_preflight()
        controller = launch(
            "slurmctld", [prefix / "sbin/slurmctld", "-D", "-f", config]
        )
        wait_for(
            lambda: loopback_listener(controller, ports[0]),
            "loopback controller listener",
            timeout=60,
        )
        wait_for(
            lambda: run(
                [prefix / "bin/scontrol", "ping"], check=False, timeout=8
            ).returncode
            == 0,
            "private controller",
            timeout=60,
        )
        loopback_preflight()
        node_process = launch(
            "slurmd", [prefix / "sbin/slurmd", "-D", "-N", node, "-f", config]
        )
        wait_for(
            lambda: loopback_listener(node_process, ports[1]), "loopback node listener"
        )
        wait_for(
            lambda: "idle" in run([prefix / "bin/sinfo", "-h", "-o", "%t"]).stdout,
            "registered native compute node",
        )
        event("node_registration", passed=True)

        command = [
            prefix / "bin/srun",
            "--nodes=1",
            "--ntasks=1",
            "--mem=128",
            "--time=1",
            "--cpu-bind=none",
            "--job-name",
            node + "-srun",
            "--chdir",
            work,
            "--output",
            work / "srun.out",
            "--error",
            work / "srun.out.err",
            sys.executable,
            probe,
            work / "srun.json",
            forbidden,
        ]
        run(command, as_worker=True, timeout=60)
        verify_job("srun")
        script = (
            "#!/bin/sh\nexec "
            + shlex.join(
                [sys.executable, str(probe), str(work / "sbatch.json"), str(forbidden)]
            )
            + "\n"
        )
        job = submit(
            script,
            as_worker=True,
            output=work / "sbatch.out",
            job_name=node + "-sbatch",
        )
        wait_for(
            lambda: state(job)[0] in ("COMPLETED", "FAILED", "CANCELLED", "NODE_FAIL"),
            "worker batch completion",
            timeout=60,
        )
        assert state(job)[0] == "COMPLETED", state(job)
        verify_job("sbatch")
        event(
            "unrelated_worker_canary_after_jobs",
            passed=True,
            observed=canary_monitor.progress(),
        )

        # Native accounting runs across the root stepd -> nonroot task boundary.
        # A second Slurm job and an outside worker process remain alive while
        # cancellation targets only the sampled job.
        accounting_probe = runtime / "accounting-probe.py"
        accounting_probe.write_text(ACCOUNTING_PROBE)
        accounting_probe.chmod(0o644)
        sampled_job = submit(
            "#!/bin/sh\nexec "
            + shlex.join([sys.executable, "-I", str(accounting_probe)])
            + "\n",
            as_worker=True,
            output=work / "accounting.out",
            job_name=node + "-accounting",
        )
        job_canary_probe = runtime / "job-canary.py"
        job_canary_probe.write_text(JOB_CANARY)
        job_canary_probe.chmod(0o644)
        canary_job = submit(
            "#!/bin/sh\nexec "
            + shlex.join([sys.executable, "-I", str(job_canary_probe)])
            + "\n",
            as_worker=True,
            output=work / "job-canary.out",
            job_name=node + "-job-canary",
        )
        wait_for(
            lambda: all(
                state(job)[0] == "RUNNING" for job in (sampled_job, canary_job)
            ),
            "two concurrent nonroot batch jobs",
            timeout=30,
        )
        event("concurrent_nonroot_jobs", passed=True, job_ids=[sampled_job, canary_job])

        def job_canary_record():
            record = json.loads(worker_file("job-canary-heartbeat.json", read=True)[1])
            if (
                set(record) != {"count", "uid", "pid"}
                or record["uid"] != worker.pw_uid
                or type(record["count"]) is not int
                or record["count"] < 0
                or type(record["pid"]) is not int
                or record["pid"] <= 1
            ):
                raise RuntimeError("Invalid surviving Slurm job canary heartbeat")
            return record

        wait_for(
            lambda: (work / "job-canary-heartbeat.json").exists(),
            "Slurm job canary heartbeat",
        )
        samples = []

        def live_accounting():
            sample = run(
                [
                    prefix / "bin/sstat",
                    "--noheader",
                    "--parsable2",
                    "--noconvert",
                    "--jobs",
                    sampled_job + ".batch",
                    "--format=JobID,MaxRSS,AveCPU",
                ],
                as_worker=True,
                timeout=8,
            )
            try:
                record = parse_accounting_sample(sample.stdout, sampled_job)
            except ValueError:
                return False
            samples.append(record)
            return True

        wait_for(
            live_accounting, "positive native memory and CPU accounting", timeout=25
        )
        event("native_live_accounting", passed=True, observed=samples[-1])
        run([prefix / "bin/scancel", sampled_job], as_worker=True)
        wait_for(
            lambda: state(sampled_job)[0].startswith("CANCELLED"),
            "sampled job cancellation",
        )
        assert state(canary_job)[0] == "RUNNING", state(canary_job)
        before_canary = job_canary_record()

        def canary_job_advanced():
            current = job_canary_record()
            if current["pid"] != before_canary["pid"]:
                raise RuntimeError("Slurm canary process identity changed")
            return current["count"] > before_canary["count"]

        wait_for(
            canary_job_advanced,
            "live Slurm canary after other job cancellation",
            timeout=5,
        )
        event(
            "job_cancellation_scope",
            passed=True,
            cancelled_job=sampled_job,
            surviving_job=canary_job,
            canary_before=before_canary,
            canary_after=job_canary_record(),
            unrelated_canary=canary_monitor.progress(),
        )
        run([prefix / "bin/scancel", canary_job], as_worker=True)
        wait_for(
            lambda: state(canary_job)[0].startswith("CANCELLED"),
            "canary job cancellation",
        )

        if args.native_enforcement:
            base = [
                prefix / "bin/srun",
                "--nodes=1",
                "--ntasks=1",
                "--mem=512",
                "--time=1",
                "--cpu-bind=none",
                "--job-name",
                node + "-native",
                "--chdir",
                work,
            ]
            footprint = args.footprint_workload.resolve()
            result = run([*base, footprint], as_worker=True, check=False, timeout=60)
            if (
                result.returncode != 137
                or "allocation-start" not in result.stdout
                or "allocation-complete" in result.stdout
            ):
                raise RuntimeError(
                    f"Initial-image footprint limit did not kill worker: {result}"
                )
            event(
                "worker_initial_image_footprint",
                passed=True,
                uid=worker.pw_uid,
                returncode=result.returncode,
            )
            result = run(
                [*base, "/bin/sh", "-c", "exec " + shlex.quote(str(footprint))],
                as_worker=True,
                timeout=60,
            )
            if "allocation-complete" not in result.stdout:
                raise RuntimeError("Later exec scope was not observed")
            event("worker_later_exec_scope", passed=True)
            metal = args.metal_worker.resolve()

            def epilog_script(name, expectation):
                script = runtime / (name + ".sh")
                script.write_text(
                    "#!/bin/sh\nexec "
                    + shlex.join(
                        [
                            sys.executable,
                            "-I",
                            "-c",
                            EPILOG_PROBE,
                            str(metal),
                            expectation,
                            str(work / (name + ".json")),
                        ]
                    )
                    + "\n"
                )
                script.chmod(0o755)
                return script

            def epilog_observation(name, expectation):
                status, text = worker_file(name + ".json", read=True)
                record = json.loads(text)
                verify_epilog_observation(
                    record, worker.pw_uid, worker.pw_gid, groups, work, expectation
                )
                if stat.S_IMODE(status.st_mode) != 0o600:
                    raise RuntimeError("Task epilog output is not private")
                jobs.add(str(record["job_id"]))
                event(name, passed=True, observed=record)

            denied_epilog = epilog_script(
                "worker_epilog_unallocated_gpu_denied", "--expect-denied"
            )
            result = run(
                [*base, "--task-epilog", denied_epilog, metal, "--expect-denied"],
                as_worker=True,
                timeout=60,
            )
            if "denied/unavailable" not in result.stdout:
                raise RuntimeError("Non-GPU worker did not report connection denial")
            event("worker_unallocated_gpu_denied", passed=True)
            epilog_observation(
                "worker_epilog_unallocated_gpu_denied", "--expect-denied"
            )
            allowed_epilog = epilog_script(
                "worker_epilog_allocated_gpu_computation", "--expect-allowed"
            )
            result = run(
                [
                    *base,
                    "--gres=gpu:1",
                    "--task-epilog",
                    allowed_epilog,
                    metal,
                    "--expect-allowed",
                ],
                as_worker=True,
                timeout=60,
            )
            if "compute=PASS" not in result.stdout:
                raise RuntimeError(
                    "GPU-allocated worker did not verify Metal computation"
                )
            event("worker_allocated_gpu_computation", passed=True)
            epilog_observation(
                "worker_epilog_allocated_gpu_computation", "--expect-allowed"
            )

        # This root-owned job is held for the whole test and never executes.
        held = submit(
            "#!/bin/sh\nexit 99\n",
            as_worker=False,
            held=True,
            output=work / "never-executed.out",
            job_name=node + "-root-held",
        )
        before_state, before_fields = state(held)
        assert (
            before_state == "PENDING" and before_fields.get("Priority") == "0"
        ), before_fields
        assert before_fields.get("UserId", "").endswith("(0)"), before_fields
        attempt = run(
            [
                prefix / "bin/scontrol",
                "update",
                "JobId=" + held,
                "Comment=unauthorized-worker-write",
            ],
            as_worker=True,
            check=False,
        )
        after_state, after_fields = state(held)
        assert attempt.returncode != 0, attempt
        assert re.search(
            r"access/permission denied|not authorized|invalid user id",
            attempt.stderr,
            re.IGNORECASE,
        ), attempt.stderr
        assert (
            after_state == "PENDING" and after_fields.get("Priority") == "0"
        ), after_fields
        assert after_fields.get("Comment") != "unauthorized-worker-write", after_fields
        event(
            "cross_user_job_update_refused",
            passed=True,
            job_id=held,
            returncode=attempt.returncode,
            stderr=attempt.stderr,
        )
        run([prefix / "bin/scancel", held])
        wait_for(
            lambda: state(held)[0].startswith("CANCELLED"), "held job cancellation"
        )
        assert not (work / "never-executed.out").exists()
        results["success"] = True
    except (Exception, KeyboardInterrupt) as error:
        results["error"] = str(error)
        event("failure", error=str(error))
    finally:
        signal.alarm(0)
        # Do not allow a second termination request to interrupt owned cleanup.
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        # Recover IDs from successful queries, including timed-out submissions.
        try:
            attempts = drain_private_jobs(run, prefix, node, jobs)
            event("private_job_cleanup", passed=True, attempts=attempts)
        except Exception as error:
            results["cleanup_errors"].append(str(error))
        for name, process in reversed(daemons):
            try:
                stop_group(process)
            except Exception as error:
                results["cleanup_errors"].append(f"{name}: {error}")
        for stream in log_streams:
            stream.close()
        if canary is not None:
            try:
                event(
                    "unrelated_worker_canary_after_cleanup",
                    passed=True,
                    observed=canary_monitor.progress(),
                )
            except Exception as error:
                results["cleanup_errors"].append(f"Unrelated worker canary: {error}")
            try:
                # Cooperative EOF, followed by bounded own-child group cleanup
                # on failure. Never send a job cancellation signal to the canary.
                canary.stdin.close()
                canary.wait(timeout=5)
                if canary.returncode != 0:
                    raise RuntimeError(f"Unrelated canary exited {canary.returncode}")
                event("unrelated_worker_canary_stopped", passed=True)
            except Exception as error:
                results["cleanup_errors"].append(
                    f"Stopping unrelated worker canary: {error}"
                )
                try:
                    stop_group(canary)
                except Exception as cleanup_error:
                    results["cleanup_errors"].append(str(cleanup_error))
            canary.stdout.close()
        canary_selector.close()
        for name in ("munge.key", "munge.seed"):
            (runtime / name).unlink(missing_ok=True)
        if results["cleanup_errors"]:
            results["success"] = False
        report = json.dumps(results, indent=2) + "\n"
        if args.report:
            try:
                fd = os.open(
                    args.report,
                    os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                    0o600,
                )
                with os.fdopen(fd, "w") as stream:
                    stream.write(report)
            except OSError as error:
                results["success"] = False
                results["cleanup_errors"].append(f"Writing report: {error}")
        fd = os.open(
            runtime / "results.json",
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
            0o600,
        )
        with os.fdopen(fd, "w") as stream:
            stream.write(json.dumps(results, indent=2) + "\n")
        if not args.keep_runtime:
            shutil.rmtree(runtime)
        print(
            json.dumps(
                {
                    "success": results["success"],
                    "runtime_retained": args.keep_runtime,
                    "runtime": str(runtime),
                    "cleanup_errors": results["cleanup_errors"],
                }
            ),
            flush=True,
        )
    signal.signal(signal.SIGTERM, previous_term)
    signal.signal(signal.SIGALRM, previous_alarm)
    return 0 if results["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
