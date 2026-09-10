#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise a temporary, same-user native Slurm cluster with MUNGE.

No services or system configuration are installed. The private runtime directory
and logs are retained for diagnosis; only daemons launched here are terminated.
This tests native job execution, not privileged identity changes or isolation.
"""

import argparse
import errno
import fcntl
import ipaddress
import json
import os
import pty
import pwd
import select
import shlex
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import time
from functools import partial
from pathlib import Path


def record_event(results, name, **values):
    record = {"check": name, **values}
    results["checks"].append(record)
    print(json.dumps(record), flush=True)


def cleanup_failure(results, error):
    message = str(error)
    results["success"] = False
    results.setdefault("cleanup_errors", []).append(message)
    record_event(results, "cleanup_failure", error=message)


def run_pty(argv, *, runtime, env, results, timeout=25, resize=False):
    pid, master = pty.fork()
    if pid == 0:
        try:
            os.chdir(runtime)
            os.execvpe(str(argv[0]), [str(arg) for arg in argv], env)
        except OSError:
            os._exit(127)
    output = bytearray()
    end = time.monotonic() + timeout
    status = None
    resized = False
    try:
        if resize:
            fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
        while time.monotonic() < end:
            if select.select([master], [], [], 0.2)[0]:
                try:
                    chunk = os.read(master, 65536)
                except OSError as error:
                    if error.errno != errno.EIO:
                        raise
                    chunk = b""
                output.extend(chunk)
            if resize and not resized and b"PTY_RESIZE_READY" in output:
                # The kernel signals the owned foreground srun process;
                # the remote task must observe the forwarded dimensions.
                fcntl.ioctl(
                    master, termios.TIOCSWINSZ, struct.pack("HHHH", 47, 109, 0, 0)
                )
                resized = True
            waited, candidate = os.waitpid(pid, os.WNOHANG)
            if waited:
                status = candidate
                break
        if status is None:
            raise RuntimeError("PTY job timed out")
        while select.select([master], [], [], 0)[0]:
            try:
                chunk = os.read(master, 65536)
            except OSError as error:
                if error.errno != errno.EIO:
                    raise
                break
            if not chunk:
                break
            output.extend(chunk)
        text = output.decode(errors="replace")
        if os.waitstatus_to_exitcode(status):
            raise RuntimeError(f"PTY job failed: {text}")
        if resize and not resized:
            raise RuntimeError(f"PTY job never requested resize: {text}")
        return text
    finally:
        try:
            if status is None:
                # Never signal a group after reaping its leader: that
                # numeric PID could then belong to another process.
                waited, _ = os.waitpid(pid, os.WNOHANG)
                if not waited:
                    try:
                        if os.getpgid(pid) != pid:
                            raise RuntimeError("PTY child lost its owned process group")
                        os.killpg(pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    end = time.monotonic() + 5
                    while time.monotonic() < end:
                        if os.waitpid(pid, os.WNOHANG)[0]:
                            break
                        time.sleep(0.05)
                    else:
                        cleanup_failure(results, "PTY child did not exit after SIGKILL")
        except Exception as error:
            cleanup_failure(results, f"PTY cleanup: {type(error).__name__}: {error}")
        finally:
            os.close(master)


def cleanup(runtime, prefix, user, processes, logs, results, run):
    if any(name.startswith("slurmctld") for name, _ in processes):
        cancelled = False
        for controller_only in (False, True):
            argv = [prefix / "bin/scancel"]
            if controller_only:
                argv.append("--ctld")
            argv.extend(["--user", user])
            try:
                cancellation = run(argv, timeout=15, check=False)
                if cancellation.returncode:
                    cleanup_failure(
                        results,
                        f"Cancellation exited {cancellation.returncode}: "
                        f"{cancellation.stderr.strip()}",
                    )
                else:
                    cancelled = True
                    break
            except Exception as error:
                cleanup_failure(
                    results, f"Cancellation: {type(error).__name__}: {error}"
                )
        if not cancelled:
            cleanup_failure(
                results, "Could not request cancellation from the private cluster"
            )
        end = time.monotonic() + 20
        while time.monotonic() < end:
            try:
                queued = run(
                    [prefix / "bin/squeue", "--noheader", "--format=%i"],
                    timeout=5,
                    check=False,
                )
            except Exception as error:
                cleanup_failure(
                    results,
                    f"Cancellation verification: {type(error).__name__}: {error}",
                )
                break
            if queued.returncode:
                cleanup_failure(
                    results,
                    f"Cancellation verification exited {queued.returncode}: "
                    f"{queued.stderr.strip()}",
                )
                break
            if not queued.stdout.strip():
                break
            time.sleep(0.2)
        else:
            cleanup_failure(
                results, "Owned jobs did not finish cancellation before shutdown"
            )
    for name, process in reversed(processes):
        try:
            status = process.poll()
            if status is None:
                # An unreaped direct child keeps its PID from reuse.
                # Validate the session we created before group signalling.
                if os.getpgid(process.pid) != process.pid:
                    raise RuntimeError("Daemon lost its owned process group")
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                try:
                    status = process.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    cleanup_failure(results, f"{name} shutdown timed out after SIGTERM")
                    # The child may have exited at the deadline. A poll that
                    # reaps it also relinquishes ownership of its numeric PID.
                    status = process.poll()
                    if status is None:
                        if os.getpgid(process.pid) != process.pid:
                            raise RuntimeError("Daemon lost its owned process group")
                        try:
                            os.killpg(process.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                        status = process.wait(timeout=5)
            if status not in (0, -signal.SIGTERM):
                cleanup_failure(results, f"{name} shutdown exited {status}")
        except Exception as error:
            cleanup_failure(
                results, f"{name} shutdown: {type(error).__name__}: {error}"
            )
    for log in logs:
        log.close()
    (runtime / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    # Preserve diagnostics but do not retain authentication material.
    for name in ("munge.key", "munge.seed"):
        (runtime / name).unlink(missing_ok=True)
    print(
        json.dumps({"success": results["success"], "runtime": str(runtime)}),
        flush=True,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--munge-bin", type=Path, required=True)
    parser.add_argument("--runtime-parent", type=Path, default=Path("/tmp"))
    parser.add_argument("--task-plugin", choices=("none", "darwin"), default="none")
    parser.add_argument(
        "--native-enforcement",
        action="store_true",
        help="Test initial-image footprint and unallocated GPU denial (task/darwin + Metal required)",
    )
    parser.add_argument("--gpu-loader-probe", type=Path)
    parser.add_argument("--gpu-loader-host", type=Path)
    parser.add_argument("--footprint-workload", type=Path)
    parser.add_argument(
        "--proctrack-plugin", choices=("pgid", "darwin"), default="pgid"
    )
    parser.add_argument(
        "--metal-workload",
        type=Path,
        help="Path to the compiled metal-smoke test executable",
    )
    parser.add_argument(
        "--pmi1-workload",
        type=Path,
        help="Path to the compiled pmi1-smoke test executable",
    )
    parser.add_argument(
        "--pmi2-workload",
        type=Path,
        help="Path to the compiled pmi2-smoke test executable",
    )
    parser.add_argument(
        "--pmix-workload",
        type=Path,
        help="Path to the compiled pmix-smoke test executable",
    )
    parser.add_argument(
        "--extended-lifecycle",
        action="store_true",
        help="Also test detached cleanup, daemon restarts and a one-minute job timeout",
    )
    parser.add_argument(
        "--verbose-network",
        action="store_true",
        help="Include detailed connection-manager/network logs",
    )
    args = parser.parse_args()
    if signal.getsignal(signal.SIGCHLD) != signal.SIG_DFL:
        parser.error("Default SIGCHLD handling is required for owned-process cleanup")
    if bool(args.gpu_loader_probe) != bool(args.gpu_loader_host):
        parser.error(
            "--gpu-loader-probe and --gpu-loader-host must be supplied together"
        )
    if args.gpu_loader_probe and not args.native_enforcement:
        parser.error("GPU loader tests require --native-enforcement")
    if args.native_enforcement and (
        args.task_plugin != "darwin"
        or not args.metal_workload
        or not args.footprint_workload
    ):
        parser.error(
            "--native-enforcement requires --task-plugin=darwin, --metal-workload and --footprint-workload"
        )
    # NoInAddrAny uses the hostname's address, not NodeAddr/SlurmctldAddr.
    # This test enables only IPv4, and refuses a non-loopback default bind.
    resolved = socket.getaddrinfo(
        socket.gethostname(), None, socket.AF_INET, socket.SOCK_STREAM
    )
    if not resolved or not ipaddress.ip_address(resolved[0][4][0]).is_loopback:
        parser.error(
            "private test requires the hostname's first IPv4 address to be loopback"
        )
    prefix = args.prefix.resolve()
    munge_bin = args.munge_bin.resolve()
    runtime = Path(tempfile.mkdtemp(prefix="slurm-mac-", dir=args.runtime_parent))
    # MUNGE requires its socket's directory to be searchable by clients.
    # State, key, configuration and logs remain private to the test user.
    runtime.chmod(0o711)
    results = {"runtime": str(runtime), "checks": [], "success": False}
    processes = []
    logs = []
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("SLURM_", "SBATCH_", "SALLOC_", "SRUN_"))
    }
    env["PATH"] = f"{prefix / 'bin'}:{env.get('PATH', '')}"
    env["SLURM_CONF"] = str(runtime / "slurm.conf")
    user = pwd.getpwuid(os.getuid()).pw_name
    host = socket.gethostname().split(".")[0]
    gpu_config = "GresTypes=gpu\n" if args.metal_workload else ""
    gpu_resources = "Gres=gpu:1 " if args.metal_workload else ""
    network_debug = "DebugFlags=Conmgr,Net\n" if args.verbose_network else ""
    interactive_script = runtime / "interactive-gpu.sh"
    interactive_command = (
        "--interactive --preserve-env --pty --cpu-bind=none /bin/sh "
        + shlex.quote(str(interactive_script))
    )
    # Slurm's quoted configuration values do not support embedded quotes.
    if args.native_enforcement and any(c in interactive_command for c in '"\r\n'):
        parser.error("interactive test path cannot contain quotes or newlines")
    interactive_config = (
        "LaunchParameters=use_interactive_step\n"
        f'InteractiveStepOptions="{interactive_command}"\n'
        if args.native_enforcement
        else ""
    )

    event = partial(record_event, results)

    def run(argv, timeout=25, check=True, **kwargs):
        proc = subprocess.run(
            [str(a) for a in argv],
            env=env,
            cwd=runtime,
            text=True,
            capture_output=True,
            timeout=timeout,
            **kwargs,
        )
        if check and proc.returncode:
            raise RuntimeError(
                f"{argv[0]} exited {proc.returncode}: {proc.stdout} {proc.stderr}"
            )
        return proc

    def launch(name, argv):
        log = (runtime / f"{name}.log").open("w")
        (runtime / f"{name}.log").chmod(0o600)
        logs.append(log)
        process = subprocess.Popen(
            [str(a) for a in argv],
            env=env,
            cwd=runtime,
            stdout=log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        processes.append((name, process))
        return process

    def restart_daemon(name, argv):
        for index, (label, process) in enumerate(processes):
            if label == name:
                process.terminate()
                status = process.wait(timeout=15)
                if status not in (0, -signal.SIGTERM):
                    raise RuntimeError(f"{name} restart shutdown exited {status}")
                processes.pop(index)
                launch(name + "-restarted", argv)
                return
        raise RuntimeError(f"No owned daemon named {name}")

    def listeners_ready():
        for name, process in processes:
            if not name.startswith(("slurmctld", "slurmd")):
                continue
            inspection = run(
                [
                    "/usr/sbin/lsof",
                    "-Pan",
                    "-p",
                    process.pid,
                    "-iTCP",
                    "-sTCP:LISTEN",
                    "-Fn",
                ],
                check=False,
            )
            if inspection.returncode not in (0, 1) or inspection.stderr.strip():
                raise RuntimeError(f"Listener inspection failed: {inspection.stderr}")
            addresses = [
                line[1:]
                for line in inspection.stdout.splitlines()
                if line.startswith("n")
            ]
            if not addresses:
                return False
            for address in addresses:
                host_address = address.rsplit(":", 1)[0].strip("[]")
                if not ipaddress.ip_address(host_address).is_loopback:
                    raise RuntimeError(f"{name} bound outside loopback: {address}")
        return True

    def verify_listeners():
        # After restart, controller readiness can precede the node listener.
        wait_for(listeners_ready, "private daemon listeners", timeout=30)
        event("loopback_listeners", passed=True)

    def wait_for(predicate, description, timeout=20):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for name, process in processes:
                if process.poll() is not None:
                    tail = (runtime / f"{name}.log").read_text()[-10000:]
                    raise RuntimeError(f"{name} exited {process.returncode}: {tail}")
            if predicate():
                return
            time.sleep(0.2)
        raise RuntimeError(f"Timeout waiting for {description}")

    def free_port():
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            return sock.getsockname()[1]

    def controller_ready():
        try:
            return (
                run(
                    [prefix / "bin/scontrol", "ping"], check=False, timeout=8
                ).returncode
                == 0
            )
        except subprocess.TimeoutExpired:
            # Initial plugin validation may outlast one ping on a fresh
            # installation. The outer startup deadline remains bounded.
            return False

    def job_state(job):
        proc = run([prefix / "bin/scontrol", "show", "job", job, "-o"])
        return next(
            (
                part.split("=", 1)[1]
                for part in proc.stdout.split()
                if part.startswith("JobState=")
            ),
            "UNKNOWN",
        )

    try:
        for name in ("slurmctld", "slurmd", "slurmstepd"):
            if not (prefix / "sbin" / name).is_file():
                raise RuntimeError(f"Missing installed daemon: {name}")
        for dirname in ("state", "spool"):
            (runtime / dirname).mkdir(mode=0o700)
        (runtime / "slurmd-daemon.log").touch(mode=0o600)
        key = runtime / "munge.key"
        with os.fdopen(
            os.open(key, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "wb"
        ) as output:
            output.write(os.urandom(128))
        munge_socket = runtime / "munge.sock"
        ctld_port, node_port = free_port(), free_port()
        while node_port == ctld_port:
            node_port = free_port()
        (runtime / "slurm.conf").write_text(
            f"ClusterName=macos-smoke\n"
            f"SlurmctldHost={host}(127.0.0.1)\n"
            f"SlurmctldAddr=127.0.0.1\n"
            f"SlurmctldPort={ctld_port}\nSlurmdPort={node_port}\n"
            f"SlurmUser={user}\nSlurmdUser={user}\n"
            f"AuthType=auth/munge\nCredType=cred/munge\n"
            f"AuthInfo=socket={munge_socket}\n"
            f"PluginDir={prefix / 'lib/slurm'}\n"
            f"SlurmctldPidFile={runtime / 'slurmctld.pid'}\n"
            f"SlurmdPidFile={runtime / 'slurmd.pid'}\n"
            f"SlurmdLogFile={runtime / 'slurmd-daemon.log'}\n"
            f"StateSaveLocation={runtime / 'state'}\n"
            f"SlurmdSpoolDir={runtime / 'spool'}\n"
            f"SlurmctldDebug=debug\nSlurmdDebug=debug\n"
            f"{network_debug}"
            f"{interactive_config}"
            f"CommunicationParameters=NoCtldInAddrAny,NoInAddrAny\n"
            f"ProctrackType=proctrack/{args.proctrack_plugin}\n"
            f"TaskPlugin=task/{args.task_plugin}\n"
            f"NamespaceType=namespace/none\n"
            f"DefRuntimePlugin=none\n"
            f"JobAcctGatherType=jobacct_gather/darwin\n"
            f"JobAcctGatherFrequency=task=1\n"
            f"SelectType=select/cons_tres\nSelectTypeParameters=CR_CPU_Memory\n"
            f"SchedulerType=sched/backfill\nReturnToService=2\n"
            f"SlurmdTimeout=30\nMessageTimeout=5\nKillWait=2\n"
            f"MinJobAge=300\nSlurmctldParameters=enable_user_top\n"
            f"{gpu_config}"
            f"NodeName=macos-smoke NodeHostname={host} NodeAddr=127.0.0.1 "
            f"CPUs={os.cpu_count()} RealMemory=4096 {gpu_resources}State=UNKNOWN\n"
            f"PartitionName=debug Nodes=macos-smoke Default=YES "
            f"MaxTime=5 State=UP\n"
        )
        (runtime / "slurm.conf").chmod(0o600)
        if args.task_plugin == "darwin":
            native_policy = "PerProcessCPUTimeSeconds=30\n"
            if args.native_enforcement:
                native_policy += (
                    "InitialTaskImageFootprintMiB=256\n"
                    "DenyUnallocatedGPUConnections=yes\n"
                )
            (runtime / "darwin.conf").write_text(native_policy)
            (runtime / "darwin.conf").chmod(0o600)
        if args.metal_workload:
            if not args.metal_workload.resolve().is_file():
                raise RuntimeError("Missing Metal test executable")
            (runtime / "gres.conf").write_text(
                "NodeName=macos-smoke AutoDetect=metal\n"
            )
            (runtime / "gres.conf").chmod(0o600)
        event("runtime", path=str(runtime))
        launch(
            "munged",
            [
                munge_bin / "munged",
                "--foreground",
                "--socket",
                munge_socket,
                "--key-file",
                key,
                "--pid-file",
                runtime / "munged.pid",
                "--seed-file",
                runtime / "munge.seed",
                "--origin",
                "127.0.0.1",
            ],
        )
        wait_for(munge_socket.exists, "MUNGE socket")
        token = run([munge_bin / "munge", "--socket", munge_socket, "-n"]).stdout
        decoded = run([munge_bin / "unmunge", "--socket", munge_socket], input=token)
        if "SUCCESS" not in decoded.stdout.upper():
            raise RuntimeError("MUNGE round trip did not report SUCCESS")
        event("munge_roundtrip", passed=True)
        launch("slurmctld", [prefix / "sbin/slurmctld", "-D", "-f", env["SLURM_CONF"]])
        wait_for(controller_ready, "controller", timeout=90)
        launch(
            "slurmd",
            [
                prefix / "sbin/slurmd",
                "-D",
                "-N",
                "macos-smoke",
                "-f",
                env["SLURM_CONF"],
            ],
        )
        wait_for(
            lambda: "idle" in run([prefix / "bin/sinfo", "-h", "-o", "%t"]).stdout,
            "idle compute node",
            timeout=60,
        )
        event("node_registration", passed=True)
        verify_listeners()
        command = [
            prefix / "bin/srun",
            "--nodes=1",
            "--ntasks=1",
            "--mem=128",
            "--cpu-bind=none",
        ]
        proc = run(
            command + ["/bin/sh", "-c", "printf native-out; printf native-err >&2"]
        )
        assert proc.stdout == "native-out", proc.stdout
        assert "native-err" in proc.stderr, proc.stderr
        event("srun_streams", passed=True)
        proc = run(command + ["/bin/cat"], input="native-stdin\n")
        assert proc.stdout == "native-stdin\n", proc.stdout
        event("srun_stdin", passed=True)
        proc = run(
            command
            + [
                "--cpus-per-task=2",
                "--chdir",
                runtime,
                "--export=ALL,SLURM_MACOS_TEST=environment-pass",
                sys.executable,
                "-c",
                "import os; print(os.getcwd()); "
                "print(os.environ['SLURM_MACOS_TEST']); "
                "print(os.environ['SLURM_CPUS_PER_TASK'])",
            ]
        )
        assert proc.stdout.splitlines() == [
            str(runtime.resolve()),
            "environment-pass",
            "2",
        ], proc.stdout
        event("job_environment_and_cwd", passed=True)
        proc = run(command + ["/bin/sh", "-c", "exit 7"], check=False)
        assert proc.returncode == 7, proc
        event("srun_exit_status", passed=True)
        output = run_pty(
            command
            + [
                "--pty",
                sys.executable,
                "-c",
                "import os, time; assert os.isatty(0) and os.isatty(1); "
                "fd = os.open('/dev/tty', os.O_RDWR); os.close(fd); "
                "print('PTY_CONTROL=PASS'); "
                "assert os.get_terminal_size(0) == (80, 24); "
                "print('PTY_RESIZE_READY', flush=True); "
                "end = time.monotonic() + 10\n"
                "while os.get_terminal_size(0) != (109, 47) and time.monotonic() < end:\n"
                "    time.sleep(0.05)\n"
                "assert os.get_terminal_size(0) == (109, 47), os.get_terminal_size(0)\n"
                "print('PTY_RESIZE=PASS')",
            ],
            runtime=runtime,
            env=env,
            results=results,
            resize=True,
        )
        assert "PTY_CONTROL=PASS" in output, output
        event("pty_controlling_terminal", passed=True)
        assert "PTY_RESIZE=PASS" in output, output
        event("pty_resize_forwarding", passed=True)
        if args.task_plugin == "darwin":
            proc = run(
                command
                + [
                    sys.executable,
                    "-c",
                    "import resource; print(resource.getrlimit(resource.RLIMIT_CPU))",
                ]
            )
            assert proc.stdout.strip() == "(30, 30)", proc.stdout
            event("launched_cpu_limit", passed=True)
        if args.native_enforcement:
            # Use a controlled image; system language launchers can replace
            # themselves and thereby reset an initial-image footprint limit.
            allocation = args.footprint_workload.resolve()
            proc = run(command + [allocation], check=False)
            assert proc.returncode == 128 + signal.SIGKILL, proc
            assert "allocation-start" in proc.stdout, proc.stdout
            assert "allocation-complete" not in proc.stdout, proc.stdout
            event("initial_image_footprint_kill", passed=True, status=proc.returncode)
            proc = run(command + ["/bin/sh", "-c", 'exec "$@"', "sh", allocation])
            assert "allocation-complete" in proc.stdout, proc.stdout
            event("later_exec_footprint_scope", passed=True)
            metal_probe = prefix / "libexec/slurm/slurm-darwin-metal-probe"
            proc = run(command + [metal_probe, "--expect-denied"])
            assert "denied/unavailable" in proc.stdout, proc.stdout
            event("unallocated_gpu_denied", passed=True, output=proc.stdout.strip())
            prolog = runtime / "gpu-prolog.sh"
            prolog.write_text(
                "#!/bin/sh\n"
                + shlex.join([str(metal_probe), "--expect-denied"])
                + " > gpu-prolog.out || exit 1\n"
            )
            prolog.chmod(0o700)
            proc = run(command + [f"--task-prolog={prolog}", "/usr/bin/true"])
            assert "denied/unavailable" in (runtime / "gpu-prolog.out").read_text()
            event("task_prolog_gpu_denied", passed=True)
            epilog = runtime / "gpu-epilog.sh"
            epilog_output = runtime / "gpu-epilog.out"
            epilog.write_text(
                "#!/bin/sh\nset -eu\n"
                '[ "${SLURM_SCRIPT_CONTEXT-}" = epilog_task ]\n'
                + shlex.join([str(metal_probe), "--expect-denied"])
                + " > "
                + shlex.quote(str(epilog_output))
                + "\nprintf 'GPU_EPILOG_MAIN=PASS\\n' >> "
                + shlex.quote(str(epilog_output))
                + "\n"
            )
            epilog.chmod(0o700)
            run(command + [f"--task-epilog={epilog}", "/usr/bin/true"])
            # User epilog failure is only logged by stepd. Require the epilog's
            # completed probe artifact, not merely a successful task exit.
            wait_for(
                lambda: epilog_output.exists()
                and "GPU_EPILOG_MAIN=PASS" in epilog_output.read_text(),
                "unallocated GPU epilog completion",
                timeout=10,
            )
            output = epilog_output.read_text()
            assert "denied/unavailable" in output, output
            event("task_epilog_gpu_denied", passed=True, output=output.strip())
            proc = run(command + ["--gres=gpu:1", metal_probe, "--expect-allowed"])
            assert "compute=PASS" in proc.stdout, proc.stdout
            event("allocated_gpu_allowed", passed=True, output=proc.stdout.strip())
            # salloc with an explicit command runs that command locally. Use
            # its configured default instead, which creates a real interactive
            # step. That step must use job GRES despite having no step GRES.
            interactive_script.write_text(
                "#!/bin/sh\nset -eu\n"
                'case "${SLURM_STEP_ID-}" in -6|4294967290) ;; '
                '*) echo "Not a Slurm interactive step" >&2; exit 1;; esac\n'
                'case "${SLURM_MACOS_INTERACTIVE_EXPECT-}" in allowed|denied) ;; '
                "*) exit 1;; esac\n"
                "step=$("
                + shlex.quote(str(prefix / "bin/scontrol"))
                + ' show step "${SLURM_JOB_ID}.interactive" -o)\n'
                'printf "%s\\n" "$step"\n'
                'case "$step" in *"StepId=${SLURM_JOB_ID}.interactive "*) ;; '
                "*) exit 1;; esac\n"
                'printf "INTERACTIVE_STEP=%s.%s EXPECT=%s\\n" '
                '"$SLURM_JOB_ID" "$SLURM_STEP_ID" "$SLURM_MACOS_INTERACTIVE_EXPECT"\n'
                + shlex.quote(str(metal_probe))
                + ' "--expect-$SLURM_MACOS_INTERACTIVE_EXPECT"\n'
            )
            interactive_script.chmod(0o700)
            try:
                for expected in ("denied", "allowed"):
                    env["SLURM_MACOS_INTERACTIVE_EXPECT"] = expected
                    selection = ["--gres=gpu:1"] if expected == "allowed" else []
                    output = run_pty(
                        [
                            prefix / "bin/salloc",
                            "--nodes=1",
                            "--ntasks=1",
                            "--mem=128",
                            "--time=1",
                            f"--job-name=interactive-{expected}-{runtime.name}",
                        ]
                        + selection,
                        runtime=runtime,
                        env=env,
                        results=results,
                        timeout=45,
                    )
                    assert f" EXPECT={expected}" in output, output
                    assert "INTERACTIVE_STEP=" in output, output
                    assert ".interactive " in output, output
                    assert (
                        "compute=PASS"
                        if expected == "allowed"
                        else "denied/unavailable"
                    ) in output, output
                    event(f"interactive_gpu_{expected}", passed=True, output=output)
            finally:
                env.pop("SLURM_MACOS_INTERACTIVE_EXPECT", None)
            if args.gpu_loader_probe:
                for expected in ("denied", "allowed"):
                    marker = runtime / f"gpu-loader-{expected}.json"
                    selection = ["--gres=gpu:1"] if expected == "allowed" else []
                    proc = run(
                        command
                        + selection
                        + [
                            "--export=ALL,"
                            f"DYLD_INSERT_LIBRARIES={args.gpu_loader_probe.resolve()},"
                            f"SLURM_GPU_LOADER_EXPECT={expected},"
                            f"SLURM_GPU_LOADER_MARKER={marker}",
                            args.gpu_loader_host.resolve(),
                        ]
                    )
                    record = json.loads(marker.read_text())
                    assert "GPU_LOADER_HOST_MAIN=PASS" in proc.stdout, proc.stdout
                    assert record["constructor_ran"] and record["matched"], record
                    assert record["expected"] == expected, record
                    assert record["computed"] == (expected == "allowed"), record
                    event(f"gpu_loader_{expected}", passed=True, result=record)
                marker = runtime / "gpu-epilog-loader.json"
                host_marker = runtime / "gpu-epilog-loader-main.out"
                run(
                    command
                    + [
                        f"--task-epilog={args.gpu_loader_host.resolve()}",
                        "--export=ALL,"
                        f"DYLD_INSERT_LIBRARIES={args.gpu_loader_probe.resolve()},"
                        "SLURM_GPU_LOADER_EXPECT=denied,"
                        f"SLURM_GPU_LOADER_MARKER={marker},"
                        f"SLURM_GPU_LOADER_HOST_MARKER={host_marker}",
                        "/usr/bin/true",
                    ]
                )
                wait_for(
                    lambda: host_marker.exists()
                    and "GPU_LOADER_HOST_MAIN=PASS" in host_marker.read_text(),
                    "unallocated GPU loader epilog completion",
                    timeout=10,
                )
                record = json.loads(marker.read_text())
                assert record["constructor_ran"] and record["matched"], record
                assert record["expected"] == "denied", record
                assert not record["computed"], record
                assert "CONTEXT=epilog_task" in host_marker.read_text()
                event("task_epilog_gpu_loader_denied", passed=True, result=record)
        if args.metal_workload:
            proc = run(command + ["--gres=gpu:1", args.metal_workload.resolve()])
            event("metal_compute", passed=True, output=proc.stdout.strip())
            gpu_jobs = []
            for number in range(2):
                proc = run(
                    [
                        prefix / "bin/sbatch",
                        "--parsable",
                        "--mem=128",
                        "--gres=gpu:1",
                        "--output",
                        runtime / f"gpu-{number}.out",
                        "--wrap",
                        "sleep 4",
                    ]
                )
                gpu_jobs.append(proc.stdout.strip().split(";")[0])
                if number == 0:
                    wait_for(
                        lambda: job_state(gpu_jobs[0]) == "RUNNING",
                        "first GPU allocation",
                    )
            assert job_state(gpu_jobs[1]) == "PENDING", "GPU was allocated twice"
            wait_for(
                lambda: all(job_state(job) == "COMPLETED" for job in gpu_jobs),
                "serialized GPU allocations",
                timeout=35,
            )
            event("metal_exclusive_scheduling", passed=True)
        if args.pmi1_workload:
            proc = run(
                [
                    prefix / "bin/srun",
                    "--nodes=1",
                    "--ntasks=2",
                    "--mem=128",
                    "--cpu-bind=none",
                    "--mpi=none",
                    args.pmi1_workload.resolve(),
                ],
                timeout=35,
            )
            assert sorted(proc.stdout.splitlines()) == [
                "PMI1 rank=0 size=2 KVS=PASS",
                "PMI1 rank=1 size=2 KVS=PASS",
            ], proc.stdout
            event("pmi1_kvs", passed=True)
        if args.pmi2_workload:
            proc = run(
                [
                    prefix / "bin/srun",
                    "--nodes=1",
                    "--ntasks=2",
                    "--mem=128",
                    "--cpu-bind=none",
                    "--mpi=pmi2",
                    args.pmi2_workload.resolve(),
                ]
            )
            assert sorted(proc.stdout.splitlines()) == [
                "PMI2 rank=0 size=2 KVS=PASS",
                "PMI2 rank=1 size=2 KVS=PASS",
            ], proc.stdout
            event("pmi2_kvs", passed=True)
        if args.pmix_workload:
            proc = run(
                [
                    prefix / "bin/srun",
                    "--nodes=1",
                    "--ntasks=2",
                    "--mem=128",
                    "--cpu-bind=none",
                    "--mpi=pmix",
                    args.pmix_workload.resolve(),
                ]
            )
            assert sorted(proc.stdout.splitlines()) == [
                "PMIx rank=0 size=2 KVS=PASS",
                "PMIx rank=1 size=2 KVS=PASS",
            ], proc.stdout
            event("pmix_kvs", passed=True)
        # Keep one task CPU-busy until two positive, increasing samples arrive.
        # This exercises live accumulation; it does not verify final database
        # totals or retained CPU time from children that have already exited.
        accounting_worker = runtime / "accounting.py"
        accounting_finish = runtime / "accounting-finish"
        accounting_worker.write_text(
            "import time\n"
            "from pathlib import Path\n"
            "end = time.monotonic() + 25\n"
            "value = 1\n"
            "while not Path('accounting-finish').exists():\n"
            "    for count in range(50000):\n"
            "        value = (value * 13 + count) % 1000003\n"
            "    if time.monotonic() > end:\n"
            "        raise RuntimeError('CPU accounting was not observed in time')\n"
        )
        jobs = []
        for number in range(2):
            output = runtime / f"batch-{number}.out"
            workload = (
                "exec " + shlex.join([sys.executable, str(accounting_worker)])
                if number == 0
                else "sleep 8"
            )
            proc = run(
                [
                    prefix / "bin/sbatch",
                    "--parsable",
                    "--mem=128",
                    "--cpus-per-task=1",
                    "--output",
                    output,
                    "--wrap",
                    f"printf batch-{number}; {workload}",
                ]
            )
            jobs.append(proc.stdout.strip().split(";")[0])
        wait_for(
            lambda: all(job_state(job) == "RUNNING" for job in jobs),
            "two concurrent batch jobs",
        )
        event("concurrent_jobs", passed=True, job_ids=jobs)
        samples = []

        def sample_accounting():
            proc = run(
                [
                    prefix / "bin/sstat",
                    "--noheader",
                    "--parsable2",
                    "--noconvert",
                    "--jobs",
                    f"{jobs[0]}.batch",
                    "--format=JobID,MaxRSS,AveCPU",
                ]
            )
            if not proc.stdout.strip():
                return False
            fields = proc.stdout.strip().split("|")
            assert len(fields) == 3 and fields[0] == f"{jobs[0]}.batch", proc.stdout
            if not fields[1] or not fields[2]:
                return False
            rss = float(fields[1])
            # AveCPU is printed in whole seconds; this bounded workload never
            # reaches the day-qualified format.
            hours, minutes, seconds = map(int, fields[2].split(":"))
            cpu_seconds = hours * 3600 + minutes * 60 + seconds
            if rss <= 0 or cpu_seconds <= 0:
                return False
            samples.append(
                {"output": proc.stdout.strip(), "rss": rss, "cpu_seconds": cpu_seconds}
            )
            return cpu_seconds > samples[0]["cpu_seconds"]

        wait_for(
            sample_accounting, "positive and increasing live CPU accounting", timeout=15
        )
        accounting_finish.touch()
        event("live_rss", passed=True, rss=samples[-1]["rss"])
        event("live_cpu_accounting", passed=True, samples=[samples[0], samples[-1]])
        wait_for(
            lambda: all(job_state(job) == "COMPLETED" for job in jobs),
            "batch completion",
        )
        for number in range(2):
            output = (runtime / f"batch-{number}.out").read_text()
            assert output == f"batch-{number}", output
        event("sbatch_outputs", passed=True)
        heartbeat = runtime / "heartbeat"
        worker = runtime / "heartbeat.py"
        worker.write_text(
            "import time\n"
            "from pathlib import Path\n"
            "for count in range(1000):\n"
            "    Path('heartbeat').write_text(str(count))\n"
            "    time.sleep(0.1)\n"
        )
        proc = run(
            [
                prefix / "bin/sbatch",
                "--parsable",
                "--mem=128",
                "--output",
                runtime / "suspend.out",
                "--wrap",
                shlex.join([sys.executable, str(worker)]),
            ]
        )
        suspended_job = proc.stdout.strip().split(";")[0]
        wait_for(
            lambda: heartbeat.exists() and heartbeat.read_text().strip(),
            "suspend test heartbeat",
        )
        run([prefix / "bin/scontrol", "suspend", suspended_job])
        wait_for(lambda: job_state(suspended_job) == "SUSPENDED", "suspended job")
        time.sleep(0.3)  # Let the node receive and deliver the stop signal.
        frozen = heartbeat.read_text()
        time.sleep(0.5)
        assert heartbeat.read_text() == frozen, "Suspended task kept running"
        run([prefix / "bin/scontrol", "resume", suspended_job])
        wait_for(lambda: heartbeat.read_text() != frozen, "resumed heartbeat")
        run([prefix / "bin/scancel", suspended_job])
        wait_for(
            lambda: job_state(suspended_job).startswith("CANCELLED"),
            "suspend test cleanup",
        )
        event("suspend_resume", passed=True)
        proc = run(
            [
                prefix / "bin/sbatch",
                "--parsable",
                "--mem=128",
                "--output",
                runtime / "cancel.out",
                "--wrap",
                "sleep 45",
            ]
        )
        job = proc.stdout.strip().split(";")[0]
        wait_for(lambda: job_state(job) == "RUNNING", "cancellation test job")
        run([prefix / "bin/scancel", job])
        wait_for(lambda: job_state(job).startswith("CANCELLED"), "cancelled job")
        wait_for(
            lambda: "idle" in run([prefix / "bin/sinfo", "-h", "-o", "%t"]).stdout,
            "node cleanup",
        )
        event("cancellation", passed=True)
        controller_log = runtime / "slurmctld.log"
        reconfigure_offset = controller_log.stat().st_size
        run([prefix / "bin/scontrol", "reconfigure"])
        # Reconfigure acknowledges the request before controller re-exec has
        # completed. Wait for this generation's startup, not the old listener.
        wait_for(
            lambda: (
                b"Running as primary controller"
                in controller_log.read_bytes()[reconfigure_offset:]
            ),
            "controller reconfiguration",
            timeout=90,
        )
        wait_for(controller_ready, "reconfigured controller", timeout=30)
        wait_for(
            lambda: "idle" in run([prefix / "bin/sinfo", "-h", "-o", "%t"]).stdout,
            "reconfigured node",
            timeout=60,
        )
        proc = run(command + ["/usr/bin/true"])
        event("reconfigure_and_run", passed=True)
        if args.extended_lifecycle:
            if args.proctrack_plugin == "darwin":
                detached = runtime / "detached.py"
                marker = runtime / "detached.pid"
                detached.write_text(
                    "import subprocess, time\n"
                    "from pathlib import Path\n"
                    "child = subprocess.Popen(['/bin/sleep', '45'], start_new_session=True)\n"
                    "Path('detached.pid').write_text(str(child.pid))\n"
                    "time.sleep(45)\n"
                )
                proc = run(
                    [
                        prefix / "bin/sbatch",
                        "--parsable",
                        "--mem=128",
                        "--output",
                        runtime / "detached.out",
                        "--wrap",
                        shlex.join([sys.executable, str(detached)]),
                    ]
                )
                job = proc.stdout.strip().split(";")[0]
                wait_for(
                    lambda: marker.exists() and marker.read_text().strip(),
                    "detached child",
                )
                detached_pid = int(marker.read_text())
                time.sleep(1)  # Exercise retained, observed detached membership.
                run([prefix / "bin/scancel", job])

                def detached_stopped():
                    state = run(
                        ["/bin/ps", "-o", "stat=", "-p", detached_pid], check=False
                    ).stdout.strip()
                    return not state or state.startswith("Z")

                wait_for(detached_stopped, "detached descendant cleanup")
                event("observed_detached_cleanup", passed=True)
            restart_marker = runtime / "restart-heartbeat"
            restart_worker = runtime / "restart.py"
            restart_worker.write_text(
                "import time\n"
                "from pathlib import Path\n"
                "for count in range(200):\n"
                "    Path('restart-heartbeat').write_text(str(count))\n"
                "    time.sleep(0.1)\n"
                "print('restart-survived')\n"
            )
            proc = run(
                [
                    prefix / "bin/sbatch",
                    "--parsable",
                    "--mem=128",
                    "--output",
                    runtime / "restart.out",
                    "--wrap",
                    shlex.join([sys.executable, str(restart_worker)]),
                ]
            )
            job = proc.stdout.strip().split(";")[0]
            wait_for(
                lambda: restart_marker.exists() and restart_marker.read_text().strip(),
                "restart test heartbeat",
            )
            restart_daemon(
                "slurmd",
                [
                    prefix / "sbin/slurmd",
                    "-D",
                    "-N",
                    "macos-smoke",
                    "-f",
                    env["SLURM_CONF"],
                ],
            )
            restart_daemon(
                "slurmctld", [prefix / "sbin/slurmctld", "-D", "-f", env["SLURM_CONF"]]
            )
            wait_for(controller_ready, "restarted controller", timeout=60)
            verify_listeners()
            wait_for(
                lambda: job_state(job) == "COMPLETED",
                "job after daemon restarts",
                timeout=45,
            )
            assert (runtime / "restart.out").read_text().strip() == "restart-survived"
            event("daemon_restart_job_survival", passed=True)
            proc = run(
                [
                    prefix / "bin/sbatch",
                    "--parsable",
                    "--mem=128",
                    "--time=00:01:00",
                    "--output",
                    runtime / "timeout.out",
                    "--wrap",
                    "sleep 120",
                ]
            )
            job = proc.stdout.strip().split(";")[0]
            wait_for(
                lambda: job_state(job) == "TIMEOUT",
                "job wall-time enforcement",
                timeout=100,
            )
            wait_for(
                lambda: "idle" in run([prefix / "bin/sinfo", "-h", "-o", "%t"]).stdout,
                "timeout cleanup",
            )
            event("wall_time_limit", passed=True)
        results["success"] = True
    except (Exception, KeyboardInterrupt) as error:
        results["error"] = f"{type(error).__name__}: {error}"
        event("failure", error=results["error"])
    finally:
        cleanup(runtime, prefix, user, processes, logs, results, run)
    return 0 if results["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
