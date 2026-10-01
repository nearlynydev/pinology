"""Fetch and extract a pinned official model PAT before creating a new instance."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.parse
import urllib.request
from profiles import profile
import vendor_identity

EXTRACTOR = 'vdsm/virtual-dsm@sha256:be18567e4d9690c053e3191f7fe5b65f68e871c50b708de82297021462175ff8'


def initialize(args):
    from instance import prepare, verify
    args.serial = vendor_identity.validate_serial(getattr(args, 'serial', ''))
    spec = profile(args.model)
    if args.instance.exists() or args.instance.is_symlink():
        raise ValueError('Instance already exists; init never overwrites')
    # Downloads/extraction remain separate, recoverable artifacts on failure.
    cache = args.instance.absolute().with_name(args.instance.name + '.media')
    cache.mkdir(mode=0o700)  # fail closed on an old partial setup
    pat = cache / 'boot.pat'
    if args.pat:
        verify(args.pat, spec['pat_sha256'])
        shutil.copyfile(args.pat, pat)
    else:
        url = args.url or spec['url']
        parsed = urllib.parse.urlsplit(url)
        if parsed.scheme != 'https' or parsed.username or parsed.password:
            raise ValueError('PAT URL must be HTTPS without credentials')
        with urllib.request.urlopen(url, timeout=60) as source, pat.open('xb') as target:
            if urllib.parse.urlsplit(source.url).scheme != 'https':
                raise ValueError('PAT redirect downgraded HTTPS')
            total = 0
            while chunk := source.read(1024**2):
                total += len(chunk)
                if total > 2 * 1024**3:
                    raise ValueError('PAT exceeds 2 GiB limit')
                target.write(chunk)
    verify(pat, spec['pat_sha256'])
    if args.artifacts is None:
        artifacts = cache / 'pat'
        artifacts.mkdir(mode=0o700)
        extractor = Path(__file__).with_name('extract.py')
        if extractor.exists():
            subprocess.run([sys.executable, str(extractor), '-i', str(pat), '-d'],
                           cwd=artifacts, check=True)
        else:
            # On native Mac, use a pinned no-network helper; no Docker socket in guest container.
            if ':' in str(cache):
                raise ValueError('Docker extractor cannot mount a path containing colon')
            subprocess.run(['docker', 'run', '--rm', '--network', 'none',
                            '--user', f'{os.getuid()}:{os.getgid()}',
                            '--entrypoint', '/bin/sh',
                            '-v', f'{pat}:/input/boot.pat:ro', '-v', f'{artifacts}:/output',
                            EXTRACTOR, '-c', 'cd /output && python3 /run/extract.py -i /input/boot.pat -d'],
                           check=True)
        args.artifacts = artifacts
    args.pat = pat
    # Validate flash/U-Boot BEFORE allocating an instance disk.
    module_spec = importlib.util.spec_from_file_location('flash_media', Path(__file__).with_name('flash-media.py'))
    module = importlib.util.module_from_spec(module_spec)
    module_spec.loader.exec_module(module)
    module.prepare(args.artifacts, cache / 'flash.bin', model=args.model, serial=args.serial)
    prepare(args)
    shutil.copyfile(cache / 'flash.bin', args.instance / 'flash.bin')
