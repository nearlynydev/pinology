"""Creation-only identity in the stock Synology vendor-v2 NOR format.

Layout/checksum: Synology linux-5.10.x/drivers/mtd/mtdpart.c,
syno_vender_v2_parser(). This does not establish entitlement to cloud services.
"""
import hashlib
import re

NOR_SIZE = 0x1000000
VENDOR_OFFSET = 0xfd5000
VENDOR_SIZE = 0x10000
SN_OFFSET = 0x10
SN_SIZE = 32


def validate_serial(value):
    """Accept an explicit ASCII identity, not shell syntax or a generated serial."""
    if not isinstance(value, str):
        raise ValueError('SERIAL must be text')
    if not value:
        return ''
    if not re.fullmatch(r'[A-Z0-9]{1,20}', value):
        raise ValueError('SERIAL must contain 1..20 uppercase ASCII letters/digits')
    if len(_record(value)) >= SN_SIZE:
        raise ValueError('SERIAL and checksum exceed the 32-byte vendor record')
    return value


def _record(serial):
    return f'SN={serial},CHK={sum(serial.encode("ascii"))}'.encode('ascii')


def encode_vendor(serial):
    serial = validate_serial(serial)
    vendor = bytearray(b'\xff' * VENDOR_SIZE)
    if serial:
        vendor[:16] = b'SYNO!!!!'.ljust(16, b'\0')
        vendor[SN_OFFSET:SN_OFFSET + SN_SIZE] = _record(serial).ljust(SN_SIZE, b'\0')
    # No MAC, custom serial, test flags or other device identity is invented.
    return bytes(vendor)


def read_serial(media):
    if len(media) != NOR_SIZE:
        raise ValueError('Expected 16 MiB NOR for vendor identity')
    vendor = media[VENDOR_OFFSET:VENDOR_OFFSET + VENDOR_SIZE]
    if vendor == b'\xff' * VENDOR_SIZE:
        return ''
    if vendor[:8] != b'SYNO!!!!':
        raise ValueError('Invalid vendor-v2 identity header')
    record = vendor[SN_OFFSET:SN_OFFSET + SN_SIZE]
    if b'\0' not in record:
        raise ValueError('Unterminated vendor serial record')
    match = re.fullmatch(rb'SN=([A-Z0-9]+),CHK=([0-9]+)', record.split(b'\0', 1)[0])
    if not match or sum(match[1]) != int(match[2]):
        raise ValueError('Invalid vendor serial record or checksum')
    return validate_serial(match[1].decode('ascii'))


def identity(serial):
    serial = validate_serial(serial)
    return {'vendor_format': 2, 'serial_sha256': hashlib.sha256(serial.encode('ascii')).hexdigest()}


def check_identity(media, manifest, requested=''):
    """Read-only boot guard. Existing legacy instances are never adopted/modified."""
    requested = validate_serial(requested)
    expected = manifest.get('identity')
    if 'identity' not in manifest and not requested:
        return  # Compatibility with instances created before this feature.
    if media is None:
        raise ValueError('Vendor identity requires persistent NOR (--flash)')
    actual = read_serial(media)
    if 'identity' in manifest and expected != identity(actual):
        raise ValueError('Vendor identity changed or is corrupt; restore the original flash/checkpoint')
    if requested and actual != requested:
        raise ValueError('SERIAL differs from existing flash; it is creation-only, not a runtime setter')
