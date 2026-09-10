#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise native MariaDB/slurmdbd in a private, same-user runtime directory.

Creates SQL data and SQL accounts only. No OS accounts, services, or system
configuration are changed. --prepare-only initializes the data directory without
starting a listening daemon; --runtime continues that prepared test later.
"""

import argparse
import ipaddress
import json
import os
import pwd
import secrets
import shlex
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def private_write(path, value):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, "wb") as stream:
        stream.write(value.encode() if isinstance(value, str) else value)


def loopback_preflight():
    # NoInAddrAny binds the first IPv4 address of the local full hostname.
    addresses = socket.getaddrinfo(
        socket.gethostname(), None, socket.AF_INET, socket.SOCK_STREAM
    )
    if not addresses or not ipaddress.ip_address(addresses[0][4][0]).is_loopback:
        raise RuntimeError("NoInAddrAny would bind a non-loopback hostname address")
    return addresses[0][4][0]


def stop_process(process):
    # Keep the leader unreaped until group signaling is complete. A reaped PID
    # may have been reused, so never signal its numeric process group afterward.
    if process.returncode is None:
        try:
            if os.getpgid(process.pid) != process.pid:
                raise RuntimeError("Owned daemon lost its private process group")
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=5)
    deadline = time.monotonic() + 3
    while True:
        try:
            os.killpg(process.pid, 0)
        except ProcessLookupError:
            break
        if time.monotonic() >= deadline:
            raise RuntimeError(
                "Unverified helper process group remains after leader exit; not signaling it"
            )
        time.sleep(0.1)
    if process.returncode not in (0, -signal.SIGTERM):
        raise RuntimeError(f"Owned process exited unexpectedly: {process.returncode}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--munge-bin", type=Path, required=True)
    parser.add_argument("--mariadb-prefix", type=Path, required=True)
    parser.add_argument("--runtime", type=Path)
    parser.add_argument("--runtime-parent", type=Path, default=Path("/tmp"))
    parser.add_argument("--prepare-only", action="store_true")
    args = parser.parse_args()
    if sys.platform != "darwin" or os.geteuid() == 0:
        parser.error("Run this native test as an ordinary macOS user")
    if signal.getsignal(signal.SIGCHLD) != signal.SIG_DFL:
        parser.error("Default SIGCHLD handling is required for owned-process cleanup")
    prefix, munge, maria = (
        p.resolve() for p in (args.prefix, args.munge_bin, args.mariadb_prefix)
    )
    for path in (prefix, munge, maria):
        if any(c.isspace() or c in "#=" for c in str(path)):
            parser.error("Test paths must not contain whitespace, #, or =")
    for binary in (
        prefix / "sbin/slurmdbd",
        prefix / "sbin/slurmctld",
        prefix / "sbin/slurmd",
        prefix / "bin/sacctmgr",
        prefix / "bin/sacct",
        prefix / "bin/sbatch",
        prefix / "bin/sinfo",
        prefix / "bin/scancel",
        prefix / "bin/squeue",
        munge / "munged",
        munge / "munge",
        munge / "unmunge",
        maria / "bin/mariadbd",
        maria / "bin/mariadb",
        maria / "bin/mariadb-install-db",
        Path("/usr/sbin/lsof"),
    ):
        if not os.access(binary, os.X_OK):
            parser.error(f"Required executable unavailable: {binary}")
    bind_address = loopback_preflight()
    user = pwd.getpwuid(os.getuid()).pw_name
    if args.runtime:
        runtime = args.runtime.resolve()
        manifest = json.loads((runtime / "manifest.json").read_text())
        if (
            runtime.stat().st_uid != os.getuid()
            or manifest.get("uid") != os.getuid()
            or manifest.get("prefix") != str(prefix)
            or manifest.get("mariadb_prefix") != str(maria)
            or manifest.get("phase") != "prepared"
        ):
            parser.error("Runtime is not an owned prepared test with these prefixes")
    else:
        runtime = Path(
            tempfile.mkdtemp(prefix="slurm-dbd-", dir=args.runtime_parent)
        ).resolve()
        runtime.chmod(0o711)
        (runtime / "sql").mkdir(mode=0o700)
        (runtime / "sql/data").mkdir(mode=0o700)
        for name in ("state", "spool"):
            (runtime / name).mkdir(mode=0o700)
        with socket.socket() as reserve, socket.socket() as ctld, socket.socket() as node:
            for descriptor in (reserve, ctld, node):
                descriptor.bind(("127.0.0.1", 0))
            ports = [
                descriptor.getsockname()[1] for descriptor in (reserve, ctld, node)
            ]
        manifest = {
            "uid": os.getuid(),
            "prefix": str(prefix),
            "mariadb_prefix": str(maria),
            "phase": "preparing",
            "port": ports[0],
            "ctld_port": ports[1],
            "node_port": ports[2],
            "cluster": "macacct" + secrets.token_hex(4),
        }
        private_write(runtime / "manifest.json", json.dumps(manifest, indent=2) + "\n")
    if len(str(runtime / "sql/mysql.sock").encode()) >= 104:
        parser.error("Runtime path is too long for Darwin UNIX sockets")
    if any(c.isspace() or c in "#=" for c in str(runtime)):
        parser.error("Runtime path must not contain whitespace, #, or =")
    env = {
        "PATH": f"{prefix / 'bin'}:/usr/bin:/bin:/usr/sbin:/sbin",
        "HOME": str(runtime),
        "USER": user,
        "LOGNAME": user,
        "LANG": "C",
        "LC_ALL": "C",
        "SLURM_CONF": str(runtime / "slurm.conf"),
    }
    results = {
        "runtime": str(runtime),
        "success": False,
        "checks": [],
        "cleanup_errors": [],
        "bind_preflight": bind_address,
    }
    processes, streams = [], []
    jobs = set()

    def event(name, **details):
        record = {"check": name, **details}
        results["checks"].append(record)
        print(json.dumps(record), flush=True)

    def run(argv, timeout=30, check=True, **kwargs):
        process = subprocess.Popen(
            [str(v) for v in argv],
            env=env,
            cwd=runtime,
            text=True,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=True,
        )
        try:
            stdout, stderr = process.communicate(kwargs.get("input"), timeout=timeout)
        except BaseException:
            stop_process(process)
            raise
        result = subprocess.CompletedProcess(argv, process.returncode, stdout, stderr)
        if check and result.returncode:
            raise RuntimeError(
                f"{Path(argv[0]).name} exited {result.returncode}: {stdout} {stderr}"
            )
        return result

    def launch(name, argv):
        fd = os.open(
            runtime / f"{name}.log", os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600
        )
        stream = os.fdopen(fd, "w")
        streams.append(stream)
        process = subprocess.Popen(
            [str(v) for v in argv],
            env=env,
            cwd=runtime,
            stdout=stream,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        processes.append((name, process))
        return process

    def wait_for(predicate, description, timeout=100):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for name, process in processes:
                if process.poll() is not None:
                    raise RuntimeError(
                        f"{name} exited during {description}; inspect its private log"
                    )
            if predicate():
                return
            time.sleep(0.2)
        raise RuntimeError(f"Timed out waiting for {description}")

    def listeners(process, expected_port=None, none=False):
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
            raise RuntimeError(f"Cannot inspect owned listeners: {output.stderr}")
        endpoints = [
            line[1:] for line in output.stdout.splitlines() if line.startswith("n")
        ]
        if none and endpoints:
            raise RuntimeError(f"Private SQL server opened TCP listeners: {endpoints}")
        for endpoint in endpoints:
            address, _, port = endpoint.rpartition(":")
            if not ipaddress.ip_address(address.strip("[]")).is_loopback:
                raise RuntimeError(
                    f"Owned daemon opened non-loopback listener: {endpoint}"
                )
            if expected_port is not None and int(port) != expected_port:
                raise RuntimeError(f"Owned daemon opened unexpected port: {endpoint}")
        if expected_port is not None and not endpoints:
            return False
        return endpoints if endpoints else True

    def sql(statement):
        return run(
            [
                maria / "bin/mariadb",
                "--no-defaults",
                "--protocol=socket",
                f"--socket={runtime / 'sql/mysql.sock'}",
                "--user=root",
                "--batch",
                "--skip-column-names",
            ],
            input=statement,
        ).stdout.strip()

    def start_sql(name):
        process = launch(
            name,
            [
                maria / "bin/mariadbd",
                "--no-defaults",
                f"--basedir={maria}",
                f"--datadir={runtime / 'sql/data'}",
                f"--socket={runtime / 'sql/mysql.sock'}",
                f"--pid-file={runtime / 'sql/mysql.pid'}",
                "--skip-networking",
                "--skip-name-resolve",
                "--skip-log-bin",
                "--innodb-buffer-pool-size=64M",
                "--innodb-log-file-size=32M",
                "--innodb-lock-wait-timeout=900",
                "--max-connections=32",
            ],
        )
        wait_for(lambda: (runtime / "sql/mysql.sock").exists(), "SQL socket")
        if sql("SELECT 1;") != "1":
            raise RuntimeError("SQL readiness query failed")
        listeners(process, none=True)
        return process

    def acct(*arguments, check=True):
        return run([prefix / "bin/sacctmgr", "-nP", "-i", *arguments], check=check)

    def start_dbd(name):
        loopback_preflight()
        # slurmdbd reads slurmdbd.conf beside SLURM_CONF, and has no -f option.
        process = launch(name, [prefix / "sbin/slurmdbd", "-D"])
        wait_for(
            lambda: listeners(process, expected_port=manifest["port"]),
            "slurmdbd listener",
        )
        event(
            name + "_listeners",
            endpoints=listeners(process, expected_port=manifest["port"]),
        )
        return process

    try:
        if not args.runtime:
            password = secrets.token_hex(24)
            private_write(runtime / "munge.key", os.urandom(1024))
            private_write(
                runtime / "darwin.conf", "# No optional resource controls requested.\n"
            )
            private_write(
                runtime / "slurmdbd.conf",
                f"""AuthType=auth/munge
AuthInfo=socket={runtime / 'munge.socket'}
CommunicationParameters=NoInAddrAny
DbdHost={socket.gethostname().split('.')[0]}
DbdAddr=127.0.0.1
DbdPort={manifest['port']}
SlurmUser={user}
PluginDir={prefix / 'lib/slurm'}
LogFile={runtime / 'slurmdbd-internal.log'}
PidFile={runtime / 'slurmdbd.pid'}
StorageType=accounting_storage/mysql
StorageHost=unix:{runtime / 'sql/mysql.sock'}
StorageLoc=slurm_native_test
StorageUser=slurm_native_test
StoragePass={password}
DebugLevel=debug
""",
            )
            private_write(
                runtime / "slurm.conf",
                f"""ClusterName={manifest['cluster']}
SlurmctldHost={socket.gethostname().split('.')[0]}(127.0.0.1)
SlurmctldAddr=127.0.0.1
SlurmctldPort={manifest['ctld_port']}
SlurmdPort={manifest['node_port']}
SlurmUser={user}
SlurmdUser={user}
SlurmctldPidFile={runtime / 'slurmctld.pid'}
SlurmdPidFile={runtime / 'slurmd.pid'}
SlurmdLogFile={runtime / 'slurmd-internal.log'}
StateSaveLocation={runtime / 'state'}
SlurmdSpoolDir={runtime / 'spool'}
SlurmctldDebug=debug
SlurmdDebug=debug
AuthType=auth/munge
AuthInfo=socket={runtime / 'munge.socket'}
CredType=cred/munge
PluginDir={prefix / 'lib/slurm'}
CommunicationParameters=NoInAddrAny,NoCtldInAddrAny
AccountingStorageType=accounting_storage/slurmdbd
AccountingStorageHost=127.0.0.1
AccountingStoragePort={manifest['port']}
AccountingStoragePass={runtime / 'munge.socket'}
AccountingStorageEnforce=associations
MessageTimeout=5
ProctrackType=proctrack/darwin
TaskPlugin=task/darwin
NamespaceType=namespace/none
JobAcctGatherType=jobacct_gather/darwin
JobAcctGatherFrequency=task=1
SelectType=select/cons_tres
SelectTypeParameters=CR_CPU_Memory
SchedulerType=sched/backfill
ReturnToService=2
SlurmdTimeout=30
KillWait=2
MinJobAge=300
NodeName=dbnode NodeHostname={socket.gethostname().split('.')[0]} NodeAddr=127.0.0.1 CPUs={os.cpu_count() or 1} RealMemory=1024 State=UNKNOWN
PartitionName=test Nodes=dbnode Default=YES MaxTime=2 State=UP
""",
            )
            init = run(
                [
                    maria / "bin/mariadb-install-db",
                    "--no-defaults",
                    f"--basedir={maria}",
                    f"--datadir={runtime / 'sql/data'}",
                    "--auth-root-authentication-method=normal",
                    "--skip-test-db",
                    "--skip-name-resolve",
                ],
                timeout=120,
            )
            private_write(runtime / "sql-initialize.log", init.stdout + init.stderr)
            manifest["phase"] = "prepared"
            (runtime / "manifest.json").write_text(
                json.dumps(manifest, indent=2) + "\n"
            )
            event("sql_data_initialized", passed=True)
        if args.prepare_only:
            results["success"] = True
            results["phase"] = "prepared"
        else:
            sql_process = start_sql("mariadb")
            password = next(
                line.split("=", 1)[1]
                for line in (runtime / "slurmdbd.conf").read_text().splitlines()
                if line.startswith("StoragePass=")
            )
            sql(
                f"CREATE USER 'slurm_native_test'@'localhost' IDENTIFIED BY '{password}';"
                "GRANT ALL ON slurm_native_test.* TO 'slurm_native_test'@'localhost';"
            )
            launch(
                "munged",
                [
                    munge / "munged",
                    "--foreground",
                    "--socket",
                    runtime / "munge.socket",
                    "--key-file",
                    runtime / "munge.key",
                    "--pid-file",
                    runtime / "munged.pid",
                    "--seed-file",
                    runtime / "munge.seed",
                    "--origin",
                    "127.0.0.1",
                ],
            )
            wait_for(lambda: (runtime / "munge.socket").exists(), "MUNGE socket")
            token = run(
                [munge / "munge", "--socket", runtime / "munge.socket", "-n"]
            ).stdout
            decoded = run(
                [munge / "unmunge", "--socket", runtime / "munge.socket"], input=token
            )
            if "SUCCESS" not in decoded.stdout.upper():
                raise RuntimeError("MUNGE credential round trip failed")
            event("munge_roundtrip", passed=True)
            dbd_process = start_dbd("slurmdbd")
            cluster = manifest["cluster"]
            # Neither disconnected sockets nor sacctmgr clients register as a
            # controller. Their cleanup must still own an initialized mutex.
            for _ in range(32):
                with socket.create_connection(
                    ("127.0.0.1", manifest["port"]), timeout=5
                ):
                    pass
            acct("list", "cluster", "format=Cluster")
            event("unregistered_client_disconnects", passed=True, connections=32)
            acct("add", "cluster", cluster)
            acct(
                "add",
                "account",
                "native_test",
                "Description=initial",
                "Organization=macos",
            )
            acct(
                "add",
                "user",
                user,
                "Account=native_test",
                f"Cluster={cluster}",
                "DefaultAccount=native_test",
            )
            acct(
                "modify",
                "account",
                "where",
                "Name=native_test",
                "set",
                "Description=updated",
            )
            clusters = acct(
                "list", "cluster", "where", f"Cluster={cluster}", "format=Cluster"
            ).stdout.strip()
            accounts = acct(
                "list",
                "account",
                "where",
                "Name=native_test",
                "format=Account,Description,Organization",
            ).stdout.strip()
            users = acct(
                "list",
                "user",
                "withassoc",
                "where",
                f"Name={user}",
                "format=User,DefaultAccount,Cluster,Account",
            ).stdout.strip()
            if (
                clusters != cluster
                or accounts != "native_test|updated|macos"
                or cluster not in users
            ):
                raise RuntimeError(
                    f"Accounting records differ: {clusters!r} {accounts!r} {users!r}"
                )
            table_count = int(
                sql(
                    "SELECT COUNT(*) FROM information_schema.tables "
                    "WHERE table_schema='slurm_native_test';"
                )
            )
            if table_count < 10:
                raise RuntimeError("Expected Slurm schema tables were not created")
            event(
                "schema_and_records",
                passed=True,
                tables=table_count,
                clusters=clusters,
                accounts=accounts,
                users=users,
            )
            stop_process(dbd_process)
            processes.remove(("slurmdbd", dbd_process))
            sql("SHUTDOWN;")
            sql_process.wait(timeout=20)
            stop_process(sql_process)
            processes.remove(("mariadb", sql_process))
            start_sql("mariadb-restart")
            start_dbd("slurmdbd-restart")
            if (
                acct(
                    "list",
                    "account",
                    "where",
                    "Name=native_test",
                    "format=Account,Description,Organization",
                ).stdout.strip()
                != accounts
            ):
                raise RuntimeError(
                    "Account did not persist across database and slurmdbd restart"
                )
            if (
                acct(
                    "list",
                    "user",
                    "withassoc",
                    "where",
                    f"Name={user}",
                    "format=User,DefaultAccount,Cluster,Account",
                ).stdout.strip()
                != users
            ):
                raise RuntimeError("User association did not persist across restart")
            event("database_and_dbd_restart", passed=True)
            acct("add", "account", "native_disposable", "Description=remove")
            acct("delete", "account", "where", "Name=native_disposable")
            if acct(
                "list", "account", "where", "Name=native_disposable", "format=Account"
            ).stdout.strip():
                raise RuntimeError("Deleted account still appears")
            event("account_delete", passed=True)

            loopback_preflight()
            controller = launch("slurmctld", [prefix / "sbin/slurmctld", "-D"])
            wait_for(
                lambda: listeners(controller, expected_port=manifest["ctld_port"]),
                "private controller listener",
            )
            loopback_preflight()
            node = launch("slurmd", [prefix / "sbin/slurmd", "-D", "-N", "dbnode"])
            wait_for(
                lambda: listeners(node, expected_port=manifest["node_port"]),
                "private compute-node listener",
            )
            wait_for(
                lambda: "idle" in run([prefix / "bin/sinfo", "-h", "-o", "%t"]).stdout,
                "native node registration",
            )
            event(
                "batch_cluster_listeners",
                controller=listeners(controller, manifest["ctld_port"]),
                node=listeners(node, manifest["node_port"]),
            )
            private_write(
                runtime / "workload.py",
                """import json, os, resource, sys, time
memory = bytearray(16 * 1024 * 1024)
for offset in range(0, len(memory), 4096): memory[offset] = 1
deadline = time.monotonic() + 4
counter = 0
while time.monotonic() < deadline: counter += 1
time.sleep(2)
usage = resource.getrusage(resource.RUSAGE_SELF)
print(json.dumps({'uid': os.getuid(), 'cpu_seconds': usage.ru_utime + usage.ru_stime,
                  'resident_bytes': usage.ru_maxrss, 'iterations': counter}), flush=True)
sys.exit(7)
""",
            )
            batch_script = (
                "#!/bin/sh\nexec "
                + shlex.join([sys.executable, str(runtime / "workload.py")])
                + "\n"
            )
            job = (
                run(
                    [
                        prefix / "bin/sbatch",
                        "--parsable",
                        "--account=native_test",
                        "--nodes=1",
                        "--ntasks=1",
                        "--mem=128",
                        "--time=1",
                        "--job-name=native-accounting-probe",
                        "--chdir",
                        runtime,
                        "--output",
                        runtime / "batch.out",
                        "--error",
                        runtime / "batch.err",
                    ],
                    input=batch_script,
                )
                .stdout.strip()
                .split(";")[0]
            )
            if not job.isdigit():
                raise RuntimeError(f"Invalid batch job ID: {job!r}")
            jobs.add(job)

            def completed_accounting():
                output = run(
                    [
                        prefix / "bin/sacct",
                        "-nP",
                        "-j",
                        job,
                        "--units=K",
                        "--format=JobIDRaw,State,ExitCode,TotalCPU,MaxRSS,AveRSS",
                    ]
                ).stdout
                rows = [line.split("|") for line in output.splitlines() if line]
                matches = [
                    row
                    for row in rows
                    if row[0] == job + ".batch" and row[1] == "FAILED"
                ]
                if not matches:
                    return False
                record = matches[0]
                if record[2] != "7:0":
                    raise RuntimeError(f"Unexpected recorded batch exit code: {record}")
                cpu = 0.0
                for component in record[3].split(":"):
                    cpu = cpu * 60 + float(component)
                if cpu <= 0.1 or not record[4] or float(record[4].rstrip("K")) < 8192:
                    raise RuntimeError(f"Missing expected CPU/RSS accounting: {record}")
                observed = json.loads((runtime / "batch.out").read_text())
                if observed["uid"] != os.getuid() or observed["cpu_seconds"] <= 0.1:
                    raise RuntimeError(f"Unexpected native workload result: {observed}")
                event(
                    "batch_to_database",
                    passed=True,
                    job_id=job,
                    rows=rows,
                    workload=observed,
                )
                return True

            wait_for(completed_accounting, "final batch accounting record", timeout=100)
            results["success"] = True
            results["phase"] = "complete"
            manifest["phase"] = "complete"
            (runtime / "manifest.json").write_text(
                json.dumps(manifest, indent=2) + "\n"
            )
    except Exception as error:
        results["error"] = str(error)
        event("failure", error=str(error))
    finally:
        if any(name == "slurmctld" for name, _ in processes):
            try:
                # Recover an accepted submission if the submitting CLI timed
                # out before returning its job ID.
                queued = run(
                    [prefix / "bin/squeue", "--noheader", "--format=%i|%j"], timeout=5
                )
                for line in queued.stdout.splitlines():
                    job, _, name = line.partition("|")
                    if job.isdigit() and name == "native-accounting-probe":
                        jobs.add(job)
            except Exception as error:
                results["cleanup_errors"].append(f"Recovering private job IDs: {error}")
        for job in jobs:
            try:

                def queued():
                    return run(
                        [
                            prefix / "bin/squeue",
                            "--noheader",
                            "--jobs",
                            job,
                            "--format=%i",
                        ],
                        timeout=5,
                    ).stdout.strip()

                if queued():
                    canceled = run(
                        [prefix / "bin/scancel", job], timeout=8, check=False
                    )
                    if canceled.returncode:
                        canceled = run(
                            [prefix / "bin/scancel", "--ctld", job],
                            timeout=8,
                            check=False,
                        )
                    if canceled.returncode and queued():
                        raise RuntimeError(f"scancel failed: {canceled.stderr}")
                    deadline = time.monotonic() + 45
                    while queued():
                        if time.monotonic() >= deadline:
                            raise RuntimeError(
                                "Private job did not drain before daemon shutdown"
                            )
                        time.sleep(0.2)
            except Exception as error:
                results["cleanup_errors"].append(
                    f"Canceling private job {job}: {error}"
                )
        for name, process in reversed(processes):
            try:
                stop_process(process)
            except Exception as error:
                results["cleanup_errors"].append(f"{name}: {error}")
        for stream in streams:
            stream.close()
        if results["cleanup_errors"]:
            results["success"] = False
        if not args.prepare_only:
            if not results["success"]:
                manifest["phase"] = "failed"
                (runtime / "manifest.json").write_text(
                    json.dumps(manifest, indent=2) + "\n"
                )
            for name in ("munge.key", "munge.seed"):
                (runtime / name).unlink(missing_ok=True)
            path = runtime / "slurmdbd.conf"
            if path.exists():
                path.write_text(
                    "\n".join(
                        (
                            line
                            if not line.startswith("StoragePass=")
                            else "StoragePass=REDACTED"
                        )
                        for line in path.read_text().splitlines()
                    )
                    + "\n"
                )
        report = runtime / (
            "prepare-results.json" if args.prepare_only else "results.json"
        )
        private_write(report, json.dumps(results, indent=2) + "\n")
        print(
            json.dumps(
                {
                    "success": results["success"],
                    "runtime": str(runtime),
                    "phase": results.get("phase"),
                    "cleanup_errors": results["cleanup_errors"],
                }
            ),
            flush=True,
        )
    return 0 if results["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
