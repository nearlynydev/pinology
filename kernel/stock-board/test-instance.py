#!/usr/bin/env python3
"""Safety regressions using temporary files only, never existing lab disks."""
import argparse
import importlib.util
import json
import sys
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("instance", Path(__file__).with_name("instance.py"))
instance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(instance)


class SafetyTests(unittest.TestCase):
    def test_accelerator_profiles(self):
        self.assertEqual(instance.accelerator_args("tcg"),
                         ["-accel", "tcg,thread=single", "-cpu", "cortex-a55"])
        self.assertEqual(instance.accelerator_args("hvf"),
                         ["-accel", "hvf,kernel-irqchip=off", "-cpu", "host"])
        with self.assertRaisesRegex(ValueError, "Unknown accelerator"):
            instance.accelerator_args("auto")

    def test_existing_directory_not_overwritten(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            sentinel = root / "disk.qcow2"
            sentinel.write_bytes(b"preserve")
            with self.assertRaisesRegex(ValueError, "already exists"):
                instance.prepare(argparse.Namespace(instance=root))
            self.assertEqual(sentinel.read_bytes(), b"preserve")

    def test_regular_rejects_symlink(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "target").write_bytes(b"original")
            (root / "link").symlink_to(root / "target")
            with self.assertRaisesRegex(ValueError, "non-symlink"):
                instance.regular(root / "link")

    def test_hash_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "artifact"
            path.write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                instance.verify(path, "0" * 64)

    def test_unknown_manifest_rejected_before_qemu(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "instance.json").write_text(json.dumps({"build": "other"}))
            with patch.object(instance.subprocess, "Popen") as launch:
                with self.assertRaisesRegex(ValueError, "Unknown instance"):
                    instance.run(argparse.Namespace(instance=root))
                launch.assert_not_called()

    def test_flash_boot_passes_vendor_root_device(self):
        self.check_flash_boot("tcg")

    def test_hvf_boot_uses_host_cpu_without_fallback(self):
        self.check_flash_boot("hvf")

    def test_experimental_gicv2_is_off_by_default(self):
        self.check_flash_boot("tcg", expected_machine="ds223")

    def test_experimental_gicv2_is_explicit_for_tcg_and_kvm(self):
        for accel in ("tcg", "kvm"):
            self.check_flash_boot(accel, experimental_gicv2=True,
                                  expected_machine="ds223,experimental-gicv2=on")

    def test_experimental_gicv2_rejects_hvf_before_launch(self):
        with patch.object(instance.subprocess, "Popen") as launch:
            with self.assertRaisesRegex(ValueError, "requires KVM or TCG"):
                instance.run(argparse.Namespace(accel="hvf", experimental_gicv2=True))
            launch.assert_not_called()

    def test_microp_trace_is_separate_from_console(self):
        self.check_flash_boot("hvf", trace_microp=True)

    def test_experimental_ds423_uses_its_own_firmware_metadata(self):
        self.check_flash_boot('hvf', model='DS423')

    def test_second_nic_does_not_apply_soc_netdev_globally(self):
        self.check_flash_boot('hvf', model='DS423', second_nic=True)

    def test_invalid_smb_port_fails_before_opening_instance(self):
        for port in (445, 15504, 65536):
            with self.assertRaisesRegex(ValueError, 'SMB port'):
                instance.run(argparse.Namespace(accel='hvf', smb_port=port, port=15504))

    def check_flash_boot(self, accel, trace_microp=False, model='DS223', second_nic=False,
                         experimental_gicv2=False, expected_machine=None):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            profile = instance.profile(model)
            (root / "instance.json").write_text(json.dumps({
                'model': model, "build": profile['build'], "hashes": profile['hashes'],
                "pat_sha256": profile['pat_sha256'], "disk": "disk.qcow2"}))
            (root / "disk.qcow2").write_bytes(b"mock qcow2")
            with (root / "flash.bin").open("wb") as flash:
                flash.truncate(0x1000000)
            args = argparse.Namespace(instance=root, qemu=sys.executable,
                                      qemu_img="unused", flash=True, port=15504, accel=accel,
                                      trace_microp=trace_microp, experimental_ds423=model == 'DS423',
                                      experimental_second_nic=second_nic,
                                      experimental_gicv2=experimental_gicv2)
            info = json.dumps({"format": "qcow2", "virtual-size": 32 * 1024**3})
            with patch.object(instance, "verify"), \
                    patch.object(instance.subprocess, "check_output", return_value=info), \
                    patch.object(instance, "extract_flash_boot", return_value=("verified-test", {})), \
                    patch.object(instance.subprocess, "Popen") as launch:
                launch.return_value.wait.return_value = 0
                self.assertEqual(instance.run(args), 0)
                command = launch.call_args.args[0]
                self.assertEqual(command[command.index("-accel"):command.index("-accel") + 4],
                                 instance.accelerator_args(accel))
                args = command[command.index("-append") + 1].split()
                self.assertEqual(args.count("root=/dev/md0"), 1)
                self.assertIn(f"syno_hw_version={model}", args)
                self.assertIn('vender_format_version=2', args)
                self.assertNotIn('vendor_format_version=2', args)
                self.assertIn('netif_num=2' if model == 'DS423' else 'netif_num=1', args)
                self.assertIn('syno_fw_version=M.115' if model == 'DS423' else 'syno_fw_version=M.215', args)
                self.assertEqual(command[command.index('-machine') + 1],
                                 expected_machine or model.lower())
                self.assertIn("if=pflash,index=0,format=raw,file=flash.bin", command)
                if second_nic:
                    self.assertNotIn('rtd1619b-net.netdev=ds223net', command)
                    self.assertIn('-nic', command)
                    self.assertIn('user,id=ds423net2,net=10.0.3.0/24', command)
                    self.assertIn('rtd-r8168-pci,bus=/pcie1/pci,addr=0.0,netdev=ds423net2', command)
                serials = [command[i + 1] for i, x in enumerate(command) if x == "-serial"]
                self.assertEqual(serials, ["stdio", "chardev:microp"] if trace_microp else ["stdio"])
                if trace_microp:
                    backend = command[command.index("-chardev") + 1]
                    self.assertTrue(backend.startswith("ringbuf,id=microp,size=65536,logfile="))
                    self.assertTrue(backend.endswith(".microp.bin"))


if __name__ == "__main__":
    unittest.main()
