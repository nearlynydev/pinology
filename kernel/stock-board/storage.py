"""Offline disk growth, with a verified mandatory cold checkpoint."""
from contextlib import ExitStack
import fcntl
import json
import os
import subprocess
from instance import digest, regular
from profiles import validate_manifest
from checkpoint import clone
import lease


def grow_disk(source, size, target, qemu_img='qemu-img'):
    source = source.resolve(strict=True)
    regular(source / 'instance.json')
    manifest = json.loads((source / 'instance.json').read_text())
    _, _, fmt, previous = validate_manifest(manifest)
    if not previous < size <= 16 * 1024**4:
        raise ValueError('Growth only: new size must exceed current size and be <=16T')
    clone(source, target, qemu_img)
    record = json.loads((target / 'checkpoint.json').read_text())
    with lease.hold(source), ExitStack() as locks:
        for name in ('supervisor.lock', 'run.lock'):
            fd = os.open(source / name, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
            handle = locks.enter_context(os.fdopen(fd, 'r+'))
            try:
                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise ValueError('Instance started after checkpoint; resize refused') from None
        # Detect changes between checkpoint releasing its locks and this acquisition.
        for name, expected in record['sha256'].items():
            regular(source / name)
            if digest(source / name) != expected:
                raise ValueError('Source changed after checkpoint; resize refused')
        if json.loads((source / 'instance.json').read_text()) != manifest:
            raise ValueError('Manifest changed while preparing resize')
        journal = source / 'resize.pending.json'
        with journal.open('x') as stream:
            json.dump({'old_size': previous, 'new_size': size,
                       'checkpoint': str(target.absolute())}, stream)
            stream.flush()
            os.fsync(stream.fileno())
        # An interrupted operation intentionally leaves the journal and blocks boot.
        subprocess.run([qemu_img, 'resize', '-f', fmt, str(source / manifest['disk']), str(size)], check=True)
        manifest['disk_size'] = size
        temp = source / 'instance.resized.json'
        with temp.open('x') as stream:
            json.dump(manifest, stream, indent=2)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp, source / 'instance.json')
        journal.unlink()
    print(f'Disk grown to {size // 1024**3} GiB; expand the pool/volume inside DSM separately.')
