#!/usr/bin/env python3
"""Test canary liveness and shutdown without root or Slurm daemons.

Distributed under Slurm's GNU GPL version 2 or later.
"""

import importlib.util
import os
import selectors
import signal
import subprocess
import sys
import time
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "privileged_smoke", Path(__file__).with_name("privileged-smoke.py")
)
smoke = importlib.util.module_from_spec(spec)
spec.loader.exec_module(smoke)


def canary_progress(process, selector):
    return smoke.CanaryMonitor(process, selector, os.getuid(), os.getgid()).progress


class ExitAfterPhaseTwoReadiness:
    """Make a final queued heartbeat and a completed child deterministic."""

    def __init__(self, selector, process):
        self.selector = selector
        self.process = process
        self.enabled = False
        self.phase_two = False
        self.exercised = False

    def select(self, timeout=None):
        events = self.selector.select(timeout)
        if self.enabled and timeout == 0 and not events:
            self.phase_two = True
        elif self.enabled and self.phase_two and events and not self.exercised:
            # This process is our own fixed same-user child, and waiting here
            # proves it has exited before the collector consumes the record.
            self.process.stdin.close()
            self.process.wait(timeout=2)
            self.exercised = True
        return events


@unittest.skipIf(
    os.getuid() == 0 or os.geteuid() == 0, "These tests must run as an ordinary user"
)
class CanaryTests(unittest.TestCase):
    def setUp(self):
        self.children = []
        self.selectors = []

    def tearDown(self):
        for process in self.children:
            if process.returncode is None:
                # SIGCONT only targets a still-unreaped child owned by this
                # test, including the explicitly stopped test below.
                try:
                    os.kill(process.pid, signal.SIGCONT)
                except ProcessLookupError:
                    pass
            if process.stdin and not process.stdin.closed:
                process.stdin.close()
            try:
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)
            if process.stdout:
                process.stdout.close()
        for selector in self.selectors:
            selector.close()

    def child(self, *args):
        process = subprocess.Popen(
            [sys.executable, "-I", "-c", smoke.CANARY, *map(str, args)],
            cwd="/",
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        self.children.append(process)
        selector = selectors.DefaultSelector()
        selector.register(process.stdout, selectors.EVENT_READ)
        self.selectors.append(selector)
        return process, selector

    def test_fixed_canary_identity_progress_and_clean_eof(self):
        process, selector = self.child()
        progress = canary_progress(process, selector)
        self.assertEqual(
            progress(ready=True),
            {
                "ready": True,
                "pid": process.pid,
                "uid": os.getuid(),
                "gid": os.getgid(),
            },
        )
        first = progress(timeout=2)
        second = progress(timeout=2)
        self.assertGreater(second["heartbeat"], first["heartbeat"])
        process.stdin.close()
        self.assertEqual(process.wait(timeout=2), 0)

    def test_eof_is_rejected(self):
        process, selector = self.child()
        progress = canary_progress(process, selector)
        progress(ready=True)
        process.stdin.close()
        process.wait(timeout=2)
        with self.assertRaises(RuntimeError):
            progress(timeout=0.4)

    def test_queued_readiness_does_not_qualify_a_dead_canary(self):
        process, selector = self.child()
        progress = canary_progress(process, selector)
        self.assertTrue(selector.select(timeout=2))
        process.stdin.close()
        self.assertEqual(process.wait(timeout=2), 0)
        with self.assertRaises(RuntimeError):
            progress(ready=True, timeout=0.4)

    def test_queued_heartbeats_do_not_qualify_a_stopped_canary(self):
        process, selector = self.child()
        progress = canary_progress(process, selector)
        progress(ready=True)
        time.sleep(0.25)
        os.kill(process.pid, signal.SIGSTOP)
        started = time.monotonic()
        with self.assertRaises(RuntimeError):
            progress(timeout=0.35)
        self.assertLess(time.monotonic() - started, 1.5)

    def test_terminal_heartbeat_does_not_qualify_a_dead_canary(self):
        process, selector = self.child()
        wrapped = ExitAfterPhaseTwoReadiness(selector, process)
        progress = canary_progress(process, wrapped)
        progress(ready=True)
        wrapped.enabled = True
        with self.assertRaises(RuntimeError):
            progress(timeout=2)
        self.assertTrue(
            wrapped.exercised, "The post-readiness exit interleaving was not tested"
        )

    def test_watchdog_bounds_a_full_output_pipe(self):
        process, _ = self.child(1, 0)
        # Deliberately do not drain stdout. A watchdog checked only before a
        # blocking print cannot expire after the anonymous pipe fills.
        try:
            code = process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            self.fail("CANARY watchdog hung behind an unread output pipe")
        self.assertEqual(code, -signal.SIGALRM)
        self.assertGreaterEqual(
            len(process.stdout.read()), 4096, "Test did not exercise buffered output"
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
