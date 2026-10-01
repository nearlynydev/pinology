#!/usr/bin/env python3
"""Initialization routing and no-overwrite regressions (no network/real disk)."""
import argparse
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import instance
import prepare_media
from profiles import profile


class InitializeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.pat = self.root / 'fixture.pat'
        self.pat.write_bytes(b'fixture, not firmware')

    def args(self, model='DS223'):
        return argparse.Namespace(instance=self.root / model, model=model,
                                  pat=self.pat, url=None, artifacts=self.root / 'artifacts')

    def test_model_passed_to_flash_and_prepare(self):
        for model in ('DS223', 'DS423'):
            args = self.args(model)
            def flash(artifacts, output, model, serial):
                self.assertEqual(serial, '')
                self.assertEqual(model, args.model)
                self.assertEqual(artifacts, args.artifacts)
                output.write_bytes(b'flash fixture')
            def prepare(value):
                self.assertIs(value, args)
                self.assertEqual(value.pat.read_bytes(), self.pat.read_bytes())
                value.instance.mkdir()
            with patch.object(instance, 'verify') as verify, \
                 patch.object(instance, 'prepare', side_effect=prepare), \
                 patch.object(prepare_media.importlib.util, 'module_from_spec') as factory:
                factory.return_value.prepare.side_effect = flash
                with patch.object(prepare_media.importlib.util, 'spec_from_file_location'):
                    prepare_media.initialize(args)
                self.assertEqual(verify.call_count, 2)
                self.assertTrue(all(c.args[1] == profile(model)['pat_sha256']
                                    for c in verify.call_args_list))
                self.assertEqual((args.instance / 'flash.bin').read_bytes(), b'flash fixture')

    def test_existing_instance_never_adopted(self):
        args = self.args()
        args.instance.mkdir()
        with self.assertRaisesRegex(ValueError, 'never overwrites'):
            prepare_media.initialize(args)
        self.assertFalse(self.root.joinpath('DS223.media').exists())

    def test_dangling_instance_symlink_refused(self):
        args = self.args()
        args.instance.symlink_to(self.root / 'missing')
        with self.assertRaisesRegex(ValueError, 'never overwrites'):
            prepare_media.initialize(args)

    def test_partial_media_never_reused(self):
        args = self.args()
        self.root.joinpath('DS223.media').mkdir()
        with self.assertRaises(FileExistsError):
            prepare_media.initialize(args)
        self.assertFalse(args.instance.exists())

    def test_wrong_pat_stops_before_disk(self):
        args = self.args('DS423')
        with patch.object(instance, 'prepare') as prepare:
            with self.assertRaisesRegex(ValueError, 'SHA-256 mismatch'):
                prepare_media.initialize(args)
            prepare.assert_not_called()
        self.assertFalse(args.instance.exists())

    def test_bad_flash_stops_before_disk(self):
        args = self.args()
        with patch.object(instance, 'verify'), patch.object(instance, 'prepare') as prepare, \
             patch.object(prepare_media.importlib.util, 'module_from_spec') as factory, \
             patch.object(prepare_media.importlib.util, 'spec_from_file_location'):
            factory.return_value.prepare.side_effect = ValueError('wrong U-Boot')
            with self.assertRaisesRegex(ValueError, 'wrong U-Boot'):
                prepare_media.initialize(args)
            prepare.assert_not_called()

    def test_non_https_source_stops_before_download(self):
        args = self.args()
        args.pat, args.url = None, 'http://example.invalid/firmware.pat'
        with patch.object(prepare_media.urllib.request, 'urlopen') as download:
            with self.assertRaisesRegex(ValueError, 'HTTPS'):
                prepare_media.initialize(args)
            download.assert_not_called()


if __name__ == '__main__':
    unittest.main()
