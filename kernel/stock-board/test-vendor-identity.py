#!/usr/bin/env python3
"""Synthetic identities only; no network, DSM credentials or existing VM."""
import argparse
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib
import instance
import prepare_media
import settings
import vendor_identity as vendor

spec = importlib.util.spec_from_file_location('flash_media', Path(__file__).with_name('flash-media.py'))
flash_media = importlib.util.module_from_spec(spec)
spec.loader.exec_module(flash_media)
SERIAL = 'LABSERIAL001'  # Deliberately not a plausible production identity.


def nor(serial):
    media = bytearray(b'\xff' * vendor.NOR_SIZE)
    media[vendor.VENDOR_OFFSET:vendor.VENDOR_OFFSET + vendor.VENDOR_SIZE] = vendor.encode_vendor(serial)
    return media


class VendorTests(unittest.TestCase):
    def test_exact_stock_record_and_no_fabricated_mac(self):
        data = vendor.encode_vendor(SERIAL)
        self.assertEqual(data[:16], b'SYNO!!!!' + b'\0' * 8)
        self.assertEqual(data[16:48], b'SN=LABSERIAL001,CHK=800'.ljust(32, b'\0'))
        self.assertEqual(data[48:], b'\xff' * (vendor.VENDOR_SIZE - 48))
        self.assertEqual(vendor.read_serial(nor(SERIAL)), SERIAL)
        self.assertEqual(vendor.read_serial(nor('')), '')

    def test_validation_and_record_capacity(self):
        for value in ('a123', 'AB CD', 'AB,CHK=1', 'AB\0C', 'СЕРИЙНЫЙ', 'A\n', 'A' * 21, 'Z' * 20, None):
            with self.subTest(value=value), self.assertRaises(ValueError):
                vendor.validate_serial(value)
        for value in ('', SERIAL, 'Z' * 19, '0' * 20):
            self.assertEqual(vendor.validate_serial(value), value)

    def test_corrupt_record_header_and_unterminated_rejected(self):
        for offset in (vendor.VENDOR_OFFSET, vendor.VENDOR_OFFSET + 19, vendor.VENDOR_OFFSET + 16 + 21):
            data = nor(SERIAL)
            data[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                vendor.read_serial(data)
        data = nor(SERIAL)
        data[vendor.VENDOR_OFFSET + 16:vendor.VENDOR_OFFSET + 48] = b'A' * 32
        with self.assertRaisesRegex(ValueError, 'Unterminated'):
            vendor.read_serial(data)

    def test_guard_persists_across_boot_payload_updates(self):
        data = nor(SERIAL)
        manifest = {'identity': vendor.identity(SERIAL)}
        vendor.check_identity(data, manifest)
        data[0x190000:0x190008] = b'newImage'
        vendor.check_identity(data, manifest, SERIAL)
        self.assertEqual(vendor.read_serial(data), SERIAL)
        for changed in (nor(''), nor('LABSERIAL002')):
            with self.assertRaises(ValueError):
                vendor.check_identity(changed, manifest)
        with self.assertRaisesRegex(ValueError, 'creation-only'):
            vendor.check_identity(data, manifest, 'LABSERIAL002')
        with self.assertRaises(ValueError):
            vendor.check_identity(None, manifest)
        for invalid in (None, {}, {'vendor_format': 1}):
            with self.assertRaises(ValueError):
                vendor.check_identity(data, {'identity': invalid})

    def test_legacy_untouched_and_empty_identity_pinned(self):
        vendor.check_identity(b'legacy', {})
        vendor.check_identity(nor(SERIAL), {}, SERIAL)
        with self.assertRaises(ValueError):
            vendor.check_identity(nor(SERIAL), {'identity': vendor.identity('')})

    def test_flash_both_models_changes_only_vendor_and_its_fis_entry(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in ('uboot_DS223.bin', 'uboot_DS423.bin', 'zImage', 'model.dtb', 'rd.bin'):
                (root / name).write_bytes(name.encode())
            for model in ('DS223', 'DS423'):
                plain, named = root / (model + '-plain'), root / (model + '-serial')
                with patch.object(flash_media.instance, 'verify'):
                    flash_media.prepare(root, plain, model)
                    flash_media.prepare(root, named, model, SERIAL)
                    with self.assertRaises(FileExistsError):
                        flash_media.prepare(root, named, model, 'LABSERIAL002')
                original, data = plain.read_bytes(), named.read_bytes()
                self.assertEqual(data[:vendor.VENDOR_OFFSET], original[:vendor.VENDOR_OFFSET])
                self.assertEqual(data[0xfe5000:0xfff400], original[0xfe5000:0xfff400])
                self.assertEqual(data[0xfff500:], original[0xfff500:])
                self.assertEqual(vendor.read_serial(data), SERIAL)
                self.assertEqual(struct.unpack_from('<I', data, 0xfff400 + 32)[0], vendor.VENDOR_SIZE)
                self.assertEqual(struct.unpack_from('<I', data, 0xfff400 + 252)[0], zlib.crc32(vendor.encode_vendor(SERIAL)))
                self.assertNotIn(SERIAL, json.dumps(vendor.identity(SERIAL)))

    def test_invalid_serial_stops_before_init_writes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'new'
            with self.assertRaises(ValueError):
                prepare_media.initialize(argparse.Namespace(instance=root, serial='bad value'))
            self.assertFalse(root.exists())
            self.assertFalse(root.with_name('new.media').exists())

    def test_config_cli_routing_and_redaction(self):
        self.assertEqual(settings.load(environ={'SERIAL': SERIAL})['SERIAL'], SERIAL)
        for model in ('DS223', 'DS423'):
            with patch.dict('os.environ', {'SERIAL': 'LABSERIAL002'}, clear=True), \
                 patch.object(sys, 'argv', ['instance.py', 'init', '/nonexistent/new', '--model', model, '--serial', SERIAL]), \
                 patch.object(prepare_media, 'initialize') as init:
                instance.main()
                self.assertEqual(init.call_args.args[0].serial, SERIAL)
                self.assertEqual(init.call_args.args[0].model, model)
        output = io.StringIO()
        with patch.dict('os.environ', {'SERIAL': SERIAL}, clear=True), \
             patch.object(sys, 'argv', ['instance.py', 'config']), contextlib.redirect_stdout(output):
            instance.main()
        self.assertEqual(json.loads(output.getvalue())['SERIAL'], '<set>')
        self.assertNotIn(SERIAL, output.getvalue())

    def test_run_rejects_changed_identity_before_qemu(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            profile = instance.profile('DS223')
            manifest = dict(model='DS223', build=profile['build'], hashes=profile['hashes'],
                            pat_sha256=profile['pat_sha256'], disk='disk.qcow2', identity=vendor.identity(SERIAL))
            (root / 'instance.json').write_text(json.dumps(manifest))
            (root / 'disk.qcow2').write_bytes(b'fixture')
            (root / 'flash.bin').write_bytes(nor('LABSERIAL002'))
            args = argparse.Namespace(instance=root, qemu=sys.executable, qemu_img='unused',
                                      flash=True, port=15504, accel='tcg')
            with patch.object(instance, 'verify'), \
                 patch.object(instance.subprocess, 'check_output', return_value=json.dumps({'format': 'qcow2', 'virtual-size': 32 * 1024**3})), \
                 patch.object(instance.subprocess, 'Popen') as launch:
                with self.assertRaisesRegex(ValueError, 'identity changed'):
                    instance.run(args)
                launch.assert_not_called()
            self.assertEqual(vendor.read_serial((root / 'flash.bin').read_bytes()), 'LABSERIAL002')


if __name__ == '__main__':
    unittest.main()
