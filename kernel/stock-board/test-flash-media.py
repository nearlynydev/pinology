#!/usr/bin/env python3
"""Validate generated layout with synthetic inputs, never production images."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("flash_media", Path(__file__).with_name("flash-media.py"))
media = importlib.util.module_from_spec(spec)
spec.loader.exec_module(media)


class FlashTests(unittest.TestCase):
    def test_offsets_payloads_and_reserved_regions(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in ("uboot_DS223.bin", "zImage", "model.dtb", "rd.bin"):
                (root / name).write_bytes(name.encode())
            output = root / "flash.bin"
            with patch.object(media.instance, "verify"):
                media.prepare(root, output)
            result = output.read_bytes()
            self.assertEqual(len(result), 0x1000000)
            self.assertEqual(result[0x190000:0x190006], b"zImage")
            self.assertEqual(result[0x8c0000:0x8c0009], b"model.dtb")
            self.assertEqual(result[0x8d0000:0x8d0006], b"rd.bin")
            self.assertEqual(result[0xfd5000:0xfff000], b"\xff" * 0x2a000)
            offset = 0
            for i, (name, kib, filename, _) in enumerate(media.PARTS):
                fields = struct.unpack_from("<16sIIIII", result, 0xfff000 + 256 * i)
                self.assertEqual(fields[0].rstrip(b"\0").decode(), name)
                self.assertEqual(fields[1], offset)
                self.assertEqual(fields[3], kib * 1024)
                self.assertEqual(fields[5], len(filename) if filename else 0)
                offset += kib * 1024
            with patch.object(media.instance, "verify"):
                with self.assertRaises(FileExistsError):
                    media.prepare(root, output)
            self.assertEqual(output.read_bytes(), result)


if __name__ == "__main__":
    unittest.main()
