#!/usr/bin/env python3
"""Deterministic cleanup protocol tests; no cluster or privilege operations.

Distributed under Slurm's GNU GPL version 2 or later.
"""

import importlib.util
import select
import signal
import subprocess
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

spec = importlib.util.spec_from_file_location(
    "privileged_smoke", Path(__file__).with_name("privileged-smoke.py")
)
smoke = importlib.util.module_from_spec(spec)
spec.loader.exec_module(smoke)


class Cleanup(unittest.TestCase):
    def run_case(self, responses, jobs=()):
        commands = []

        def run(command, **options):
            commands.append([str(value) for value in command])
            code, output = responses.pop(0)
            return subprocess.CompletedProcess(command, code, output, "fixture")

        with patch.object(smoke.time, "sleep"), patch.object(
            smoke.time, "monotonic", side_effect=range(50)
        ):
            result = smoke.drain_private_jobs(
                run, Path("/private"), "test", set(jobs), 3
            )
        self.assertFalse(responses)
        return result, commands

    def test_failed_empty_query_is_not_success(self):
        with self.assertRaisesRegex(RuntimeError, "verifiably drain"):
            self.run_case([(1, "")] * 3)

    def test_cancel_failure_uses_controller_and_observes_empty(self):
        attempts, commands = self.run_case(
            [(0, "7|test-native\n"), (1, ""), (0, ""), (0, "")]
        )
        self.assertEqual(commands[2], ["/private/bin/scancel", "--ctld", "7"])
        self.assertEqual(len(commands), 4)
        self.assertEqual(attempts[0]["returncode"], 1)

    def test_both_cancellation_paths_fail(self):
        with self.assertRaisesRegex(RuntimeError, "cancellation failed"):
            self.run_case([(0, "7|test-native\n"), (1, ""), (1, "")])

    def test_failed_query_recovers_recorded_job_and_retains_failure(self):
        attempts, commands = self.run_case([(1, ""), (0, ""), (0, "")], ["7"])
        self.assertIn("queue_error", attempts[0])
        self.assertEqual(commands[1], ["/private/bin/scancel", "7"])

    def test_live_job_after_cancel_is_not_drained(self):
        with self.assertRaisesRegex(RuntimeError, "verifiably drain"):
            self.run_case(
                [(0, "7|test-native\n"), (0, "")] + [(0, "7|test-native\n")] * 2
            )

    def test_successful_empty_queue_needs_no_terminal_job_cancel(self):
        _, commands = self.run_case([(0, "")], ["7"])
        self.assertEqual(len(commands), 1)


class GroupCleanup(unittest.TestCase):
    def process(self, status=None):
        return SimpleNamespace(pid=987654, returncode=status, wait=Mock())

    def signal_group(self, pid, number):
        if number == 0:
            raise ProcessLookupError

    def test_clean_and_sigterm_statuses_pass(self):
        for status in (0, -signal.SIGTERM):
            process = self.process()
            process.wait.return_value = status
            with self.subTest(status=status), patch.object(
                smoke.os, "killpg", side_effect=self.signal_group
            ) as kill:
                smoke.stop_group(process)
                self.assertEqual(
                    [call.args[1] for call in kill.call_args_list],
                    [signal.SIGTERM, 0],
                )

    def test_already_reaped_failure_is_not_signalled(self):
        process = self.process(23)
        with patch.object(smoke.os, "killpg", side_effect=self.signal_group) as kill:
            with self.assertRaisesRegex(RuntimeError, "shutdown exited 23"):
                smoke.stop_group(process)
            self.assertEqual([call.args[1] for call in kill.call_args_list], [0])
        process.wait.assert_not_called()

    def test_abnormal_shutdown_status_fails(self):
        for status in (23, -signal.SIGSEGV, -signal.SIGKILL):
            process = self.process()
            process.wait.return_value = status
            with self.subTest(status=status), patch.object(
                smoke.os, "killpg", side_effect=self.signal_group
            ), self.assertRaisesRegex(RuntimeError, "shutdown exited"):
                smoke.stop_group(process)

    def test_timeout_requires_failure_even_when_forced_cleanup_finishes(self):
        for status in (0, -signal.SIGKILL):
            process = self.process()
            process.wait.side_effect = [subprocess.TimeoutExpired("fixture", 5), status]
            with self.subTest(status=status), patch.object(
                smoke.os, "killpg", side_effect=self.signal_group
            ) as kill:
                with self.assertRaisesRegex(RuntimeError, "shutdown timed out"):
                    smoke.stop_group(process)
                self.assertEqual(
                    [call.args[1] for call in kill.call_args_list],
                    [signal.SIGTERM, signal.SIGKILL, 0],
                )

    def test_timeout_cannot_signal_an_already_reaped_identity(self):
        process = self.process()

        def wait(**options):
            process.returncode = 0
            raise subprocess.TimeoutExpired("fixture", 5)

        process.wait.side_effect = wait
        with patch.object(smoke.os, "killpg", side_effect=self.signal_group) as kill:
            with self.assertRaisesRegex(RuntimeError, "shutdown timed out"):
                smoke.stop_group(process)
            self.assertEqual(
                [call.args[1] for call in kill.call_args_list],
                [signal.SIGTERM, 0],
            )

    def test_surviving_group_after_reap_is_only_probed(self):
        process = self.process(0)
        with patch.object(smoke.os, "killpg") as kill, patch.object(
            smoke.time, "monotonic", side_effect=[0, 0, 4]
        ), patch.object(smoke.time, "sleep"):
            with self.assertRaisesRegex(RuntimeError, "unverified identity"):
                smoke.stop_group(process)
            self.assertEqual([call.args[1] for call in kill.call_args_list], [0])

    def test_real_child_exit_23_after_sigterm_is_reported(self):
        process = subprocess.Popen(
            [
                sys.executable,
                "-I",
                "-c",
                "import signal,time\n"
                "signal.signal(signal.SIGTERM, lambda *_: exit(23))\n"
                "print('ready', flush=True)\ntime.sleep(10)\n",
            ],
            start_new_session=True,
            stdout=subprocess.PIPE,
            text=True,
        )
        try:
            self.assertTrue(select.select([process.stdout], [], [], 3)[0])
            self.assertEqual(process.stdout.readline(), "ready\n")
            with self.assertRaisesRegex(RuntimeError, "shutdown exited 23"):
                smoke.stop_group(process)
            self.assertEqual(process.returncode, 23)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=3)
            process.stdout.close()


if __name__ == "__main__":
    unittest.main()
