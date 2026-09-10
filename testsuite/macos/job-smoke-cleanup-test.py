#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise smoke harness cleanup with mocked processes and RPCs."""

import importlib.util
import json
import signal
import subprocess
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

SOURCE = Path(__file__).with_name("job-smoke.py")
SPEC = importlib.util.spec_from_file_location("job_smoke", SOURCE)
job_smoke = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(job_smoke)


class Clock:
    def __init__(self):
        self.now = 0

    def monotonic(self):
        self.now += 1
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class CleanupTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="slurm-cleanup-unit-")
        self.addCleanup(self.directory.cleanup)
        self.process = SimpleNamespace(
            pid=123, poll=Mock(return_value=None), wait=Mock(return_value=0)
        )
        self.os = SimpleNamespace(
            getpgid=Mock(return_value=123),
            killpg=Mock(),
            waitpid=Mock(return_value=(123, 0)),
            close=Mock(),
            WNOHANG=1,
        )
        self.results = {"success": True, "checks": [], "error": "primary failure"}
        self.runtime = Path(self.directory.name)
        mocks = patch.multiple(
            job_smoke,
            time=Clock(),
            os=self.os,
            pty=SimpleNamespace(fork=lambda: (123, 9)),
        )
        mocks.start()
        self.addCleanup(mocks.stop)
        output = patch("builtins.print")
        output.start()
        self.addCleanup(output.stop)

    def finish(self, run):
        job_smoke.cleanup(
            self.runtime,
            Path("/unused"),
            "test-worker",
            [("slurmctld", self.process)],
            [],
            self.results,
            run,
        )
        self.assertEqual(self.results["error"], "primary failure")
        self.assertEqual(
            json.loads((self.runtime / "results.json").read_text()), self.results
        )
        return self.results

    def test_successful_cleanup(self):
        result = self.finish(
            Mock(return_value=SimpleNamespace(returncode=0, stdout="", stderr=""))
        )
        self.assertTrue(result["success"])
        self.os.killpg.assert_called_once_with(123, signal.SIGTERM)

    def test_sigterm_shutdown_is_successful(self):
        self.process.wait.return_value = -signal.SIGTERM
        result = self.finish(
            Mock(return_value=SimpleNamespace(returncode=0, stdout="", stderr=""))
        )
        self.assertTrue(result["success"])

    def test_nonzero_shutdown_is_not_hidden(self):
        self.process.wait.return_value = 23
        result = self.finish(
            Mock(return_value=SimpleNamespace(returncode=0, stdout="", stderr=""))
        )
        self.assertFalse(result["success"])
        self.assertIn("slurmctld shutdown exited 23", result["cleanup_errors"])

    def test_reaped_failed_daemon_is_reported_without_signalling(self):
        self.process.poll.return_value = -signal.SIGSEGV
        result = self.finish(
            Mock(return_value=SimpleNamespace(returncode=0, stdout="", stderr=""))
        )
        self.assertFalse(result["success"])
        self.assertIn(
            f"slurmctld shutdown exited {-signal.SIGSEGV}", result["cleanup_errors"]
        )
        self.process.wait.assert_not_called()
        self.os.getpgid.assert_not_called()
        self.os.killpg.assert_not_called()

    def test_forced_kill_is_not_successful_cleanup(self):
        self.process.wait.side_effect = [
            subprocess.TimeoutExpired("slurmctld", 8),
            -signal.SIGKILL,
        ]
        result = self.finish(
            Mock(return_value=SimpleNamespace(returncode=0, stdout="", stderr=""))
        )
        self.assertFalse(result["success"])
        self.assertIn(
            "slurmctld shutdown timed out after SIGTERM", result["cleanup_errors"]
        )
        self.assertEqual(
            [call.args for call in self.os.killpg.call_args_list],
            [(123, signal.SIGTERM), (123, signal.SIGKILL)],
        )

    def test_daemon_reaped_at_shutdown_deadline_is_not_signalled_again(self):
        self.process.poll.side_effect = [None, 0]
        self.process.wait.side_effect = subprocess.TimeoutExpired("slurmctld", 8)
        result = self.finish(
            Mock(return_value=SimpleNamespace(returncode=0, stdout="", stderr=""))
        )
        self.assertFalse(result["success"])
        self.os.killpg.assert_called_once_with(123, signal.SIGTERM)
        self.os.getpgid.assert_called_once_with(123)
        self.process.wait.assert_called_once_with(timeout=8)

    def test_failed_daemon_reaped_at_shutdown_deadline_is_not_signalled_again(self):
        self.process.poll.side_effect = [None, 23]
        self.process.wait.side_effect = subprocess.TimeoutExpired("slurmctld", 8)
        result = self.finish(
            Mock(return_value=SimpleNamespace(returncode=0, stdout="", stderr=""))
        )
        self.assertFalse(result["success"])
        self.assertIn("slurmctld shutdown exited 23", result["cleanup_errors"])
        self.os.killpg.assert_called_once_with(123, signal.SIGTERM)

    def test_clean_exit_after_forced_kill_still_fails_shutdown_deadline(self):
        self.process.wait.side_effect = [
            subprocess.TimeoutExpired("slurmctld", 8),
            0,
        ]
        result = self.finish(
            Mock(return_value=SimpleNamespace(returncode=0, stdout="", stderr=""))
        )
        self.assertFalse(result["success"])
        self.assertEqual(
            ["slurmctld shutdown timed out after SIGTERM"], result["cleanup_errors"]
        )

    def test_changed_process_group_prevents_forced_kill(self):
        self.os.getpgid.side_effect = [123, 456]
        self.process.wait.side_effect = subprocess.TimeoutExpired("slurmctld", 8)
        result = self.finish(
            Mock(return_value=SimpleNamespace(returncode=0, stdout="", stderr=""))
        )
        self.assertFalse(result["success"])
        self.os.killpg.assert_called_once_with(123, signal.SIGTERM)
        self.assertTrue(
            any(
                "lost its owned process group" in error
                for error in result["cleanup_errors"]
            )
        )

    def test_failed_cancel_retries_controller_and_stays_failed(self):
        run = Mock(
            side_effect=[
                SimpleNamespace(returncode=1, stdout="", stderr="failed"),
                SimpleNamespace(returncode=0, stdout="", stderr=""),
                SimpleNamespace(returncode=0, stdout="", stderr=""),
            ]
        )
        result = self.finish(run)
        self.assertFalse(result["success"])
        self.assertIn("--ctld", run.call_args_list[1].args[0])
        self.assertTrue(result["cleanup_errors"])
        self.os.killpg.assert_called_once()

    def test_failed_queue_verification_still_stops_daemons(self):
        run = Mock(
            side_effect=[
                SimpleNamespace(returncode=0, stdout="", stderr=""),
                SimpleNamespace(returncode=2, stdout="", stderr="offline"),
            ]
        )
        result = self.finish(run)
        self.assertFalse(result["success"])
        self.assertIn("verification exited 2", result["cleanup_errors"][0])
        self.os.killpg.assert_called_once()

    def test_rpc_timeout_is_not_hidden(self):
        run = Mock(
            side_effect=[
                subprocess.TimeoutExpired("scancel", 15),
                SimpleNamespace(returncode=0, stdout="", stderr=""),
                SimpleNamespace(returncode=0, stdout="", stderr=""),
            ]
        )
        self.assertFalse(self.finish(run)["success"])
        self.os.killpg.assert_called_once()

    def test_pending_jobs_timeout(self):
        run = Mock(return_value=SimpleNamespace(returncode=0, stdout="123", stderr=""))
        result = self.finish(run)
        self.assertFalse(result["success"])
        self.assertIn("did not finish cancellation", result["cleanup_errors"][0])

    def test_wrong_process_group_is_never_signalled(self):
        self.os.getpgid.return_value = 456
        result = self.finish(
            Mock(return_value=SimpleNamespace(returncode=0, stdout="", stderr=""))
        )
        self.assertFalse(result["success"])
        self.os.killpg.assert_not_called()

    def test_reaped_daemon_is_never_signalled(self):
        self.process.poll.return_value = 0
        self.finish(
            Mock(return_value=SimpleNamespace(returncode=0, stdout="", stderr=""))
        )
        self.os.getpgid.assert_not_called()
        self.os.killpg.assert_not_called()

    def test_automatic_child_reaping_is_rejected_before_launch(self):
        with (
            patch.object(
                job_smoke.sys,
                "argv",
                [str(SOURCE), "--prefix=/unused", "--munge-bin=/unused"],
            ),
            patch.object(job_smoke.signal, "getsignal", return_value=signal.SIG_IGN),
            patch.object(
                job_smoke.argparse.ArgumentParser, "error", side_effect=ValueError
            ) as parser_error,
        ):
            with self.assertRaises(ValueError):
                job_smoke.main()
        parser_error.assert_called_once_with(
            "Default SIGCHLD handling is required for owned-process cleanup"
        )

    def test_pty_reaped_leader_is_never_signalled(self):
        with self.assertRaisesRegex(RuntimeError, "PTY job timed out"):
            job_smoke.run_pty(
                ["/unused"],
                runtime=self.runtime,
                env={},
                results=self.results,
                timeout=0,
            )
        self.os.killpg.assert_not_called()
        self.os.close.assert_called_once_with(9)

    def test_pty_wait_error_does_not_leak_master_or_hide_timeout(self):
        self.os.waitpid.side_effect = ChildProcessError("already reaped")
        with self.assertRaisesRegex(RuntimeError, "PTY job timed out"):
            job_smoke.run_pty(
                ["/unused"],
                runtime=self.runtime,
                env={},
                results=self.results,
                timeout=0,
            )
        self.assertFalse(self.results["success"])
        self.os.killpg.assert_not_called()
        self.os.close.assert_called_once_with(9)


if __name__ == "__main__":
    unittest.main()
