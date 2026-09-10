#!/usr/bin/env python3
"""Unit/own-process evidence checks; never starts Slurm or uses privilege.

The ordinary-user probe check validates the fixed probe's result shape and
direct credential syscalls. It does not qualify Slurm's root-to-user transition.
Distributed under Slurm's GNU GPL version 2 or later.
"""

import copy
import importlib.util
import json
import os
import resource
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "privileged_smoke", Path(__file__).with_name("privileged-smoke.py")
)
smoke = importlib.util.module_from_spec(spec)
spec.loader.exec_module(smoke)


class AccountingObservation(unittest.TestCase):
    def test_raw_maxrss_bytes_are_not_reported_as_kib(self):
        # sstat print.c passes TRES_MEM bytes with UNIT_NONE; --noconvert
        # preserves this value without adding K/M suffixes.
        observed = smoke.parse_accounting_sample("7.batch|33554432|00:00:01\n", "7")
        self.assertEqual(observed["max_rss_bytes"], 32 * 1024 * 1024)
        self.assertNotIn("max_rss_kib", observed)
        self.assertEqual(observed["average_cpu_seconds"], 1)

    def test_elapsed_time_days(self):
        observed = smoke.parse_accounting_sample("17.batch|65536|2-03:04:05\n", "17")
        self.assertEqual(observed["average_cpu_seconds"], 183845)

    def test_no_sample_and_header_cannot_qualify(self):
        for text in ("", "\n", "JobID|MaxRSS|AveCPU\n"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                smoke.parse_accounting_sample(text, "7")

    def test_wrong_step_job_and_multiple_rows_cannot_qualify(self):
        for text in (
            "8.batch|33554432|00:00:01",
            "7.0|33554432|00:00:01",
            "7.batch|33554432|00:00:01\n7.batch|33554432|00:00:02",
            "7.batch|33554432|00:00:01|",
        ):
            with self.subTest(text=text), self.assertRaises(ValueError):
                smoke.parse_accounting_sample(text, "7")

    def test_memory_must_be_positive_raw_numeric(self):
        for memory in ("0", "0.0", "-1", "32M", "32K", "", "nan", "inf", "1e9"):
            with self.subTest(memory=memory), self.assertRaises(ValueError):
                smoke.parse_accounting_sample("7.batch|" + memory + "|00:00:01", "7")

    def test_cpu_must_be_positive_valid_elapsed_time(self):
        for cpu in (
            "00:00:00",
            "00:00:00.000",
            "",
            "Unknown",
            "00:60:00",
            "00:00:60",
            "-1:00:00",
        ):
            with self.subTest(cpu=cpu), self.assertRaises(ValueError):
                smoke.parse_accounting_sample("7.batch|33554432|" + cpu, "7")


class EpilogObservation(unittest.TestCase):
    def setUp(self):
        self.record = {
            "uid": 501,
            "euid": 501,
            "gid": 20,
            "egid": 20,
            "groups": [20, 80],
            "cwd": "/private/work",
            "context": "epilog_task",
            "job_id": "7",
            "cpu_limit": [30, 30],
            "expectation": "--expect-denied",
            "returncode": 0,
            "stdout": "Metal denied/unavailable\n",
            "stderr": "",
        }

    def verify(self, record, expectation="--expect-denied"):
        smoke.verify_epilog_observation(
            record, 501, 20, [20, 80], Path("/private/work"), expectation
        )

    def test_denial_and_allocated_computation(self):
        self.verify(self.record)
        self.record.update(expectation="--expect-allowed", stdout="compute=PASS\n")
        self.verify(self.record, "--expect-allowed")

    def test_wrong_identity_context_and_policy_cannot_qualify(self):
        for key, value in (
            ("uid", 0),
            ("euid", 0),
            ("gid", 0),
            ("egid", 0),
            ("groups", [20]),
            ("cwd", "/"),
            ("context", "task"),
            ("job_id", None),
            ("cpu_limit", [-1, -1]),
            ("expectation", "--expect-allowed"),
            ("returncode", 1),
            ("stdout", "compute=PASS\n"),
        ):
            with self.subTest(key=key), self.assertRaises(ValueError):
                record = copy.deepcopy(self.record)
                record[key] = value
                self.verify(record)

    def test_missing_epilog_record_cannot_qualify(self):
        with self.assertRaises(ValueError):
            self.verify({})


@unittest.skipIf(os.getuid() == 0 or os.geteuid() == 0, "ordinary-user-only fixture")
class FixedProbe(unittest.TestCase):
    def test_epilog_probe_publishes_private_worker_observation(self):
        with tempfile.TemporaryDirectory(prefix="slurm-epilog-unit-") as directory:
            root = Path(directory).resolve()
            fixture = root / "metal-fixture.sh"
            fixture.write_text(
                '#!/bin/sh\n[ "$1" = --expect-denied ] || exit 2\n'
                'echo "Metal denied/unavailable"\n'
            )
            fixture.chmod(0o700)
            output = root / "epilog.json"

            def child_limit():
                resource.setrlimit(resource.RLIMIT_CPU, (30, 30))

            subprocess.run(
                [
                    sys.executable,
                    "-I",
                    "-c",
                    smoke.EPILOG_PROBE,
                    str(fixture),
                    "--expect-denied",
                    str(output),
                ],
                cwd=root,
                env={
                    "PATH": "/usr/bin:/bin",
                    "SLURM_JOB_ID": "73",
                    "SLURM_SCRIPT_CONTEXT": "epilog_task",
                },
                capture_output=True,
                text=True,
                timeout=6,
                preexec_fn=child_limit,
                check=True,
            )
            record = json.loads(output.read_text())
            smoke.verify_epilog_observation(
                record,
                os.getuid(),
                os.getgid(),
                os.getgroups(),
                root,
                "--expect-denied",
            )
            self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o600)

    def test_worker_observer_rejects_fifo_without_waiting_for_a_writer(self):
        # Keep the blocking-read regression bounded without starting a cluster.
        source = """import importlib.util, os, sys
from pathlib import Path
spec = importlib.util.spec_from_file_location("privileged_smoke", sys.argv[1])
smoke = importlib.util.module_from_spec(spec)
spec.loader.exec_module(smoke)
try:
    smoke.read_worker_file(Path("fifo"), os.getuid(), read=True)
except RuntimeError:
    pass
else:
    raise SystemExit("FIFO accepted")
"""
        with tempfile.TemporaryDirectory(prefix="slurm-worker-file-unit-") as directory:
            os.mkfifo(Path(directory) / "fifo", 0o600)
            subprocess.run(
                [sys.executable, "-I", "-c", source, smoke.__file__],
                cwd=directory,
                capture_output=True,
                text=True,
                timeout=2,
                check=True,
            )

    def test_ordinary_user_probe_reports_credential_denial_and_private_output(self):
        with tempfile.TemporaryDirectory(prefix="slurm-probe-unit-") as directory:
            root = Path(directory).resolve()
            forbidden = root / "unreadable"
            forbidden.write_text("unit-test-only, no secrets\n")
            forbidden.chmod(0)
            output = root / "probe.json"

            def child_limit():
                resource.setrlimit(resource.RLIMIT_CPU, (30, 30))

            try:
                result = subprocess.run(
                    [
                        sys.executable,
                        "-I",
                        "-c",
                        smoke.PROBE,
                        str(output),
                        str(forbidden),
                    ],
                    cwd=root,
                    env={"PATH": "/usr/bin:/bin", "SLURM_JOB_ID": "73"},
                    capture_output=True,
                    text=True,
                    timeout=10,
                    preexec_fn=child_limit,
                    check=True,
                )
                record = json.loads(output.read_text())
                self.assertEqual(
                    (record["uid"], record["euid"]), (os.getuid(), os.geteuid())
                )
                self.assertEqual(
                    (record["gid"], record["egid"]), (os.getgid(), os.getegid())
                )
                self.assertEqual(record["groups"], sorted(set(os.getgroups())))
                self.assertEqual(record["cwd"], str(root))
                self.assertEqual(
                    record["root_regain_denied"], {"setuid": True, "setgid": True}
                )
                self.assertTrue(record["root_file_denied"])
                self.assertEqual(record["cpu_limit"], [30, 30])
                self.assertEqual(record["job_id"], "73")
                self.assertEqual(
                    stat.S_IMODE((root / "probe.created").stat().st_mode), 0o600
                )
                self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o600)
                self.assertEqual(result.stdout, "PRIVILEGED_SMOKE_JOB=73\n")
                self.assertEqual(result.stderr, "PRIVILEGED_SMOKE_STDERR\n")
            finally:
                forbidden.chmod(0o600)

    def test_job_canary_publishes_atomic_fresh_same_process_observations(self):
        with tempfile.TemporaryDirectory(prefix="slurm-job-canary-unit-") as directory:
            heartbeat = Path(directory) / "job-canary-heartbeat.json"
            process = subprocess.Popen(
                [sys.executable, "-I", "-c", smoke.JOB_CANARY],
                cwd=directory,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
            )
            try:
                first = None
                advanced = False
                deadline = time.monotonic() + 4
                while time.monotonic() < deadline:
                    try:
                        current = json.loads(heartbeat.read_text())
                    except FileNotFoundError:
                        time.sleep(0.01)
                        continue
                    # A partially written JSON file fails this test: publishing
                    # by rename must give each observer a complete record.
                    self.assertEqual(set(current), {"uid", "pid", "count"})
                    self.assertEqual(current["uid"], os.getuid())
                    self.assertEqual(current["pid"], process.pid)
                    if first is None:
                        first = current
                    elif current["count"] > first["count"]:
                        advanced = True
                        break
                    time.sleep(0.001)
                self.assertTrue(advanced, "fixed Slurm job canary did not advance")
            finally:
                process.terminate()
                process.communicate(timeout=3)


if __name__ == "__main__":
    unittest.main()
