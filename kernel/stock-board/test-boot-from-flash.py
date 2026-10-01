#!/usr/bin/env python3
"""Validate boot profile selection and refusal of corrupt/incomplete media."""
import hashlib
import lzma
import struct
import unittest
from unittest.mock import patch
import boot_from_flash


class BootTests(unittest.TestCase):
    def fixture(self):
        media = bytearray(b"\xff" * 0x1000000)
        files = {"Image.stock": b"test kernel", "rd.bin": b"test ramdisk", "model.dtb": b"test dtb"}
        parts = [("zImage", lzma.compress(files["Image.stock"], format=lzma.FORMAT_ALONE)),
                 ("rd.gz", files["rd.bin"]), ("dtb", files["model.dtb"])]
        for i, (name, payload) in enumerate(parts):
            offset = 0x1000 * (i + 1)
            media[offset:offset + len(payload)] = payload
            struct.pack_into("<16sIIIII", media, 0xfff000 + 256 * i,
                             name.encode(), offset, 0, 4096, 0, len(payload))
        profile = {key: hashlib.sha256(value).hexdigest() for key, value in files.items()}
        return media, files, profile

    def test_verified_and_unknown_profiles(self):
        media, files, profile = self.fixture()
        with patch.dict(boot_from_flash.PROFILES, {"test": profile}, clear=True):
            self.assertEqual(boot_from_flash.extract(media), ("test", files))
            media[0x2000] ^= 1
            with self.assertRaisesRegex(ValueError, "refusing stale"):
                boot_from_flash.extract(media)

    def test_bad_bounds_and_duplicate(self):
        media, _, _ = self.fixture()
        struct.pack_into("<I", media, 0xfff000 + 16, 0xffffff)
        with self.assertRaisesRegex(ValueError, "Invalid"):
            boot_from_flash.extract(media)
        media, _, _ = self.fixture()
        media[0xfff100:0xfff110] = media[0xfff000:0xfff010]
        with self.assertRaisesRegex(ValueError, "duplicate"):
            boot_from_flash.extract(media)

    def test_update_is_model_scoped_and_corruption_still_refused(self):
        media, files, profile = self.fixture()
        with patch.dict(boot_from_flash.MODEL_UPDATES, {'DS423': {'test-update': profile}}, clear=True):
            self.assertEqual(boot_from_flash.extract(media, model='DS423'), ('test-update', files))
            with self.assertRaisesRegex(ValueError, 'refusing stale'):
                boot_from_flash.extract(media, model='DS223')
            media[0x2000] ^= 1
            with self.assertRaisesRegex(ValueError, 'refusing stale'):
                boot_from_flash.extract(media, model='DS423')

    def test_blank_and_wrong_size(self):
        with self.assertRaisesRegex(ValueError, "16 MiB"):
            boot_from_flash.extract(b"")
        with self.assertRaisesRegex(ValueError, "Missing"):
            boot_from_flash.extract(b"\xff" * 0x1000000)


if __name__ == "__main__":
    unittest.main()
