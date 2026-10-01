#!/usr/bin/env python3
"""Small, fail-closed public wrapper around the stock-board runtime."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import socket
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / "kernel" / "stock-board"
IMAGE = "pinology:local"
sys.path.insert(0, str(RUNTIME))
import settings as runtime_settings


def host():
    system, machine = platform.system(), platform.machine().lower()
    if system == "Darwin" and machine == "arm64":
        return "mac"
    if system == "Linux" and machine in ("aarch64", "arm64"):
        return "linux"
    raise RuntimeError("Pinology supports Apple Silicon macOS or Linux aarch64 only")


def run(cmd, **kwargs):
    return subprocess.run(cmd, check=True, **kwargs)


def regular(path: Path, label: str):
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"{label} must be a regular file: {path}")
    return path.resolve()


def new_instance(path: Path):
    path = path.expanduser().absolute()
    if path.exists() or path.is_symlink():
        raise ValueError(f"Instance already exists: {path}")
    if not path.parent.is_dir() or "," in str(path) or "\n" in str(path):
        raise ValueError("Instance parent must exist and path may not contain comma/newline")
    return path


def existing(path: Path):
    path = path.expanduser().resolve(strict=True)
    if not (path / "instance.json").is_file() or (path / "instance.json").is_symlink():
        raise ValueError(f"Not a Pinology instance: {path}")
    return path


def serial_args(value):
    return ["--serial", value] if value else []


def native_runtime():
    bundle = ROOT / "dist" / "pinology-macos"
    if not bundle.is_dir():
        raise RuntimeError("Build dist/pinology-macos first")
    return bundle / "runtime" / "instance.py", bundle / "bin" / "qemu-system-aarch64", bundle / "bin" / "qemu-img"


def docker_mount(path: Path, target: str, readonly=False):
    path = Path(path).expanduser().absolute()
    if "," in str(path) or "\n" in str(path):
        raise ValueError("Docker mount paths may not contain comma/newline")
    mode = ",readonly" if readonly else ""
    return ["--mount", f"type=bind,src={path},dst={target}{mode}"]


def docker_instance(path: Path, target="/data"):
    return docker_mount(path, target, False)


def docker_name(path):
    return "pinology-" + hashlib.sha256(str(path).encode()).hexdigest()[:16]


def docker_name_available(name):
    result = subprocess.run(["docker", "ps", "-a", "--filter", f"name=^{name}$", "--format", "{{.Names}}"],
                            check=True, capture_output=True, text=True)
    if result.stdout.strip():
        raise ValueError(f"Docker container name already exists: {name}")


def cmd_init(args):
    instance = new_instance(Path(args.path))
    model = args.model
    serial = serial_args(args.serial)
    pat = regular(Path(args.pat), "PAT") if args.pat else None
    artifacts = Path(args.artifacts).expanduser().resolve(strict=True) if args.artifacts else None
    if artifacts and (artifacts.is_symlink() or not artifacts.is_dir()):
        raise ValueError("Artifacts must be a real directory")
    if host() == "mac":
        runtime, _, qemu_img = native_runtime()
        command = [sys.executable, str(runtime), "--qemu-img", str(qemu_img), "init", str(instance),
                   "--model", model, "--disk-size", args.disk_size] + serial
        if pat:
            command += ["--pat", str(pat)]
        if artifacts:
            command += ["--artifacts", str(artifacts)]
        run(command, cwd=instance.parent, env=clean_env())
        return
    command = ["docker", "run", "--rm", "--user", f"{os.getuid()}:{os.getgid()}",
               *docker_mount(instance.parent, "/parent")]
    app_args = []
    if pat:
        command += docker_mount(pat, "/input/boot.pat", True)
        app_args += ["--pat", "/input/boot.pat"]
    if artifacts:
        command += docker_mount(artifacts, "/input/artifacts", True)
        app_args += ["--artifacts", "/input/artifacts"]
    if pat and artifacts:
        command += ["--network", "none"]
    command += [IMAGE, "init", f"/parent/{instance.name}", "--model", model,
                "--disk-size", args.disk_size] + serial + app_args
    run(command, cwd=ROOT, env=clean_env())


def clean_env():
    env = dict(os.environ)
    for key in runtime_settings.DEFAULTS:
        env.pop(key, None)
    return env


def docker_kvm_args():
    device = Path("/dev/kvm")
    if not device.is_char_device():
        raise RuntimeError("Linux KVM requires /dev/kvm")
    return ["--device", "/dev/kvm", "--group-add", str(device.stat().st_gid)]


def docker_identity(instance):
    name = docker_name(instance)
    result = subprocess.run(["docker", "inspect", "--format", "{{json .}}", name],
                            check=True, capture_output=True, text=True)
    try:
        container = json.loads(result.stdout)
    except (TypeError, json.JSONDecodeError):
        raise ValueError("Could not verify Pinology container mounts") from None
    expected = str(instance)
    labels = container.get("Config", {}).get("Labels", {})
    mounts = container.get("Mounts", [])
    if labels.get("org.pinology.instance") != expected or not any(
            m.get("Destination") == "/data" and Path(m.get("Source", "")).resolve() == Path(expected).resolve()
            for m in mounts if isinstance(m, dict)):
        raise ValueError("Pinology container identity does not match instance")
    return name


def network_options(args):
    mode = getattr(args, 'network', 'local')
    address = getattr(args, 'bind_address', None)
    if mode == 'local':
        if address not in (None, '127.0.0.1'):
            raise ValueError('A non-loopback bind requires --network host')
        address = '127.0.0.1'
    elif mode == 'host':
        if address is None:
            address = '0.0.0.0'
        address = runtime_settings.bind_address(address)
        if address.startswith('127.'):
            raise ValueError('Use --network local for loopback')
    else:
        raise ValueError('Unsupported network mode; router DHCP/bridging is not implemented')
    https = getattr(args, 'https_port', 5001)
    extra = f'tcp:{https}:5001' if https is not None else ''
    forwards = runtime_settings.forwards(args.port, args.smb_port, extra)
    return address, extra, forwards


def check_native_ports(address, forwards):
    # Diagnostic preflight only, not a reservation: QEMU still binds atomically
    # at launch and remains authoritative if another process wins the race.
    for _protocol, port, guest in forwards:
        flag = {5000: '--port', 5001: '--https-port', 445: '--smb-port'}[guest]
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
                sock.bind((address, port))
        except OSError as error:
            raise RuntimeError(f'Cannot bind {address}:{port}: {error}. '
                               f'Choose another port with {flag}; no service was stopped or privileges elevated.') from None


def cmd_start(args):
    address, extra, forwards = network_options(args)
    instance = existing(Path(args.path))
    accel = "kvm" if host() == "linux" else "hvf"
    if args.experimental_gicv2 and host() != "linux":
        raise ValueError("--experimental-gicv2 is supported only on Linux aarch64 KVM")
    if host() == "mac":
        runtime, qemu, qemu_img = native_runtime()
        check_native_ports(address, forwards)
        command = [sys.executable, str(runtime), "--qemu-img", str(qemu_img), "run", str(instance),
                   "--qemu", str(qemu), "--accel", accel, "--flash", "--port", str(args.port),
                   "--smb-port", str(args.smb_port), "--bind-address", address,
                   "--network", "user", "--console-socket", "--restart-on-guest-reset"]
        if extra:
            command += ["--user-ports", extra]
        if args.experimental_ds423:
            command.append("--experimental-ds423")
        print_network(address, forwards)
        run(command, cwd=instance, env=clean_env())
        return
    kvm = docker_kvm_args()
    name = docker_name(instance)
    docker_name_available(name)
    command = ["docker", "run", "--rm", "--name", name, *kvm,
               "--label", f"org.pinology.instance={instance}",
               "--user", f"{os.getuid()}:{os.getgid()}", *docker_instance(instance)]
    # Publish native host ports, but never require low-port privileges for the
    # unprivileged QEMU process inside the isolated container.
    container_ports = {5000: 5000, 5001: 5001, 445: 14445}
    for protocol, port, guest in forwards:
        command += ["-p", f"{address}:{port}:{container_ports[guest]}/{protocol}"]
    command += [
               "-e", "STORAGE=/data", "-e", "ACCEL=kvm", "-e", "BIND_ADDRESS=0.0.0.0", IMAGE,
               "run", "/data", "--accel", "kvm", "--port", "5000", "--smb-port", "14445"]
    if extra:
        command += ["--user-ports", "tcp:5001:5001"]
    if args.experimental_ds423:
        command.append("--experimental-ds423")
    if args.experimental_gicv2:
        command.append("--experimental-gicv2")
    print_network(address, forwards)
    run(command, cwd=ROOT, env=clean_env())


def print_network(address, forwards):
    display = 'HOST_IPV4' if address == '0.0.0.0' else address
    if address != '127.0.0.1':
        print('WARNING: DSM ports will be reachable on the selected host address. '
              'Use a trusted LAN; no router DHCP lease or firewall rules are created.', flush=True)
        if address == '0.0.0.0':
            print('WARNING: 0.0.0.0 includes LAN, VPN and other IPv4 interfaces.', flush=True)
    for _protocol, port, guest in forwards:
        scheme = {5000: 'http', 5001: 'https', 445: 'smb'}[guest]
        print(f'{scheme.upper()}: {scheme}://{display}:{port}/', flush=True)


def cmd_status(args):
    instance = existing(Path(args.path))
    if host() == "mac":
        runtime, _, _ = native_runtime()
        run([sys.executable, str(RUNTIME / "instance-qmp.py"), str(instance), "status"], cwd=instance)
    else:
        name = docker_identity(instance)
        run(["docker", "run", "--rm", "--network", f"container:{name}",
             *docker_instance(instance), IMAGE, "health", "/data"], cwd=ROOT)


def cmd_stop(args):
    instance = existing(Path(args.path))
    credentials = regular(Path(args.credentials), "Credentials")
    if credentials.stat().st_mode & 0o777 not in (0o400, 0o600):
        raise ValueError("Credentials must have mode 0600 or 0400")
    if host() == "mac":
        runtime, _, qemu_img = native_runtime()
        run([sys.executable, str(runtime), "--qemu-img", str(qemu_img), "stop", str(instance),
             "--shutdown-credentials", str(credentials)], cwd=instance, env=clean_env())
    else:
        name = docker_identity(instance)
        run(["docker", "run", "--rm", "--network", f"container:{name}", "--user", f"{os.getuid()}:{os.getgid()}",
             *docker_instance(instance), *docker_mount(credentials, "/run/credentials.json", True), IMAGE,
             "stop", "/data", "--shutdown-credentials", "/run/credentials.json"], cwd=ROOT)


def cmd_backup(args):
    source = existing(Path(args.path))
    target = Path(args.destination).expanduser().absolute()
    if target.exists() or target.is_symlink():
        raise ValueError(f"Backup destination already exists: {target}")
    if not target.parent.is_dir():
        raise ValueError("Backup parent must exist")
    if host() == "mac":
        _, _, qemu_img = native_runtime()
        run([sys.executable, str(RUNTIME / "checkpoint.py"), str(source), str(target),
             "--qemu-img", str(qemu_img)], cwd=source)
    else:
        run(["docker", "run", "--rm", "--user", f"{os.getuid()}:{os.getgid()}",
             *docker_mount(source.parent, "/source"), *docker_mount(target.parent, "/backup"),
             "--entrypoint", "/opt/pat-python/bin/python", IMAGE,
             "/opt/ds223/runtime/checkpoint.py", f"/source/{source.name}",
             f"/backup/{target.name}"], cwd=ROOT)


def mac_sdk():
    result = subprocess.run(["xcrun", "--sdk", "macosx", "--show-sdk-version"],
                            check=True, capture_output=True, text=True)
    version = result.stdout.strip()
    parts = version.split(".")
    if len(parts) < 2 or not all(part.isdigit() for part in parts):
        raise RuntimeError("Cannot determine macOS SDK version")
    if tuple(map(int, parts[:2])) < (15, 2):
        raise RuntimeError("QEMU HVF build requires macOS SDK 15.2+ (Xcode/Command Line Tools 16.2+)")
    return version


def cmd_build(_args):
    kind = host()
    if kind == "mac":
        mac_sdk()
        meson = shutil.which("meson")
        if not meson:
            raise RuntimeError("meson is required; install prerequisites manually")
        destination = ROOT / "dist" / "pinology-macos"
        if destination.exists():
            raise ValueError(f"Refusing to overwrite existing bundle: {destination}")
        work = ROOT / "work" / "macos"
        env = clean_env() | {"MESON": meson, "DSM_QEMU_WORK": str(work),
                              "DSM_QEMU_BUILD": str(work / "qemu-ds223-build"), "DSM_QEMU_SLIRP": "1",
                              "PKG_CONFIG_PATH": str(work / "libslirp-hostfwd" / "lib" / "pkgconfig")}
        run(["bash", str(RUNTIME / "build-slirp.sh")], cwd=RUNTIME, env=env)
        run(["bash", str(RUNTIME / "build-qemu.sh")], cwd=RUNTIME, env=env)
        destination.parent.mkdir(parents=True, exist_ok=True)
        run([sys.executable, str(RUNTIME / "bundle-macos.py"),
             str(work / "qemu-ds223-build" / "qemu-system-aarch64"), str(destination)], cwd=RUNTIME)
    else:
        run(["docker", "build", "-t", IMAGE, str(RUNTIME)], cwd=ROOT)


def cmd_doctor(_args):
    kind = host()
    required = ["docker"] if kind == "linux" else ["meson", "qemu-img", "gpatch", "ninja", "pkg-config", "cc", "curl", "openssl"]
    result = {"host": kind, "python": sys.version.split()[0], **{name: bool(shutil.which(name)) for name in required}}
    if kind == "linux":
        result["kvm"] = Path("/dev/kvm").is_char_device()
        result["ready"] = result["docker"] and result["kvm"]
    else:
        result["macos_sdk"] = mac_sdk()
        hvf = subprocess.run(["sysctl", "-n", "kern.hv_support"], capture_output=True, text=True).stdout.strip() == "1"
        result["hvf"] = hvf
        result["ready"] = hvf and all(result.values())
    print(json.dumps(result, indent=2, sort_keys=True))
    if not result["ready"]:
        raise RuntimeError("Host prerequisites are incomplete")


def parser():
    p = argparse.ArgumentParser(prog="pinology")
    sub = p.add_subparsers(dest="command", required=True)
    sub.add_parser("doctor")
    sub.add_parser("build")
    init = sub.add_parser("init"); init.add_argument("path"); init.add_argument("--model", choices=("DS223", "DS423"), default="DS223"); init.add_argument("--disk-size", default="32G"); init.add_argument("--serial", default=""); init.add_argument("--pat"); init.add_argument("--artifacts")
    start = sub.add_parser("start"); start.add_argument("path")
    start.add_argument('--port', type=int, default=5000, help='Host HTTP port (default: 5000)')
    start.add_argument('--smb-port', type=int, default=445, help='Host SMB port (default: 445)')
    start.add_argument('--experimental-ds423', action='store_true'); start.add_argument('--experimental-gicv2', action='store_true')
    start.add_argument('--network', choices=('local', 'host'), default='local',
                       help='local: loopback only (default); host: listen on 0.0.0.0 unless --bind-address overrides it')
    start.add_argument('--bind-address', help='Optional host IPv4 restriction; host mode defaults to 0.0.0.0')
    start.add_argument('--https-port', type=int, default=5001, help='Host HTTPS port (default: 5001)')
    for name in ("status",):
        q = sub.add_parser(name); q.add_argument("path")
    stop = sub.add_parser("stop"); stop.add_argument("path"); stop.add_argument("--credentials", required=True)
    backup = sub.add_parser("backup"); backup.add_argument("path"); backup.add_argument("destination")
    sub.add_parser("menu")
    return p


def main(argv=None):
    args = parser().parse_args(argv)
    try:
        if args.command == "doctor": cmd_doctor(args)
        elif args.command == "build": cmd_build(args)
        elif args.command == "init": cmd_init(args)
        elif args.command == "start": cmd_start(args)
        elif args.command == "status": cmd_status(args)
        elif args.command == "stop": cmd_stop(args)
        elif args.command == "backup": cmd_backup(args)
        elif args.command == "menu":
            if host() != "mac": raise RuntimeError("menu is available only on Apple Silicon macOS")
            runtime, _, _ = native_runtime(); run([sys.executable, str(runtime.parent / "menu.py"),
                                                   "--bundle", str(runtime.parent.parent)], cwd=runtime.parent, env=clean_env())
        return 0
    except KeyboardInterrupt:
        print("Interrupted. This is NOT a confirmed guest shutdown. Check status and shut down inside DSM.", file=sys.stderr)
        return 130
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        print(f"pinology: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
