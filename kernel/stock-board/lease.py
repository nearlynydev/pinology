"""Atomic directory lease shared by native and bind-mounted Docker runtimes.

Unlike flock across different kernels, mkdir exclusivity crosses the mount.
An unclean exit deliberately leaves a stale lease. Never steal it automatically.
"""
from contextlib import contextmanager, ExitStack
import fcntl
import json
import os
import socket
import time

NAME = 'active.lease'


def local_idle(root):
    with ExitStack() as locks:
        for name in ('supervisor.lock', 'run.lock'):
            fd = os.open(root / name, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
            handle = locks.enter_context(os.fdopen(fd, 'r+'))
            try:
                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                return False
        return True


@contextmanager
def hold(root):
    root = root.resolve(strict=True)
    marker = root / NAME
    try:
        marker.mkdir(mode=0o700)
    except FileExistsError:
        raise ValueError('Instance active or stale active.lease; verify both native and Docker are stopped before manual recovery') from None
    owner = marker / 'owner.json'
    owner.write_text(json.dumps({'host': socket.gethostname(), 'pid': os.getpid(),
                                 'created': time.time()}) + '\n')
    try:
        yield
    finally:
        # In particular, retain the lease if the launcher exits while QEMU lives.
        if local_idle(root):
            owner.unlink()
            marker.rmdir()
