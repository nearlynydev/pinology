#!/usr/bin/env python3
"""Configuration, model-selection and offline-growth safety regressions."""
import argparse
import fcntl
import json
from pathlib import Path
import tempfile
import subprocess
import urllib.parse
import unittest
from unittest.mock import patch
import settings
import profiles
import instance
import guest_control
import storage


def manifest(model='DS223', fmt='qcow2', size=32 * 1024**3):
    spec = profiles.profile(model)
    return dict(model=model, build=spec['build'], hashes=spec['hashes'],
                pat_sha256=spec['pat_sha256'], disk='disk.' + fmt,
                disk_format=fmt, disk_size=size)


class SettingsTests(unittest.TestCase):
    def test_ipv4_bind_addresses(self):
        for address in ('127.0.0.1', '0.0.0.0', '192.0.2.10'):
            self.assertEqual(settings.load(environ={'BIND_ADDRESS': address})['BIND_ADDRESS'], address)
        for address in ('localhost', '::1', '224.0.0.1', '255.255.255.255', '0.1.2.3',
                        '169.254.1.1', '192.0.2.10,hostfwd=tcp::22-:22', '192.0.2.10:5000'):
            with self.subTest(address=address), self.assertRaises(ValueError):
                settings.load(environ={'BIND_ADDRESS': address})

    def test_api_endpoint_tracks_bind_with_legacy_and_container_fallback(self):
        for address, expected in ((None, '127.0.0.1'), ('0.0.0.0', '127.0.0.1'),
                                  ('192.0.2.10', '192.0.2.10')):
            state = {'port': 15504}
            if address is not None:
                state['bind_address'] = address
            self.assertEqual(guest_control.api_endpoint(state), f'http://{expected}:15504')
        for state in ({'port': True}, {'port': 80}, {'port': 15504, 'bind_address': 'other-host'}):
            with self.assertRaises(ValueError):
                guest_control.api_endpoint(state)

    def test_health_uses_native_bind_address(self):
        from unittest.mock import MagicMock
        opener = MagicMock()
        opener.open.return_value.__enter__.return_value.read.return_value = b'{"success":true,"data":{"SYNO.API.Auth":{}}}'
        with patch.object(guest_control, 'qmp_status', return_value={'running': True}), \
                patch.object(guest_control, 'read_state', return_value={'port': 15504, 'bind_address': '192.0.2.10'}), \
                patch.object(guest_control.urllib.request, 'build_opener', return_value=opener):
            self.assertEqual(guest_control.health(Path('/unused'))[0], 0)
        self.assertTrue(opener.open.call_args.args[0].startswith('http://192.0.2.10:15504/'))

    def test_qmp_identity_follows_verified_model_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for model in ('DS223', 'DS423'):
                (root / 'instance.json').write_text(json.dumps(manifest(model)))
                self.assertEqual(guest_control.vm_name(root), model.lower() + '-stock-72806')
            wrong = manifest('DS223')
            wrong['model'] = 'DS423'
            (root / 'instance.json').write_text(json.dumps(wrong))
            with self.assertRaises(ValueError):
                guest_control.vm_name(root)

    def test_growth_failure_keeps_recovery_journal(self):
        with tempfile.TemporaryDirectory() as tmp:
            root, backup = Path(tmp) / 'source', Path(tmp) / 'backup'
            root.mkdir()
            original = json.dumps(manifest())
            (root / 'instance.json').write_text(original)
            (root / 'disk.qcow2').write_bytes(b'disposable fixture')
            def checkpoint(source, target, qemu):
                target.mkdir()
                hashes = {name: instance.digest(source / name) for name in ('instance.json', 'disk.qcow2')}
                (target / 'checkpoint.json').write_text(json.dumps({'sha256': hashes}))
            with patch.object(storage, 'clone', side_effect=checkpoint), \
                    patch.object(storage.subprocess, 'run', side_effect=subprocess.CalledProcessError(1, 'resize')):
                with self.assertRaises(subprocess.CalledProcessError):
                    storage.grow_disk(root, 40 * 1024**3, backup)
            self.assertTrue((root / 'resize.pending.json').is_file())
            self.assertEqual((root / 'instance.json').read_text(), original)

    def test_changed_source_after_checkpoint_never_resized(self):
        with tempfile.TemporaryDirectory() as tmp:
            root, backup = Path(tmp) / 'source', Path(tmp) / 'backup'
            root.mkdir()
            (root / 'instance.json').write_text(json.dumps(manifest()))
            (root / 'disk.qcow2').write_bytes(b'before')
            def checkpoint(source, target, qemu):
                target.mkdir()
                hashes = {'disk.qcow2': instance.digest(source / 'disk.qcow2')}
                (target / 'checkpoint.json').write_text(json.dumps({'sha256': hashes}))
                (source / 'disk.qcow2').write_bytes(b'changed')
            with patch.object(storage, 'clone', side_effect=checkpoint), \
                    patch.object(storage.subprocess, 'run') as resize:
                with self.assertRaisesRegex(ValueError, 'changed'):
                    storage.grow_disk(root, 40 * 1024**3, backup)
                resize.assert_not_called()
            self.assertFalse((root / 'resize.pending.json').exists())

    def test_shutdown_response_loss_is_unknown_not_success(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            calls = []
            class Response:
                def __enter__(self): return self
                def __exit__(self, *args): pass
                def read(self, size): return b'{"success":true,"data":{"sid":"fixture"}}'
            class Opener:
                def open(self, request, timeout):
                    params = urllib.parse.parse_qs(request.data.decode())
                    calls.append(params['method'][0])
                    if request.full_url != 'http://192.0.2.10:15504/webapi/entry.cgi':
                        raise AssertionError('Shutdown ignored the native bind address')
                    if calls[-1] == 'shutdown':
                        raise TimeoutError('response lost')
                    return Response()
            with patch.object(guest_control, 'qmp_status', return_value={'running': True}), \
                    patch.object(guest_control, 'read_state', return_value={'port': 15504, 'network': 'user', 'bind_address': '192.0.2.10'}), \
                    patch.object(guest_control, 'credentials', return_value={'account': 'fixture', 'password': 'fixture'}), \
                    patch.object(guest_control.urllib.request, 'build_opener', return_value=Opener()):
                self.assertIs(guest_control.poweroff(root, root / 'fixture'), False)
            self.assertEqual(calls, ['login', 'shutdown'])

    def test_defaults_and_models(self):
        self.assertEqual(settings.load(environ={})['EXPERIMENTAL_DS423'], 'N')
        self.assertEqual(settings.load(environ={'EXPERIMENTAL_DS423': 'Y'})['EXPERIMENTAL_DS423'], 'Y')
        self.assertEqual(settings.load(environ={})['MODEL'], 'DS223')
        self.assertEqual(settings.load(environ={'MODEL': 'DS423'})['MODEL'], 'DS423')
        for model in ('DS423+', 'DS224', ''):
            with self.assertRaises(ValueError):
                settings.load(environ={'MODEL': model})

    def test_precedence_and_non_executable(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'settings'
            path.write_text('HTTP_PORT=15507\nMODEL="DS423"\nSTORAGE=$(touch /not-executed)\n')
            conf = settings.load(path, {'HTTP_PORT': '15508'})
            self.assertEqual(conf['HTTP_PORT'], '15508')
            self.assertEqual(conf['MODEL'], 'DS423')
            self.assertEqual(conf['STORAGE'], '$(touch /not-executed)')
            path.write_text('UNSAFE_OPTION=1')
            with self.assertRaises(ValueError):
                settings.load(path, {})

    def test_invalid_settings(self):
        for key, value in [('CPU_CORES', '8'), ('RAM_SIZE', '4G'), ('DISK_SIZE', '7G'),
                           ('DISK_SIZE', '17T'), ('DISK_FMT', 'vmdk'), ('ALLOCATE', 'yes'),
                           ('MAC', 'ff:ff:ff:ff:ff:ff'), ('MAC', '00:00:00:00:00:00'),
                           ('NETWORK', 'host'), ('HTTP_PORT', '80'), ('TIMEOUT', '0'),
                           ('STARTUP_TIMEOUT', 'abc'), ('USER_PORTS', 'tcp:15504:80'),
                           ('EXPERIMENTAL_DS423', 'yes')]:
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                settings.load(environ={key: value})

    def test_port_protocol_namespaces(self):
        self.assertEqual(settings.forwards(15504, 14445, 'tcp:15508:5000,udp:15504:53'),
                         [('tcp',15504,5000),('tcp',14445,445),('tcp',15508,5000),('udp',15504,53)])

    def test_legacy_manifest(self):
        value = manifest()
        for name in ('model', 'disk_format', 'disk_size'):
            del value[name]
        model, _, fmt, size = profiles.validate_manifest(value)
        self.assertEqual((model, fmt, size), ('DS223', 'qcow2', 32 * 1024**3))

    def test_cross_model_pat_rejected(self):
        value = manifest()
        value['model'] = 'DS423'
        with self.assertRaises(ValueError):
            profiles.validate_manifest(value)

    def test_unimplemented_board_refused_before_launch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'instance.json').write_text(json.dumps(manifest('DS423')))
            with patch.object(instance.subprocess, 'Popen') as launch:
                with self.assertRaisesRegex(ValueError, 'PCIe'):
                    instance.run(argparse.Namespace(instance=root))
                launch.assert_not_called()

    def test_resize_journal_blocks_boot(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'resize.pending.json').write_text('{}')
            with self.assertRaisesRegex(ValueError, 'Unfinished'):
                instance.run(argparse.Namespace(instance=root))

    def test_no_shrink_or_noop(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'instance.json').write_text(json.dumps(manifest()))
            for size in (16 * 1024**3, 32 * 1024**3):
                with self.assertRaisesRegex(ValueError, 'Growth only'):
                    storage.grow_disk(root, size, root / 'checkpoint')
            self.assertFalse((root / 'checkpoint').exists())

    def test_credentials_permissions_and_symlink(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'credentials'
            path.write_text('{"account":"test","password":"public-fixture"}')
            path.chmod(0o644)
            with self.assertRaises(ValueError):
                guest_control.credentials(path)
            path.chmod(0o600)
            self.assertEqual(guest_control.credentials(path)['account'], 'test')
            link = Path(tmp) / 'link'
            link.symlink_to(path)
            with self.assertRaises(OSError):
                guest_control.credentials(link)

    def test_stopped_uses_lifetime_locks(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.assertTrue(guest_control.stopped(root))
            for name in ('run.lock', 'supervisor.lock'):
                with (root / name).open('r+') as lock:
                    fcntl.flock(lock, fcntl.LOCK_EX)
                    self.assertFalse(guest_control.stopped(root))
            self.assertTrue(guest_control.stopped(root))

    def test_offline_health_never_claims_ready(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'runtime.json').write_text(json.dumps(dict(port=15504, network='N')))
            with patch.object(guest_control, 'qmp_status', return_value={'running': True}):
                self.assertEqual(guest_control.health(root)[0], 1)


if __name__ == '__main__':
    unittest.main()
