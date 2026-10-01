"""Read boot payloads from emulated NOR, allowing only verified stock profiles."""
import hashlib
import lzma
import struct

PROFILES = {
    "7.2.2-72806": {
        "Image.stock": "4c6584070c2bfee8a7ab1b73ef74d1872a0469700e61fc806e6b72d23e576b14",
        "rd.bin": "43aaeaf04ec30962c722c3ce531cd3f5b3cf72f30554ccce62b0041db960f593",
        "model.dtb": "b582240773095ab2fca981923885cb6331a2825a45471bc3c3df7eaf8793c387",
    },
    # Update 9 is cumulative and ships the flash payload from Update 4.
    # Independently extracted from the signed 72806-9 PAT by extract-update9.py.
    "7.2.2-72806-flash4": {
        "Image.stock": "04486d8bb581ed0d58a8f09b43b1311c0b0505042a590dd1f43a3a430f68a3d5",
        "rd.bin": "f4ad668e8e916bf02efabb015894d35d0fd336474ab241a45d3135103279b5ca",
        "model.dtb": "b582240773095ab2fca981923885cb6331a2825a45471bc3c3df7eaf8793c387",
    },
    "7.4.1-90080": {
        "Image.stock": "eeca763d13fd37def3a1f8973c3e62e2405fd3a63d4df5b3223829364aa484a6",
        "rd.bin": "de54b401347741c07df02caed7eb7b332bc210e063c9991a34052176fa26ad93",
        "model.dtb": "10eda11855e0494693fa41a26b7644b12e2f508682e296f07ffc4364ba69891d",
    },
}

# Independently signature-verified DS423 72806-9 PAT, flash payload Update 4.
# Kept model-scoped: neither a DS223 ramdisk nor its DTB may boot DS423.
MODEL_UPDATES = {
    'DS423': {
        # Official full PAT, signature verified independently on 2026-09-25.
        # PAT SHA256: 1e604fea44b44daf2487900f13b03c17890de899164bcf38461127358648087c
        '7.4.1-90080': {
            'Image.stock': 'eeca763d13fd37def3a1f8973c3e62e2405fd3a63d4df5b3223829364aa484a6',
            'rd.bin': '14eebaa522eb9090c905c9b536cfb67a8b5073ddec3653c12d863961c955c4a9',
            'model.dtb': 'f394a7a0b0b51ee30e572726a576b62d69f98ce0ef0812a5eae48a7fc58697ed',
        },
        '7.2.2-72806-flash4': {
            'Image.stock': '04486d8bb581ed0d58a8f09b43b1311c0b0505042a590dd1f43a3a430f68a3d5',
            'rd.bin': '27093b242f729748654fe6ee8875924f84621bce1ae78be0746724e6837b7552',
            'model.dtb': 'bb16cbce355d16fa35249dc317652d5045046e03e84fc870d8f827383972e7b2',
        },
    },
}


def extract(media, model='DS223'):
    if len(media) != 0x1000000:
        raise ValueError("Expected 16 MiB NOR")
    entries = {}
    for pos in range(0xfff000, 0x1000000, 256):
        if media[pos] == 0xff:
            break
        name, offset, _, size, _, length = struct.unpack_from("<16sIIIII", media, pos)
        name = name.split(b"\0", 1)[0].decode("ascii")
        if name in entries or offset >= len(media) or size > len(media) - offset or length > size:
            raise ValueError("Invalid/duplicate FIS entry")
        entries[name] = (offset, size, length)
    files = {}
    for name, filename in (("zImage", "zImage"), ("rd.gz", "rd.bin"), ("dtb", "model.dtb")):
        if name not in entries or not entries[name][2]:
            raise ValueError(f"Missing boot payload in FIS: {name}")
        offset, size, length = entries[name]
        files[filename] = media[offset:offset + length]
    decoder = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE, memlimit=128 * 1024**2)
    files["Image.stock"] = decoder.decompress(files.pop("zImage"), max_length=64 * 1024**2)
    if not decoder.eof:
        raise ValueError("Incomplete or oversized flash kernel")
    hashes = {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}
    if model != 'DS223':
        from profiles import profile as get_profile
        spec = get_profile(model)
        allowed = {spec['build']: {k: v for k, v in spec['hashes'].items() if k != 'zImage'}}
        allowed.update(MODEL_UPDATES.get(model, {}))
    else:
        allowed = PROFILES
    for build, profile in allowed.items():
        if profile == hashes:
            return build, files
    raise ValueError(f"Flash boot profile unverified; refusing stale kernel fallback: {hashes}")
