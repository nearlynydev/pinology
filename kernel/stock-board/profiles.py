"""Verified installation profiles. Model is immutable after installation."""
PROFILES = {
    'DS223': {
        'boot_supported': True,
        'build': '7.2.2-72806',
        'pat_sha256': '18f175f4488e92a91c3dcbcb7ec2f4cae3d8b1105f581fd885ab49693a5fe6f3',
        'url': 'https://global.synologydownload.com/download/DSM/release/7.2.2/72806/DSM_DS223_72806.pat',
        'uboot': 'uboot_DS223.bin',
        'uboot_sha256': '4b60536f7df43d97483f730a97117ff053e6589d3a9616aa7c35c8ff8f11ac10',
        'hashes': {
            'zImage': 'c68a2740943f696a0339471c2a463e2519e3b6fea5a27b2f357d847ee985c1f4',
            'Image.stock': '4c6584070c2bfee8a7ab1b73ef74d1872a0469700e61fc806e6b72d23e576b14',
            'rd.bin': '43aaeaf04ec30962c722c3ce531cd3f5b3cf72f30554ccce62b0041db960f593',
            'model.dtb': 'b582240773095ab2fca981923885cb6331a2825a45471bc3c3df7eaf8793c387',
        },
    },
    'DS423': {
        'boot_supported': False,
        'build': '7.2.2-72806',
        'pat_sha256': 'a76390a5a87f8a742f3e5fa9329a204bfdb07f72557090967c8be02ed68fef0a',
        'url': 'https://global.synologydownload.com/download/DSM/release/7.2.2/72806/DSM_DS423_72806.pat',
        'uboot': 'uboot_DS423.bin',
        'uboot_sha256': '0207d077a6a0785f34502654af449e2ab88ad21567d4324b48e0feb6a027bbbd',
        'hashes': {
            'zImage': 'c68a2740943f696a0339471c2a463e2519e3b6fea5a27b2f357d847ee985c1f4',
            'Image.stock': '4c6584070c2bfee8a7ab1b73ef74d1872a0469700e61fc806e6b72d23e576b14',
            'rd.bin': '43b909cef2409370e6d9e3fcaad116f1c05e9970deee5f2894cccd343489c456',
            'model.dtb': 'bb16cbce355d16fa35249dc317652d5045046e03e84fc870d8f827383972e7b2',
        },
    },
}


def profile(model):
    if model not in PROFILES:
        raise ValueError(f'Unverified model profile: {model}')
    return PROFILES[model]


def validate_manifest(manifest):
    model = manifest.get('model', 'DS223')
    spec = profile(model)
    if (manifest.get('build') != spec['build'] or manifest.get('hashes') != spec['hashes']
            or manifest.get('pat_sha256') != spec['pat_sha256']):
        raise ValueError('Unknown instance profile')
    fmt = manifest.get('disk_format', 'qcow2')
    if fmt not in ('raw', 'qcow2') or manifest.get('disk') != 'disk.' + fmt:
        raise ValueError('Unknown instance disk profile')
    size = manifest.get('disk_size', 32 * 1024**3)
    if type(size) is not int or not 8 * 1024**3 <= size <= 16 * 1024**4:
        raise ValueError('Invalid manifest disk size')
    return model, spec, fmt, size
