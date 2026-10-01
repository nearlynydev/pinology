#!/usr/bin/env python3
"""Create new emulated NOR from stock PAT files and vendor U-Boot layout."""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import struct
import zlib
import vendor_identity

spec = importlib.util.spec_from_file_location("instance", Path(__file__).with_name("instance.py"))
instance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(instance)

# Sizes/order: stock U-Boot M.215 kernelargs. FIS names are DISTINCT from
# mtdparts labels: confirmed in stock updater's relocated table at 0xe28220.
PARTS = [("RedBoot", 1600, "uboot_DS223.bin", 0),
         ("zImage", 7360, "zImage", 0x01008000),
         ("dtb", 64, "model.dtb", 0),
         ("rd.gz", 7188, "rd.bin", 0x03100000),
         ("vendor", 64, None, 0), ("pstore", 96, None, 0),
         ("Misc Info", 8, None, 0), ("FIS directory", 4, None, 0)]
UBOOT_HASH = "4b60536f7df43d97483f730a97117ff053e6589d3a9616aa7c35c8ff8f11ac10"


def prepare(artifacts, output, model='DS223', serial=''):
    serial = vendor_identity.validate_serial(serial)
    from profiles import profile
    spec = profile(model)
    hashes = {name: spec['hashes'][name] for name in ("zImage", "rd.bin", "model.dtb")}
    hashes[spec['uboot']] = spec['uboot_sha256']
    for name, expected in hashes.items():
        instance.verify(artifacts / name, expected)
    media = bytearray(b"\xff" * 0x1000000)
    offset = 0
    table = bytearray(b"\xff" * 4096)
    for i, (name, kib, filename, memory) in enumerate(PARTS):
        if name == 'RedBoot':
            filename = spec['uboot']
        size = kib * 1024
        if name == 'vendor' and (offset != vendor_identity.VENDOR_OFFSET or size != vendor_identity.VENDOR_SIZE):
            raise ValueError('Vendor identity layout disagrees with flash partition table')
        data = (artifacts / filename).read_bytes() if filename else b""
        if name == 'vendor' and serial:
            data = vendor_identity.encode_vendor(serial)
        if len(data) > size:
            raise ValueError(f"{filename} exceeds partition")
        media[offset:offset + len(data)] = data
        # RedBoot FIS descriptor as defined by the stock GPL redboot.c.
        # Synthetic layout metadata, not a dump of a physical machine.
        entry = bytearray(256)
        struct.pack_into("<16sIIIII", entry, 0, name.encode(), offset, memory,
                         size, memory, len(data))
        struct.pack_into("<I", entry, 252, zlib.crc32(data))
        table[i * 256:(i + 1) * 256] = entry
        offset += size
    assert offset == len(media)
    media[0xfff000:] = table
    # Never adopts or overwrites pre-existing media, including symlinks.
    with output.open("xb") as stream:
        stream.write(media)
    state = 'supplied vendor serial' if serial else 'erased vendor, no serial'
    print(f"Created {output}: 16 MiB NOR; {state}; oops/misc areas erased")
    print(f"SHA-256: {hashlib.sha256(media).hexdigest()}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifacts", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument('--model', choices=('DS223', 'DS423'), default='DS223')
    parser.add_argument('--serial', default='', help='Optional user-supplied serial, written only to NEW NOR')
    args = parser.parse_args()
    prepare(args.artifacts, args.output, args.model, args.serial)
