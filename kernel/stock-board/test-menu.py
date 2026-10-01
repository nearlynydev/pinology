import contextlib
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch, Mock

import menu
import profiles


class MenuTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='DSM menu test ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.bundle = self.root / 'bundle with spaces'
        self.item = dict(path=str(self.root), http=15520, smb=14460,
                         experimental=False, nic2=False)
        self.manifest('DS223')

    def manifest(self, model):
        spec = profiles.profile(model)
        (self.root / 'instance.json').write_text(json.dumps(dict(
            model=model, build=spec['build'], hashes=spec['hashes'],
            pat_sha256=spec['pat_sha256'], disk='disk.qcow2')))

    def test_hvf_loopback_and_space_safe_argv(self):
        args = menu.run_command(self.bundle, self.item)
        self.assertEqual(args[:3], [str(self.bundle / 'dsm'), 'run', str(self.root)])
        for value in ('hvf', '127.0.0.1', '--flash', '--console-socket', '--restart-on-guest-reset'):
            self.assertIn(value, args)
        self.assertNotIn('--experimental-ds423', args)

    def test_ds423_requires_opt_in(self):
        self.manifest('DS423')
        with self.assertRaises(ValueError):
            menu.run_command(self.bundle, self.item)
        self.item.update(experimental=True, nic2=True)
        args = menu.run_command(self.bundle, self.item)
        self.assertIn('--experimental-ds423', args)
        self.assertIn('--experimental-second-nic', args)

    def test_nic2_rejected_for_ds223(self):
        self.item['nic2'] = True
        with self.assertRaises(ValueError):
            menu.validate(self.item)

    def test_invalid_ports(self):
        for port in (True, 80, 65536, '15520', 14460):
            with self.subTest(port=port), self.assertRaises(ValueError):
                menu.validate(dict(self.item, http=port))

    def test_environment_isolation(self):
        with patch.dict(os.environ, {'ACCEL': 'tcg', 'BIND_ADDRESS': '0.0.0.0',
                                     'SERIAL': 'INHERITED', 'PATH': '/bin'}):
            env = menu.clean_env()
        self.assertNotIn('ACCEL', env)
        self.assertNotIn('BIND_ADDRESS', env)
        self.assertNotIn('SERIAL', env)
        self.assertEqual(env['PATH'], '/bin')

    def create_inputs(self, serial='', pat='да', prepare='да'):
        new_root = self.root / 'new instance'
        return new_root, [
            'DS223', '32G', serial, pat, prepare,
        ]

    def test_create_without_serial_omits_init_option(self):
        new_root, inputs = self.create_inputs()
        with patch.object(menu, 'ask', side_effect=inputs), \
                patch.object(menu, 'ask_path', return_value=new_root), \
                patch.object(menu.subprocess, 'run') as run, \
                patch.object(menu, 'configure', return_value=self.item), \
                patch.object(menu.vendor_identity, 'validate_serial', return_value='') as validate:
            result = menu.create(self.bundle)
        self.assertEqual(result, self.item)
        validate.assert_called_once_with('')
        run.assert_called_once()
        self.assertNotIn('--serial', run.call_args.args[0])

    def test_create_passes_validated_serial_only_to_init(self):
        new_root, inputs = self.create_inputs('AB12CD')
        with patch.object(menu, 'ask', side_effect=inputs), \
                patch.object(menu, 'ask_path', return_value=new_root), \
                patch.object(menu.subprocess, 'run') as run, \
                patch.object(menu, 'configure', return_value=self.item), \
                patch.object(menu.vendor_identity, 'validate_serial', return_value='AB12CD') as validate:
            menu.create(self.bundle)
        validate.assert_called_once_with('AB12CD')
        init_args = run.call_args.args[0]
        self.assertEqual(init_args[1:3], ['init', str(new_root)])
        self.assertEqual(init_args[init_args.index('--serial') + 1], 'AB12CD')

    def test_create_rejects_invalid_serial_before_init(self):
        new_root, inputs = self.create_inputs('bad-serial')
        with patch.object(menu, 'ask', side_effect=inputs), \
                patch.object(menu, 'ask_path', return_value=new_root), \
                patch.object(menu.subprocess, 'run') as run, \
                patch.object(menu.vendor_identity, 'validate_serial',
                             side_effect=ValueError('Некорректный серийный номер')):
            with self.assertRaisesRegex(ValueError, 'серийный номер'):
                menu.create(self.bundle)
        run.assert_not_called()

    def test_busy_never_spawns(self):
        with patch.object(menu.guest_control, 'stopped', return_value=False), patch.object(menu.subprocess, 'Popen') as spawn:
            with self.assertRaises(ValueError):
                menu.start(self.bundle, self.item)
            spawn.assert_not_called()

    def test_detached_start(self):
        process = Mock(pid=123)
        process.poll.return_value = None
        with patch.object(menu.guest_control, 'stopped', return_value=True), \
                patch.object(menu.socket, 'socket'), patch.object(menu.time, 'sleep'), \
                patch.object(menu.subprocess, 'Popen', return_value=process) as spawn, \
                contextlib.redirect_stdout(io.StringIO()):
            menu.start(self.bundle, self.item)
        self.assertTrue(spawn.call_args.kwargs['start_new_session'])
        self.assertEqual(spawn.call_args.kwargs['stdin'], menu.subprocess.DEVNULL)

    def test_failed_start_reported(self):
        process = Mock(pid=123, returncode=1)
        process.poll.return_value = 1
        with patch.object(menu.guest_control, 'stopped', return_value=True), \
                patch.object(menu.socket, 'socket'), patch.object(menu.time, 'sleep'), \
                patch.object(menu.subprocess, 'Popen', return_value=process):
            with self.assertRaisesRegex(ValueError, 'кодом 1'):
                menu.start(self.bundle, self.item)

    def test_save_atomic_private(self):
        path = self.root / 'menu.json'
        menu.save(path, self.item)
        self.assertEqual(json.loads(path.read_text()), self.item)
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)

    def test_open_uses_actual_runtime_port(self):
        with patch.object(menu.guest_control, 'qmp_status'), \
                patch.object(menu.guest_control, 'read_state', return_value={'port': 15514, 'network': 'user'}), \
                patch.object(menu.subprocess, 'run') as run:
            menu.open_dsm(self.item)
        run.assert_called_once_with(['open', 'http://127.0.0.1:15514/'], check=True)

    def test_stop_without_credentials_only_opens_dsm(self):
        with patch.object(menu, 'ask', return_value='нет'), patch.object(menu, 'open_dsm') as opened, \
                patch.object(menu.subprocess, 'run') as run, contextlib.redirect_stdout(io.StringIO()):
            menu.stop(self.bundle, self.item)
        opened.assert_called_once_with(self.item)
        run.assert_not_called()

    def test_exit_does_not_stop(self):
        with patch.object(menu, 'ask', return_value='0'), patch.object(menu, 'stop') as stop, \
                contextlib.redirect_stdout(io.StringIO()):
            menu.menu(self.bundle, self.root / 'missing-state.json')
        stop.assert_not_called()


if __name__ == '__main__':
    unittest.main()
