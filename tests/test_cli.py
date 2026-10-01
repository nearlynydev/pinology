import os
from pathlib import Path
import stat
import sys
import tempfile
import unittest
from unittest.mock import patch, Mock

sys.path.insert(0, str(Path(__file__).parents[1] / "scripts"))
import cli


class CliTests(unittest.TestCase):
    def test_mac_sdk_requires_modern_hvf_headers(self):
        for version, accepted in (("14.5", False), ("15.2", True), ("27.0", True), ("unknown", False)):
            with self.subTest(version=version), patch.object(cli.subprocess, "run", return_value=Mock(stdout=version)):
                if accepted:
                    self.assertEqual(cli.mac_sdk(), version)
                else:
                    with self.assertRaises(RuntimeError):
                        cli.mac_sdk()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.instance = self.root / "vm"

    def tearDown(self):
        self.tmp.cleanup()

    def args(self, **values):
        defaults = dict(path=str(self.instance), model="DS223", disk_size="32G",
                        serial="", pat=None, artifacts=None, port=15504,
                        smb_port=14445, experimental_ds423=False,
                        experimental_gicv2=False, credentials=None,
                        destination=str(self.root / "backup"))
        defaults.update(values)
        return type("Args", (), defaults)()

    def test_init_mac_routes_serial_and_readonly_inputs(self):
        pat = self.root / "DSM.pat"; pat.write_bytes(b"pat")
        artifacts = self.root / "artifacts"; artifacts.mkdir()
        with patch.object(cli, "host", return_value="mac"), \
                patch.object(cli, "native_runtime", return_value=(Path("runtime.py"), Path("qemu"), Path("qemu-img"))), \
                patch.object(cli.subprocess, "run") as run:
            cli.cmd_init(self.args(serial="AB12", pat=str(pat), artifacts=str(artifacts)))
        command = run.call_args.args[0]
        self.assertIn("--serial", command)
        self.assertEqual(command[command.index("--serial") + 1], "AB12")
        self.assertIn("--pat", command)
        self.assertIn("--artifacts", command)

    def test_init_linux_mounts_inputs_readonly_and_preserves_uid(self):
        pat = self.root / "DSM.pat"; pat.write_bytes(b"pat")
        artifacts = self.root / "artifacts"; artifacts.mkdir()
        with patch.object(cli, "host", return_value="linux"), \
                patch.object(cli, "docker_name_available"), \
                patch.object(cli.subprocess, "run") as run:
            cli.cmd_init(self.args(serial="AB12", pat=str(pat), artifacts=str(artifacts)))
        command = run.call_args.args[0]
        self.assertIn("--user", command)
        self.assertIn("type=bind,src=" + str(pat.resolve()) + ",dst=/input/boot.pat,readonly", command)
        self.assertIn("type=bind,src=" + str(artifacts.resolve()) + ",dst=/input/artifacts,readonly", command)
        self.assertNotIn("--privileged", command)
        self.assertIn("--network", command)
        self.assertIn("none", command)

    def test_linux_start_requires_kvm_and_uses_loopback_ports(self):
        self.instance.mkdir()
        (self.instance / "instance.json").write_text("{}")
        with patch.object(cli, "host", return_value="linux"), \
                patch.object(cli, "docker_kvm_args", return_value=["--device", "/dev/kvm", "--group-add", "123"]), \
                patch.object(cli, "docker_name_available"), \
                patch.object(cli.subprocess, "run") as run:
            cli.cmd_start(self.args())
        command = run.call_args.args[0]
        self.assertIn("--device", command)
        self.assertIn("/dev/kvm", command)
        self.assertIn("127.0.0.1:15504:5000/tcp", command)
        self.assertIn("127.0.0.1:14445:14445/tcp", command)
        self.assertNotIn("--privileged", command)

    def test_host_network_uses_wildcard_and_valid_ports(self):
        self.assertEqual(cli.network_options(self.args(network='host'))[0], '0.0.0.0')
        for values in (dict(bind_address='192.0.2.10'),
                       dict(network='bridge'), dict(network='host', bind_address='127.0.0.1'),
                       dict(https_port=15504), dict(https_port=0), dict(port=14445)):
            with self.subTest(values=values), self.assertRaises(ValueError):
                cli.network_options(self.args(**values))
        address, extra, forwards = cli.network_options(self.args(
            network='host', bind_address='192.0.2.10', https_port=15505))
        self.assertEqual(address, '192.0.2.10')
        self.assertEqual(extra, 'tcp:15505:5001')
        self.assertIn(('tcp', 15505, 5001), forwards)
        self.assertEqual(cli.network_options(self.args(network='host', bind_address='0.0.0.0'))[0], '0.0.0.0')

    def test_host_network_mac_passes_explicit_bind_and_https(self):
        self.instance.mkdir()
        (self.instance / 'instance.json').write_text('{}')
        with patch.object(cli, 'host', return_value='mac'), \
                patch.object(cli, 'native_runtime', return_value=(Path('runtime.py'), Path('qemu'), Path('qemu-img'))), \
                patch.object(cli, 'check_native_ports'), \
                patch.object(cli.subprocess, 'run') as run:
            cli.cmd_start(self.args(network='host', bind_address='192.0.2.10', https_port=15505))
        command = run.call_args.args[0]
        self.assertEqual(command[command.index('--bind-address') + 1], '192.0.2.10')
        self.assertEqual(command[command.index('--network') + 1], 'user')
        self.assertEqual(command[command.index('--user-ports') + 1], 'tcp:15505:5001')

    def test_host_network_linux_keeps_container_isolated(self):
        self.instance.mkdir()
        (self.instance / 'instance.json').write_text('{}')
        with patch.object(cli, 'host', return_value='linux'), \
                patch.object(cli, 'docker_kvm_args', return_value=['--device', '/dev/kvm']), \
                patch.object(cli, 'docker_name_available'), patch.object(cli.subprocess, 'run') as run:
            cli.cmd_start(self.args(network='host', bind_address='192.0.2.10', https_port=15505))
        command = run.call_args.args[0]
        for port, internal in ((15504,5000), (14445,14445), (15505,5001)):
            self.assertIn(f'192.0.2.10:{port}:{internal}/tcp', command)
        self.assertNotIn('--network', command)
        self.assertNotIn('--privileged', command)
        self.assertNotIn('--cap-add', command)
        self.assertIn('BIND_ADDRESS=0.0.0.0', command)
        self.assertIn('tcp:5001:5001', command)

    def test_native_port_defaults_and_overrides(self):
        args = cli.parser().parse_args(['start', 'unused', '--network', 'host'])
        self.assertEqual(cli.network_options(args), ('0.0.0.0', 'tcp:5001:5001',
            [('tcp',5000,5000),('tcp',445,445),('tcp',5001,5001)]))
        args = cli.parser().parse_args(['start', 'unused', '--port', '80', '--https-port', '443', '--smb-port', '14445'])
        self.assertEqual(cli.network_options(args)[2], [('tcp',80,5000),('tcp',14445,445),('tcp',443,5001)])

    def test_default_host_ports_publish_without_container_low_ports(self):
        self.instance.mkdir()
        (self.instance / 'instance.json').write_text('{}')
        args = cli.parser().parse_args(['start', str(self.instance), '--network', 'host'])
        with patch.object(cli, 'host', return_value='linux'), \
                patch.object(cli, 'docker_kvm_args', return_value=['--device', '/dev/kvm']), \
                patch.object(cli, 'docker_name_available'), patch.object(cli.subprocess, 'run') as run:
            cli.cmd_start(args)
        command = run.call_args.args[0]
        for published in ('0.0.0.0:5000:5000/tcp', '0.0.0.0:5001:5001/tcp', '0.0.0.0:445:14445/tcp'):
            self.assertIn(published, command)
        self.assertEqual(command[command.index('--smb-port') + 1], '14445')
        self.assertNotIn('--privileged', command)
        self.assertNotIn('--cap-add', command)

    def test_native_bind_error_has_override_hint(self):
        with patch.object(cli.socket, 'socket') as socket:
            socket.return_value.__enter__.return_value.bind.side_effect = PermissionError('not permitted')
            with self.assertRaisesRegex(RuntimeError, '--smb-port'):
                cli.check_native_ports('0.0.0.0', [('tcp',445,445)])

    def test_gicv2_is_linux_only(self):
        self.instance.mkdir()
        (self.instance / "instance.json").write_text("{}")
        with patch.object(cli, "host", return_value="mac"):
            with self.assertRaisesRegex(ValueError, "gicv2"):
                cli.cmd_start(self.args(experimental_gicv2=True))

    def test_stop_requires_real_0600_credentials(self):
        self.instance.mkdir()
        (self.instance / "instance.json").write_text("{}")
        credentials = self.root / "credentials.json"; credentials.write_text("{}")
        credentials.chmod(0o600)
        with patch.object(cli, "host", return_value="mac"), \
                patch.object(cli, "native_runtime", return_value=(Path("runtime.py"), Path("qemu"), Path("qemu-img"))), \
                patch.object(cli.subprocess, "run") as run:
            cli.cmd_stop(self.args(credentials=str(credentials)))
        self.assertIn("--shutdown-credentials", run.call_args.args[0])

    def test_backup_never_overwrites_destination(self):
        self.instance.mkdir()
        (self.instance / "instance.json").write_text("{}")
        destination = self.root / "backup"; destination.mkdir()
        with self.assertRaisesRegex(ValueError, "already exists"):
            cli.cmd_backup(self.args(destination=str(destination)))

    def test_environment_drops_runtime_overrides(self):
        with patch.dict(os.environ, {"ACCEL": "tcg", "SERIAL": "secret", "PATH": "/bin"}):
            env = cli.clean_env()
        self.assertNotIn("ACCEL", env)
        self.assertNotIn("SERIAL", env)
        self.assertEqual(env["PATH"], "/bin")

    def test_mount_and_container_name_syntax(self):
        self.assertEqual(cli.docker_mount(Path("/tmp/input"), "/input", True),
                         ["--mount", "type=bind,src=/tmp/input,dst=/input,readonly"])
        self.assertEqual(cli.docker_name(Path("/tmp/vm")),
                         "pinology-" + __import__("hashlib").sha256(b"/tmp/vm").hexdigest()[:16])

    def test_existing_container_name_is_rejected(self):
        result = Mock(stdout="pinology-existing\n")
        with patch.object(cli.subprocess, "run", return_value=result) as run:
            with self.assertRaisesRegex(ValueError, "already exists"):
                cli.docker_name_available("pinology-existing")
        self.assertEqual(run.call_args.args[0][-1], "{{.Names}}")

    def test_linux_stop_uses_verified_network_namespace(self):
        self.instance.mkdir()
        (self.instance / "instance.json").write_text("{}")
        credentials = self.root / "credentials.json"; credentials.write_text("{}")
        credentials.chmod(0o600)
        with patch.object(cli, "host", return_value="linux"), \
                patch.object(cli, "docker_identity", return_value="pinology-vm"), \
                patch.object(cli.subprocess, "run") as run:
            cli.cmd_stop(self.args(credentials=str(credentials)))
        command = run.call_args.args[0]
        self.assertIn("container:pinology-vm", command)
        self.assertIn("--shutdown-credentials", command)


if __name__ == "__main__":
    unittest.main()
