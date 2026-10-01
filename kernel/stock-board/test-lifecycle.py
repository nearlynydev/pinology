#!/usr/bin/env python3
"""Fail-closed reboot decisions; no VM or persistent disks required."""
import unittest
import argparse
from pathlib import Path
import tempfile
from unittest.mock import patch
import instance
from lifecycle import should_restart


class LifecycleTests(unittest.TestCase):
    def test_supervisor_exit_codes_cannot_trigger_restart(self):
        with tempfile.TemporaryDirectory() as tmp:
            args = argparse.Namespace(instance=Path(tmp), restart_on_guest_reset=True)
            for code in (0, 1, 75, 137):
                with patch.object(instance, 'run', return_value=code) as run:
                    self.assertEqual(instance.supervise(args), code)
                    run.assert_called_once()

    def test_supervisor_rate_limit(self):
        with tempfile.TemporaryDirectory() as tmp:
            args = argparse.Namespace(instance=Path(tmp), restart_on_guest_reset=True)
            with patch.object(instance, 'run', return_value=instance.GUEST_RESTART) as run:
                self.assertEqual(instance.supervise(args), 70)
                self.assertEqual(run.call_count, 4)

    def test_only_successful_guest_reset(self):
        event = {'event': 'SHUTDOWN', 'data': {'guest': True, 'reason': 'guest-reset'}}
        self.assertTrue(should_restart(0, event))
        for code in (-9, -15, 1, 75):
            self.assertFalse(should_restart(code, event))

    def test_other_shutdowns_never_restart(self):
        for reason in ('guest-shutdown', 'host-qmp-quit', 'host-signal', 'guest-panic', 'watchdog'):
            for guest in (True, False):
                self.assertFalse(should_restart(0, {'event': 'SHUTDOWN', 'data': {'guest': guest, 'reason': reason}}))
        for event in (None, {}, {'event': 'RESET', 'data': {'guest': True, 'reason': 'guest-reset'}},
                      {'event': 'SHUTDOWN', 'data': {'guest': False, 'reason': 'guest-reset'}}):
            self.assertFalse(should_restart(0, event))


if __name__ == '__main__':
    unittest.main()
