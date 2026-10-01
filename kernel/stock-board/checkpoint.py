#!/usr/bin/env python3
"""Clone a STOPPED stock instance into a NEW bootable recovery directory.

No overwrite, no live snapshot, no repair. Restore means cloning a preserved
checkpoint to another new directory, never replacing the working instance.
"""
import argparse
from contextlib import ExitStack
import datetime
import fcntl
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
from instance import HASHES, digest, regular, verify
from boot_from_flash import extract
from profiles import validate_manifest
import lease


def clone(source, target, qemu_img='qemu-img'):
    source = source.resolve(strict=True)
    target = target.absolute()
    if target.exists() or target.is_symlink():
        raise ValueError('Destination already exists; refusing overwrite')
    with lease.hold(source), ExitStack() as locks:
        # Same order as the launcher; exclude the short between-boots gap too.
        for name in ('supervisor.lock', 'run.lock'):
            fd = os.open(source / name, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
            lock = locks.enter_context(os.fdopen(fd, 'r+'))
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise ValueError('Source is running; shut DSM down before checkpoint') from None
        if (source / 'resize.pending.json').exists():
            raise ValueError('Unfinished disk resize; use the preceding checkpoint')
        regular(source / 'instance.json')
        manifest = json.loads((source / 'instance.json').read_text())
        model, spec, fmt, size = validate_manifest(manifest)
        disk = manifest['disk']
        names = ['instance.json', disk, 'flash.bin'] + ['boot/' + n for n in spec['hashes']]
        for name in names:
            regular(source / name)
        for name, expected in spec['hashes'].items():
            verify(source / 'boot' / name, expected)
        build, _ = extract((source / 'flash.bin').read_bytes(), model=model)
        info = json.loads(subprocess.check_output([qemu_img, 'info', '--output=json', str(source / disk)]))
        if (info.get('format') != fmt or info.get('virtual-size') != size or
                info.get('backing-filename') or info.get('format-specific', {}).get('data', {}).get('data-file')):
            raise ValueError('Only standalone media matching its manifest can be cloned')
        if fmt == 'qcow2':
            subprocess.run([qemu_img, 'check', str(source / disk)], check=True)
        target.mkdir(mode=0o700)
        (target / 'boot').mkdir()
        hashes = {}
        for name in names:
            src, dst = source / name, target / name
            if platform.system() == 'Darwin':
                subprocess.run(['cp', '-c', str(src), str(dst)], check=True)
            elif platform.system() == 'Linux':
                subprocess.run(['cp', '--reflink=auto', '--sparse=always', str(src), str(dst)], check=True)
            else:
                shutil.copyfile(src, dst)
            hashes[name] = digest(src)
            if digest(dst) != hashes[name]:
                raise ValueError(f'Copy checksum mismatch: {name}')
        if fmt == 'qcow2':
            subprocess.run([qemu_img, 'check', str(target / disk)], check=True)
        record = {'source': str(source), 'build': build, 'sha256': hashes,
                  'created_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  'cold_copy': True, 'bootable_instance': spec['boot_supported'], 'model': model}
        with (target / 'checkpoint.json').open('x') as f:
            json.dump(record, f, indent=2)
        print(json.dumps({'checkpoint': str(target), 'build': build, 'verified_files': len(hashes)}))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source', type=Path)
    p.add_argument('destination', type=Path)
    p.add_argument('--qemu-img', default='qemu-img')
    a = p.parse_args()
    clone(a.source, a.destination, a.qemu_img)
