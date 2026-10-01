"""Local readiness and authenticated DSM poweroff; never cuts VM power."""
import json
import fcntl
from contextlib import ExitStack
import os
from pathlib import Path
import socket
import stat
import time
import urllib.parse
import urllib.request
import lease
import settings
from profiles import validate_manifest


def vm_name(root):
    path = root / 'instance.json'
    if path.is_symlink():
        raise ValueError('Symlink instance manifest refused')
    model, _, _, _ = validate_manifest(json.loads(path.read_text()))
    return f'{model.lower()}-stock-72806'


def stopped(root):
    """Confirm both launcher and QEMU released their locks, not just lost QMP."""
    if (root / lease.NAME).exists() or (root / lease.NAME).is_symlink():
        return False
    with ExitStack() as locks:
        for name in ('supervisor.lock', 'run.lock'):
            fd = os.open(root / name, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
            handle = locks.enter_context(os.fdopen(fd, 'r+'))
            try:
                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                return False
        return True


def qmp_status(root):
    expected_name = vm_name(root)
    if (root / 'status.sock').is_symlink():
        raise ValueError('Symlink QMP socket refused')
    old = os.open('.', os.O_RDONLY)
    try:
        os.chdir(root)
        with socket.socket(socket.AF_UNIX) as sock:
            sock.settimeout(3)
            sock.connect('status.sock')
            with sock.makefile('rwb', buffering=0) as stream:
                if 'QMP' not in json.loads(stream.readline(65536)):
                    raise ValueError('Not a QMP peer')
                def call(name):
                    stream.write(json.dumps({'execute': name}).encode() + b'\n')
                    while True:
                        answer = json.loads(stream.readline(65536))
                        if 'event' not in answer:
                            if 'return' not in answer:
                                raise ValueError('QMP request failed')
                            return answer['return']
                call('qmp_capabilities')
                if call('query-name').get('name') != expected_name:
                    raise ValueError('Wrong VM identity')
                return call('query-status')
    finally:
        os.fchdir(old)
        os.close(old)


def read_state(root):
    path = root / 'runtime.json'
    if path.is_symlink():
        raise ValueError('Symlink runtime metadata refused')
    state = json.loads(path.read_text())
    if not isinstance(state.get('port'), int) or not 1024 <= state['port'] <= 65535:
        raise ValueError('Invalid runtime port')
    return state


def api_endpoint(state):
    # Older instances used loopback and did not record a bind address. Inside
    # Docker the wildcard listener is reached through container-local loopback.
    address = settings.bind_address(state.get('bind_address', '127.0.0.1'))
    if address == '0.0.0.0':
        address = '127.0.0.1'
    port = state['port']
    if type(port) is not int or not 1024 <= port <= 65535:
        raise ValueError('Invalid runtime port')
    return f'http://{address}:{port}'


def health(root):
    try:
        state = read_state(root)
        status = qmp_status(root)
        if not status['running']:
            return 1, 'VM is not running'
        if state.get('network') == 'N':
            return 1, 'Network disabled; DSM readiness cannot be verified'
        try:
            opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
            with opener.open(api_endpoint(state) + '/webapi/query.cgi?api=SYNO.API.Info&version=1&method=query&query=SYNO.API.Auth', timeout=3) as r:
                value = json.loads(r.read(1024 * 1024))
            if value.get('success') and 'SYNO.API.Auth' in value.get('data', {}):
                return 0, 'DSM API ready (does not certify every service)'
        except (OSError, ValueError):
            pass
        if time.time() - state['started'] < state['startup_timeout']:
            return 2, 'DSM is starting'
        return 1, 'DSM startup timeout or API unavailable'
    except (OSError, ValueError, KeyError):
        return 1, 'Instance stopped or runtime metadata unavailable'


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


def credentials(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(fd) as f:
        st = os.fstat(f.fileno())
        if not stat.S_ISREG(st.st_mode) or stat.S_IMODE(st.st_mode) not in (0o600, 0o400):
            raise ValueError('Shutdown credentials must be a regular file with mode 0600 or 0400')
        data = json.load(f)
    if set(data) != {'account', 'password'} or not all(isinstance(v, str) and v for v in data.values()):
        raise ValueError('Expected account/password credential file')
    return data


def poweroff(root, path):
    if not qmp_status(root)['running']:
        raise ValueError('VM is not running')
    state = read_state(root)
    if state.get('network') == 'N':
        raise ValueError('Network disabled; API shutdown unavailable')
    secret = credentials(path)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
    sid, token = '', ''
    def request(api, method, version=1, **params):
        data = urllib.parse.urlencode(dict(api=api, method=method, version=version,
                                          **params, _sid=sid)).encode()
        r = urllib.request.Request(api_endpoint(state) + '/webapi/entry.cgi',
                                   data=data, headers={'X-SYNO-TOKEN': token})
        try:
            with opener.open(r, timeout=30) as response:
                result = json.loads(response.read(1024 * 1024))
        except OSError as error:
            raise OSError(f'DSM {api}.{method}: {error}') from None
        if not result.get('success'):
            raise ValueError(f'DSM {api}.{method} refused: {result.get("error", {}).get("code")}')
        return result.get('data', {})
    auth = request('SYNO.API.Auth', 'login', version=7, account=secret['account'], passwd=secret['password'],
                   session='DSMShutdown', format='sid', enable_syno_token='yes')
    sid, token = auth['sid'], auth.get('synotoken', '')
    try:
        request('SYNO.Core.System', 'shutdown')
    except OSError:
        # The server can disappear before returning the shutdown response.
        # Outcome is UNKNOWN, not failure or success: caller must await lifetime locks.
        return False
    except ValueError:
        try:
            request('SYNO.API.Auth', 'logout', session='DSMShutdown')
        except (OSError, ValueError):
            pass
        raise
    # Do not wait for logout from a server that is already shutting down.
    return True
