#!/usr/bin/env python3
"""Isolated stock DS223; Python >=3.11, optional bounded guest-reset supervisor."""
import argparse
import datetime
import fcntl
import hashlib
import json
import lzma
import os
from pathlib import Path
import shutil
import stat
import subprocess
import time
import signal
import settings
import vendor_identity
from profiles import profile, validate_manifest
import guest_control
import lease
from boot_from_flash import extract as extract_flash_boot
from lifecycle import ShutdownObserver, should_restart

BUILD = "7.2.2-72806"
HASHES = {
    "zImage": "c68a2740943f696a0339471c2a463e2519e3b6fea5a27b2f357d847ee985c1f4",
    "Image.stock": "4c6584070c2bfee8a7ab1b73ef74d1872a0469700e61fc806e6b72d23e576b14",
    "rd.bin": "43aaeaf04ec30962c722c3ce531cd3f5b3cf72f30554ccce62b0041db960f593",
    "model.dtb": "b582240773095ab2fca981923885cb6331a2825a45471bc3c3df7eaf8793c387",
}
PAT_HASH = "18f175f4488e92a91c3dcbcb7ec2f4cae3d8b1105f581fd885ab49693a5fe6f3"
GUEST_RESTART = object()


def accelerator_args(name):
    # Explicit choice; an HVF failure must never silently fall back to TCG.
    if name == "tcg":
        return ["-accel", "tcg,thread=single", "-cpu", "cortex-a55"]
    if name == "hvf":
        return ["-accel", "hvf,kernel-irqchip=off", "-cpu", "host"]
    if name == 'kvm':
        return ['-accel', 'kvm', '-cpu', 'host']
    raise ValueError(f"Unknown accelerator: {name}")


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def regular(path):
    if path.is_symlink() or not stat.S_ISREG(path.stat().st_mode):
        raise ValueError(f"Not a regular, non-symlink file: {path}")


def verify(path, expected):
    regular(path)
    if digest(path) != expected:
        raise ValueError(f"SHA-256 mismatch: {path}")


def prepare(args):
    serial = vendor_identity.validate_serial(getattr(args, 'serial', ''))
    # No reuse, replacement or adoption of an existing disk/directory.
    if args.instance.exists() or args.instance.is_symlink():
        raise ValueError("Instance already exists; prepare never overwrites it")
    model = getattr(args, 'model', 'DS223')
    spec = profile(model)
    hashes = spec['hashes']
    fmt = getattr(args, 'disk_format', 'qcow2')
    size = getattr(args, 'disk_size', 32 * 1024**3)
    if fmt not in ('raw', 'qcow2') or not 8 * 1024**3 <= size <= 16 * 1024**4:
        raise ValueError('Invalid new disk format or size')
    verify(args.pat, spec['pat_sha256'])
    for name in ("zImage", "rd.bin", "model.dtb"):
        verify(args.artifacts / name, hashes[name])
    image = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(
        (args.artifacts / "zImage").read_bytes())
    if hashlib.sha256(image).hexdigest() != hashes["Image.stock"]:
        raise ValueError("Unexpected decompressed kernel")
    args.instance.mkdir(mode=0o700)
    boot = args.instance / "boot"
    boot.mkdir()
    for name in ("zImage", "rd.bin", "model.dtb"):
        shutil.copyfile(args.artifacts / name, boot / name)
    (boot / "Image.stock").write_bytes(image)
    disk = 'disk.' + fmt
    command = [args.qemu_img, 'create', '-f', fmt]
    if getattr(args, 'allocate', False):
        command += ['-o', 'preallocation=full']
    subprocess.run(command + [str(args.instance / disk), str(size)], check=True)
    manifest = {"model": model, "build": spec['build'], "hashes": hashes,
                "pat_sha256": spec['pat_sha256'], "disk": disk,
                "disk_format": fmt, "disk_size": size}
    # init supplies serial even when empty: pin absence as well as explicit identity.
    if hasattr(args, 'serial'):
        manifest['identity'] = vendor_identity.identity(serial)
    (args.instance / "instance.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Prepared {args.instance}: {model}, one new {size // 1024**3} GiB {fmt} disk")
    if not spec['boot_supported']:
        print(f'NOTICE: {model} is experimental; run requires --experimental-ds423 (second NIC unsupported).')


def run(args):
    accel = getattr(args, "accel", "tcg")
    experimental_gicv2 = getattr(args, "experimental_gicv2", False)
    if experimental_gicv2 and accel not in ("kvm", "tcg"):
        raise ValueError("Experimental GICv2 requires KVM or TCG acceleration")
    acceleration = accelerator_args(accel)
    smb_port = getattr(args, "smb_port", None)
    if smb_port is not None and (not 1 <= smb_port <= 65535 or smb_port == args.port):
        raise ValueError('SMB port must be 1..65535 and distinct from HTTP')
    instance = args.instance.resolve(strict=True)
    if ',' in str(instance) or '\n' in str(instance):
        raise ValueError('Instance path may not contain comma or newline')
    if (instance / 'resize.pending.json').exists():
        raise ValueError('Unfinished disk resize; restore the recorded cold checkpoint')
    if (instance / 'runtime.json').is_symlink():
        raise ValueError('Refusing symlink runtime metadata')
    manifest_path = instance / "instance.json"
    regular(manifest_path)
    manifest = json.loads(manifest_path.read_text())
    model, spec, disk_format, disk_size = validate_manifest(manifest)
    if getattr(args, 'experimental_second_nic', False) and model != 'DS423':
        raise ValueError('Experimental second PCI NIC requires DS423')
    if not spec['boot_supported'] and not (
            model == 'DS423' and getattr(args, 'experimental_ds423', False)):
        raise ValueError(f'{model}: installation media prepared, but boot is blocked until PCIe AHCI/NIC emulation is implemented')
    if model == 'DS423':
        nic_state = ('experimental RTL8168 endpoint enabled' if
                     getattr(args, 'experimental_second_nic', False) else
                     'second Ethernet endpoint disabled')
        print(f'EXPERIMENTAL DS423: PCIe AHCI under validation; {nic_state}.', flush=True)
    for name, expected in spec['hashes'].items():
        verify(instance / "boot" / name, expected)
    disk = instance / manifest['disk']
    regular(disk)
    qemu = str(Path(shutil.which(args.qemu) or args.qemu).resolve(strict=True))
    # Lock is held by both this supervisor and QEMU, including if the parent dies.
    lock_fd = os.open(instance / "run.lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    with os.fdopen(lock_fd, "r+") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError("Instance already running (lifetime lock held)") from None
        info = json.loads(subprocess.check_output(
            [args.qemu_img, "info", "--output=json", str(disk)], text=True))
        if (info.get("format") != disk_format or "backing-filename" in info
                or info.get("virtual-size") != disk_size
                or info.get("format-specific", {}).get("data", {}).get("corrupt")
                or info.get("format-specific", {}).get("data", {}).get("data-file")):
            raise ValueError("Disk does not match manifest, or has backing/external data/corruption")
        flash_args = []
        flash_boot = None
        boot_build = BUILD
        if not args.flash and (instance / "flash.bin").exists():
            raise ValueError("This instance has persistent NOR; use --flash (no stale-image fallback)")
        if not args.flash:
            vendor_identity.check_identity(None, manifest, getattr(args, 'serial', ''))
        cmdline = "console=ttyS0,460800 earlycon=uart8250,mmio32,0x98007800 loglevel=8 panic=0"
        if args.flash:
            flash = instance / "flash.bin"
            regular(flash)
            if flash.stat().st_size != 0x1000000:
                raise ValueError("Expected 16 MiB NOR image")
            media = flash.read_bytes()
            vendor_identity.check_identity(media, manifest, getattr(args, 'serial', ''))
            boot_build, flash_boot = extract_flash_boot(media, model=model)
            flash_args = ["-drive", "if=pflash,index=0,format=raw,file=flash.bin"]
            # Normal model-specific firmware metadata from unmodified U-Boot.
            # QEMU still directly loads the kernel; it does not execute U-Boot.
            netifs, firmware = (2, 'M.115') if model == 'DS423' else (1, 'M.215')
            cmdline += (f" root=/dev/md0 netif_num={netifs} syno_hw_version={model} "
                        f"syno_fw_version={firmware} vender_format_version=2 "
                        "uio_pdrv_genirq.of_id=generic-uio "
                        "mtdparts=RtkSFC:1600k(u-boot),7360k(kernel),64k(dtb),"
                        "7188k(rootfs),64k(vendor),96k(oops),8k(misc_info),4k(FIS_directory)")
        control = instance / "control.sock"
        if control.is_symlink():
            raise ValueError("Refusing symlink control socket")
        if control.exists():
            if not stat.S_ISSOCK(control.stat().st_mode):
                raise ValueError("Control socket path occupied")
            control.unlink()  # stale socket only, after taking the lifetime lock
        status_socket = instance / 'status.sock'
        if status_socket.is_symlink():
            raise ValueError('Refusing symlink status socket')
        if status_socket.exists():
            if not stat.S_ISSOCK(status_socket.stat().st_mode):
                raise ValueError('Status socket path occupied')
            status_socket.unlink()
        if getattr(args, 'console_socket', False):
            console_socket = instance / 'console.sock'
            if console_socket.is_symlink():
                raise ValueError('Refusing symlink console socket')
            if console_socket.exists():
                if not stat.S_ISSOCK(console_socket.stat().st_mode):
                    raise ValueError('Console socket path occupied by a non-socket')
                console_socket.unlink()  # instance lock excludes the prior VM
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        logs = instance / "logs"
        logs.mkdir(exist_ok=True)
        prefix = logs / f"{stamp}-{os.getpid()}"
        boot_dir = Path("boot")
        if flash_boot is not None:
            # Fresh derived files from persisted flash, never old cached images.
            boot_dir = Path("logs") / f"{stamp}-{os.getpid()}.boot"
            (instance / boot_dir).mkdir()
            for name, data in flash_boot.items():
                (instance / boot_dir / name).write_bytes(data)
        bind = settings.bind_address(getattr(args, 'bind_address', '127.0.0.1'))
        forwards = settings.forwards(args.port, smb_port, getattr(args, 'user_ports', ''))
        network = 'user,id=ds223net' + ''.join(f',hostfwd={proto}:{bind}:{host}-:{guest}'
                                             for proto, host, guest in forwards)
        enabled_network = getattr(args, 'network', 'user') != 'N'
        machine = model.lower() + (",experimental-gicv2=on" if experimental_gicv2 else "")
        command = [qemu, "-name", f"{model.lower()}-stock-72806", "-machine", machine] + acceleration + [
                   "-smp", "4", "-m", "2048", "-display", "none", "-monitor", "none",
                   "-serial", "stdio", "-no-reboot", "-qmp", "unix:control.sock,server=on,wait=off",
                   "-qmp", "unix:status.sock,server=on,wait=off",
                   "-kernel", str(boot_dir / "Image.stock"),
                   "-initrd", str(boot_dir / "rd.bin"), "-dtb", str(boot_dir / "model.dtb"),
                   "-append", cmdline,
                   "-drive", f"if=none,id=data,file={manifest['disk']},format={disk_format}",
                   "-device", f"ide-hd,drive=data,bus=ide.0,serial={model}-LAB-72806",
                   "-d", "guest_errors,unimp", "-D", str(prefix) + ".qemu.log"] + flash_args
        if enabled_network:
            if getattr(args, 'experimental_second_nic', False):
                command += ['-nic', network + ',model=rtd1619b-net' +
                            (',mac=' + args.mac if getattr(args, 'mac', '') else '')]
            else:
                command += ['-netdev', network, '-global', 'rtd1619b-net.netdev=ds223net']
        else:
            command += ['-net', 'none']
        if getattr(args, 'experimental_second_nic', False):
            nic = 'rtd-r8168-pci,bus=/pcie1/pci,addr=0.0'
            if enabled_network:
                command += ['-netdev', 'user,id=ds423net2,net=10.0.3.0/24']
                nic += ',netdev=ds423net2'
            command += ['-device', nic]
        if getattr(args, 'mac', '') and not getattr(args, 'experimental_second_nic', False):
            command += ['-global', 'rtd1619b-net.mac=' + args.mac]
        if getattr(args, 'console_socket', False):
            command[command.index('-serial') + 1] = 'chardev:serial0'
            command += ['-chardev', 'socket,id=serial0,path=console.sock,server=on,wait=off,logfile=' +
                        str(prefix) + '.serial.log']
        if getattr(args, "trace_microp", False):
            # UART1 is the management MCU, never the login console. This
            # sink observes requests but cannot synthesize sensor readings.
            command += ["-chardev", "ringbuf,id=microp,size=65536,logfile=" +
                        str(prefix) + ".microp.bin", "-serial", "chardev:microp"]
        if getattr(args, "trace_ahci", False):
            for event in ("ahci_*", "ide_bus_exec_cmd", "handle_cmd_fis_dump",
                          "handle_reg_h2d_fis_dump", "process_ncq_command",
                          "execute_ncq_command_*", "ncq_finish"):
                command += ["-trace", f"enable={event}"]
        meta = {"command": command, "cwd": str(instance), "qemu_sha256": digest(Path(qemu)),
                "build": boot_build, "http": f"http://{bind if bind != '0.0.0.0' else '127.0.0.1'}:{args.port}",
                "bind_address": bind,
                "automatic_reboot": bool(getattr(args, 'restart_on_guest_reset', False)), "packet_capture": False,
                "microp": {"current_sensor": "unavailable",
                           "tx_trace": bool(getattr(args, "trace_microp", False)),
                           "synthetic_readings": False}}
        Path(str(prefix) + ".json").write_text(json.dumps(meta, indent=2) + "\n")
        serial_log = str(prefix) + ('.serial.log' if getattr(args, 'console_socket', False) else '.console.log')
        reboot_mode = ('verified guest resets restart via supervisor' if meta['automatic_reboot']
                       else 'guest reboot stops QEMU')
        print(f"Console: {serial_log}\nHTTP: {meta['http']}\n"
              f"Boot build: {boot_build}; {reboot_mode} (no host autostart).", flush=True)
        with Path(str(prefix) + ".console.log").open("xb") as console:
            process = subprocess.Popen(command, cwd=instance, stdout=console,
                                       stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                       pass_fds=(lock.fileno(),), start_new_session=True, umask=0o077)
            print(f"QEMU pid: {process.pid}", flush=True)
            state = {'port': args.port, 'network': 'user' if enabled_network else 'N',
                     'bind_address': bind,
                     'started': time.time(), 'startup_timeout': getattr(args, 'startup_timeout', 600),
                     'pid': int(process.pid), 'model': model}
            state_path = instance / 'runtime.json'
            state_path.write_text(json.dumps(state) + '\n')
            stopping = False
            stop_deadline = None
            old_handlers = {}
            def request_stop(signum, frame):
                nonlocal stopping
                stopping = True
            if getattr(args, 'shutdown_credentials', None):
                for sig in (signal.SIGTERM, signal.SIGINT):
                    old_handlers[sig] = signal.signal(sig, request_stop)
            observer = None
            if getattr(args, 'restart_on_guest_reset', False):
                try:
                    observer = ShutdownObserver(instance, process, Path(str(prefix) + '.events.jsonl'))
                except RuntimeError as error:
                    # Do not kill a running NAS because diagnostics failed.
                    print(str(error), flush=True)
            while True:
                try:
                    result = process.wait(timeout=1) if old_handlers else process.wait()
                    break
                except subprocess.TimeoutExpired:
                    if stopping:
                        stopping = False
                        try:
                            stop_deadline = time.monotonic() + getattr(args, 'shutdown_timeout', 105)
                            accepted = guest_control.poweroff(instance, args.shutdown_credentials)
                            print(('DSM accepted graceful shutdown' if accepted else
                                   'DSM shutdown response lost; outcome unknown') +
                                  '; waiting for guest poweroff.', flush=True)
                        except (OSError, ValueError) as error:
                            print(f'Graceful shutdown unavailable: {error}; VM NOT power-cut.', flush=True)
                    if stop_deadline is not None and time.monotonic() >= stop_deadline:
                        print('Shutdown timeout: VM retained; Docker may enforce its external stop grace limit.', flush=True)
                        stop_deadline = None
                except KeyboardInterrupt:
                    # This board has no ACPI power button. Never silently cut
                    # power during installation or to a mounted persistent disk.
                    print("Still running. Shut down in DSM; forced QMP quit is a hard power-off.", flush=True)
            for sig, handler in old_handlers.items():
                signal.signal(sig, handler)
        print(f"QEMU exit: {result}; disk preserved", flush=True)
        event = observer.finish() if observer else None
        if should_restart(result, event):
            return GUEST_RESTART  # Cannot collide with a QEMU process exit code.
        return result if result >= 0 else 128 - result


def supervise(args):
    with lease.hold(args.instance):
        return _supervise(args)


def _supervise(args):
    if not getattr(args, 'restart_on_guest_reset', False):
        return run(args)
    root = args.instance.resolve(strict=True)
    fd = os.open(root / 'supervisor.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'r+') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError('Instance supervisor already running') from None
        resets = []
        while True:
            result = run(args)
            if result is not GUEST_RESTART:
                return result
            now = time.monotonic()
            resets = [t for t in resets if now - t < 300]
            if len(resets) >= 3:
                print('Reboot rate limit reached (3 in 5 minutes); instance left stopped.', flush=True)
                return 70
            resets.append(now)
            print('Verified guest reset: reload current flash and restart (no host autostart).', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu-img", default="qemu-img")
    parser.add_argument('--config', type=Path, help='Strict KEY=value file; environment overrides it, CLI wins')
    sub = parser.add_subparsers(dest="action", required=True)
    create = sub.add_parser("prepare")
    create.add_argument("instance", type=Path, nargs='?')
    create.add_argument("--pat", required=True, type=Path)
    create.add_argument("--artifacts", required=True, type=Path)
    init = sub.add_parser('init', help='New model-specific instance + verified PAT + flash')
    init.add_argument('instance', type=Path, nargs='?')
    init.add_argument('--pat', type=Path)
    init.add_argument('--artifacts', type=Path)
    init.add_argument('--url')
    init.add_argument('--serial', help='Optional serial for a NEW VM; never changes an existing VM')
    for item in (create, init):
        item.add_argument('--model', choices=('DS223', 'DS423'))
        item.add_argument('--disk-size')
        item.add_argument('--disk-format', choices=('qcow2', 'raw'))
        item.add_argument('--allocate', action=argparse.BooleanOptionalAction, default=None)
    boot = sub.add_parser("run")
    boot.add_argument("instance", type=Path, nargs='?')
    boot.add_argument("--qemu", required=True)
    boot.add_argument("--port", type=int)
    boot.add_argument("--smb-port", type=int, help="Optional loopback-only SMB forward to guest port 445")
    boot.add_argument("--accel", choices=("tcg", "hvf", "kvm"),
                      help="CPU acceleration; TCG remains the device-correctness baseline")
    boot.add_argument('--experimental-gicv2', action='store_true',
                      help='Developer opt-in to the experimental GICv2 board property; KVM or TCG only')
    boot.add_argument("--flash", action="store_true", help="Attach this instance's 16 MiB flash.bin")
    boot.add_argument('--experimental-ds423', action=argparse.BooleanOptionalAction, default=None,
                      help='Developer opt-in to incomplete DS423 PCIe board; never enables stable model support')
    boot.add_argument('--experimental-second-nic', action='store_true',
                      help='Unvalidated synthetic RTL8168B PCI prototype, DS423 only')
    boot.add_argument("--trace-ahci", action="store_true", help="Diagnostic AHCI command tracing")
    boot.add_argument("--trace-microp", action="store_true",
                      help="Record UART1 MCU requests; no sensor replies are synthesized")
    boot.add_argument('--restart-on-guest-reset', action='store_true',
                      help='Restart only after QMP-confirmed guest reset; stop on poweroff/crash; max 3 per 5 minutes')
    boot.add_argument('--bind-address', type=settings.bind_address,
                      help='Literal host IPv4 for forwards; 0.0.0.0 exposes all interfaces')
    boot.add_argument('--console-socket', action='store_true',
                      help='Local instance console.sock plus per-boot serial log; no network listener')
    boot.add_argument('--mac')
    boot.add_argument('--network', choices=('user', 'N'))
    boot.add_argument('--user-ports')
    boot.add_argument('--shutdown-credentials', type=Path)
    boot.add_argument('--startup-timeout', type=int)
    sub.add_parser('config', help='Print effective non-secret settings')
    for action in ('health', 'stop'):
        item = sub.add_parser(action)
        item.add_argument('instance', type=Path, nargs='?')
        if action == 'stop':
            item.add_argument('--shutdown-credentials', type=Path)
    grow = sub.add_parser('grow', help='Offline growth only; a NEW cold checkpoint is mandatory')
    grow.add_argument('instance', type=Path, nargs='?')
    grow.add_argument('--size', required=True)
    grow.add_argument('--checkpoint', required=True, type=Path)
    args = parser.parse_args()
    try:
        conf = settings.load(args.config)
        if args.action == 'config':
            if conf['SERIAL']:
                conf['SERIAL'] = '<set>'
            print(json.dumps(conf, indent=2))
            return 0
        if args.instance is None:
            if not conf['STORAGE']:
                raise ValueError('Specify instance directory or STORAGE')
            args.instance = Path(conf['STORAGE'])
        if args.action in ('prepare', 'init'):
            args.model = args.model or conf['MODEL']
            args.disk_size = settings.size(args.disk_size or conf['DISK_SIZE'])
            args.disk_format = args.disk_format or conf['DISK_FMT']
            args.allocate = args.allocate if args.allocate is not None else conf['ALLOCATE'] == 'Y'
            if args.action == 'prepare':
                if conf['SERIAL']:
                    raise ValueError('SERIAL requires init, which creates persistent NOR')
                return prepare(args)
            from prepare_media import initialize
            args.serial = args.serial if args.serial is not None else conf['SERIAL']
            args.pat = args.pat or (Path(conf['PAT_FILE']) if conf['PAT_FILE'] else None)
            args.url = args.url or conf['URL']
            return initialize(args)
        if args.action == 'health':
            code, message = guest_control.health(args.instance.resolve(strict=True))
            print(message)
            return code
        if args.action == 'grow':
            from storage import grow_disk
            return grow_disk(args.instance, settings.size(args.size), args.checkpoint, args.qemu_img)
        args.shutdown_credentials = args.shutdown_credentials or (Path(conf['SHUTDOWN_CREDENTIALS']) if conf['SHUTDOWN_CREDENTIALS'] else None)
        if args.action == 'stop':
            if not args.shutdown_credentials:
                raise ValueError('SHUTDOWN_CREDENTIALS is required; no forced stop fallback')
            root = args.instance.resolve(strict=True)
            deadline = time.monotonic() + int(conf['TIMEOUT'])
            accepted = guest_control.poweroff(root, args.shutdown_credentials)
            if not accepted:
                print('Shutdown response lost; outcome unknown, waiting for lifetime locks', flush=True)
            while time.monotonic() < deadline:
                if guest_control.stopped(root):
                    print('Guest stopped')
                    return 0
                time.sleep(1)
            raise ValueError('Shutdown timed out; VM was NOT power-cut')
        for field, key in dict(port='HTTP_PORT', smb_port='SMB_PORT', accel='ACCEL',
                               bind_address='BIND_ADDRESS', network='NETWORK', mac='MAC',
                               user_ports='USER_PORTS', startup_timeout='STARTUP_TIMEOUT').items():
            if getattr(args, field) is None:
                setattr(args, field, conf[key])
        args.port = int(args.port)
        if args.experimental_ds423 is None:
            args.experimental_ds423 = conf['EXPERIMENTAL_DS423'] == 'Y'
        args.smb_port = int(args.smb_port) if args.smb_port else None
        args.startup_timeout = int(args.startup_timeout)
        args.shutdown_timeout = int(conf['TIMEOUT'])
        args.serial = conf['SERIAL']  # assertion only; run never writes vendor identity
        settings.forwards(args.port, args.smb_port, args.user_ports)
        # Validate CLI overrides using the same rules as env/file configuration.
        settings.load(environ=dict(MAC=args.mac, NETWORK=args.network,
                                   STARTUP_TIMEOUT=str(args.startup_timeout)))
        if conf['RAM_CHECK'] == 'Y':
            settings.check_ram()
        if conf['SHUTDOWN'] == 'Y' and not args.shutdown_credentials:
            raise ValueError('SHUTDOWN=Y requires a protected SHUTDOWN_CREDENTIALS file')
        if args.shutdown_credentials:
            guest_control.credentials(args.shutdown_credentials)
            if args.network == 'N':
                raise ValueError('API shutdown requires networking')
        if conf['DEBUG'] == 'Y':
            args.trace_ahci = True
        return supervise(args)
    except (OSError, ValueError, lzma.LZMAError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Error: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
