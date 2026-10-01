"""Strict, non-executable configuration shared by native and Docker launches."""
import os
import ipaddress
from pathlib import Path
import re
from vendor_identity import validate_serial

DEFAULTS = dict(MODEL='DS223', SERIAL='', STORAGE='', DISK_SIZE='32G', DISK_FMT='qcow2',
                ALLOCATE='N', NETWORK='user', USER_PORTS='', MAC='', RAM_CHECK='Y',
                CPU_CORES='4', RAM_SIZE='2G', HTTP_PORT='15504', SMB_PORT='',
                ACCEL='tcg', BIND_ADDRESS='127.0.0.1', DEBUG='N',
                SHUTDOWN='N', SHUTDOWN_CREDENTIALS='', TIMEOUT='105',
                STARTUP_TIMEOUT='600', URL='', PAT_FILE='', EXPERIMENTAL_DS423='N')


def load(path=None, environ=None):
    result = DEFAULTS.copy()
    if path:
        for number, line in enumerate(Path(path).read_text().splitlines(), 1):
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            key, sep, value = line.partition('=')
            key, value = key.strip(), value.strip()
            if not sep or key not in DEFAULTS:
                raise ValueError(f'Unknown setting at line {number}: {key}')
            if value.startswith(('"', "'")):
                if len(value) < 2 or value[-1] != value[0]:
                    raise ValueError(f'Unclosed quote at line {number}')
                value = value[1:-1]
            # Deliberately no shell, interpolation, escapes or inline comments.
            result[key] = value
    env = os.environ if environ is None else environ
    result.update({key: env[key] for key in DEFAULTS if key in env})
    result['SERIAL'] = validate_serial(result['SERIAL'])
    if result['MODEL'] not in ('DS223', 'DS423'):
        raise ValueError('MODEL must be DS223 or DS423')
    if result['CPU_CORES'] != '4' or size(result['RAM_SIZE']) != 2 * 1024**3:
        raise ValueError('Stock board requires CPU_CORES=4 and RAM_SIZE=2G')
    if not 8 * 1024**3 <= size(result['DISK_SIZE']) <= 16 * 1024**4:
        raise ValueError('DISK_SIZE must be between 8G and 16T')
    for key in ('ALLOCATE', 'RAM_CHECK', 'DEBUG', 'SHUTDOWN', 'EXPERIMENTAL_DS423'):
        if result[key] not in ('Y', 'N'):
            raise ValueError(f'{key} must be Y or N')
    for key, choices in dict(DISK_FMT=('raw', 'qcow2'), NETWORK=('user', 'N'),
                             ACCEL=('hvf', 'kvm', 'tcg')).items():
        if result[key] not in choices:
            raise ValueError(f'Unsupported {key}')
    result['BIND_ADDRESS'] = bind_address(result['BIND_ADDRESS'])
    for key in ('TIMEOUT', 'STARTUP_TIMEOUT'):
        if not result[key].isdigit() or not 10 <= int(result[key]) <= 3600:
            raise ValueError(f'{key} must be 10..3600 seconds')
    mac = result['MAC'].lower()
    if mac and (not re.fullmatch(r'(?:[0-9a-f]{2}:){5}[0-9a-f]{2}', mac)
                or int(mac[:2], 16) & 1 or mac == '00:00:00:00:00:00'):
        raise ValueError('MAC must be a nonzero unicast address')
    result['MAC'] = mac
    forwards(result['HTTP_PORT'], result['SMB_PORT'], result['USER_PORTS'])
    return result


def bind_address(value):
    """Literal IPv4 only: no QEMU option injection, DNS or IPv6 ambiguity."""
    address = ipaddress.IPv4Address(value)
    if address.is_multicast or address.is_link_local or address.is_reserved:
        raise ValueError('Bind address must be a unicast IPv4 address or 0.0.0.0')
    if int(address) != 0 and int(address) >> 24 == 0:
        raise ValueError('Invalid forwarding bind address')
    return str(address)


def size(value):
    match = re.fullmatch(r'([1-9][0-9]*)([MGT])', str(value).upper())
    if not match:
        raise ValueError('Use an integer size with M, G or T suffix')
    return int(match[1]) * 1024 ** {'M': 2, 'G': 3, 'T': 4}[match[2]]


def forwards(http, smb=None, extra=''):
    result = [('tcp', int(http), 5000)]
    if smb:
        result.append(('tcp', int(smb), 445))
    for item in extra.split(',') if extra else []:
        match = re.fullmatch(r'(tcp|udp):([0-9]+):([0-9]+)', item.strip())
        if not match:
            raise ValueError('USER_PORTS syntax: tcp:host_port:guest_port,udp:host_port:guest_port')
        result.append((match[1], int(match[2]), int(match[3])))
    used = set()
    for protocol, host, guest in result:
        if not (1 <= host <= 65535 and 1 <= guest <= 65535):
            raise ValueError('Host and guest ports must be 1..65535')
        if (protocol, host) in used:
            raise ValueError('Duplicate host forwarding port')
        used.add((protocol, host))
    return result


def check_ram():
    import platform
    import subprocess
    if platform.system() == 'Darwin':
        total = int(subprocess.check_output(['sysctl', '-n', 'hw.memsize']))
        # macOS compresses/reclaims memory: report physical capacity, not fake availability.
        if total < 3 * 1024**3:
            raise ValueError('Insufficient host RAM for the 2G guest')
    else:
        values = dict(re.findall(r'^(MemAvailable):\s+(\d+)', Path('/proc/meminfo').read_text(), re.M))
        available = int(values.get('MemAvailable', 0)) * 1024
        # Honour cgroup v2 as well as the host's MemAvailable inside Docker.
        limit, current = Path('/sys/fs/cgroup/memory.max'), Path('/sys/fs/cgroup/memory.current')
        if limit.exists() and current.exists():
            maximum = limit.read_text().strip()
            if maximum != 'max':
                available = min(available, int(maximum) - int(current.read_text()))
        if available < 2304 * 1024**2:
            raise ValueError('Less than 2304 MiB available host RAM; explicit RAM_CHECK=N overrides')
