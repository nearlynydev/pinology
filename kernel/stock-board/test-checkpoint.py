#!/usr/bin/env python3
"""Offline recovery-tool safety checks without touching a real disk."""
import fcntl
from pathlib import Path
import tempfile
import unittest
from checkpoint import clone


class CheckpointTests(unittest.TestCase):
    def test_existing_destination_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaisesRegex(ValueError, 'exists'):
                clone(root, root)

    def test_running_instance_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with (root / 'run.lock').open('w') as lock:
                fcntl.flock(lock, fcntl.LOCK_EX)
                with self.assertRaisesRegex(ValueError, 'running'):
                    clone(root, root / 'new')
            self.assertFalse((root / 'new').exists())

    def test_symlink_lock_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'other').touch()
            (root / 'run.lock').symlink_to(root / 'other')
            with self.assertRaises(OSError):
                clone(root, root / 'new')

    def test_between_boots_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with (root / 'supervisor.lock').open('w') as lock:
                fcntl.flock(lock, fcntl.LOCK_EX)
                with self.assertRaisesRegex(ValueError, 'running'):
                    clone(root, root / 'new')
            self.assertFalse((root / 'new').exists())


if __name__ == '__main__':
    unittest.main()
