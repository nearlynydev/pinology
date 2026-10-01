#!/usr/bin/env python3
import fcntl
from pathlib import Path
import tempfile
import unittest
import lease
import guest_control
from checkpoint import clone


class LeaseTests(unittest.TestCase):
    def test_exclusive_and_clean_release(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with lease.hold(root):
                self.assertTrue((root / lease.NAME / 'owner.json').is_file())
                self.assertFalse(guest_control.stopped(root))
                with self.assertRaisesRegex(ValueError, 'active.lease'):
                    with lease.hold(root):
                        self.fail('Nested acquisition')
            self.assertTrue(guest_control.stopped(root))
            self.assertFalse((root / lease.NAME).exists())

    def test_symlink_or_stale_lease_not_stolen(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / lease.NAME).symlink_to(root / 'absent')
            with self.assertRaises(ValueError):
                with lease.hold(root):
                    self.fail('Stolen lease')
            self.assertTrue((root / lease.NAME).is_symlink())

    def test_checkpoint_observes_foreign_lease(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / lease.NAME).mkdir()
            with self.assertRaisesRegex(ValueError, 'active.lease'):
                clone(root, root / 'must-not-exist')
            self.assertFalse((root / 'must-not-exist').exists())

    def test_parent_exit_while_qemu_lock_held_retains_lease(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with (root / 'run.lock').open('w') as lock:
                with lease.hold(root):
                    fcntl.flock(lock, fcntl.LOCK_EX)
                self.assertTrue((root / lease.NAME).exists())
            # No automatic stealing after the process disappears either.
            with self.assertRaises(ValueError):
                with lease.hold(root):
                    self.fail('Stolen stale lease')


if __name__ == '__main__':
    unittest.main()
