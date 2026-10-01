#!/usr/bin/env python3
"""Diagnostics for the named isolated 72806 instance; not a generic QMP client."""
import argparse
import datetime
import json
import os
from pathlib import Path
import socket
from guest_control import vm_name

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("instance", type=Path)
parser.add_argument("action", choices=("status", "dump-memory", "power-cut", "serial-attach"))
args = parser.parse_args()
root = args.instance.resolve(strict=True)
socket_name = 'status.sock' if (root / 'status.sock').exists() else 'control.sock'
if (root / socket_name).is_symlink():
    parser.error("Refusing symlink socket")
os.chdir(root)  # Short relative socket path avoids sockaddr_un length limits.
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.settimeout(60)
sock.connect(socket_name)
stream = sock.makefile("rwb", buffering=0)
json.loads(stream.readline())


def call(name, arguments=None):
    request = {"execute": name}
    if arguments is not None:
        request["arguments"] = arguments
    stream.write(json.dumps(request).encode() + b"\n")
    while True:
        response = json.loads(stream.readline())
        if "event" in response:
            continue
        if "error" in response:
            raise RuntimeError(response["error"])
        return response["return"]


call("qmp_capabilities")
if call("query-name").get("name") != vm_name(root):
    parser.error("Wrong VM identity")
if args.action == "status":
    print(json.dumps({"status": call("query-status"), "disks": call("query-block")}, indent=2))
elif args.action == "dump-memory":
    # Only for the disposable installer before credentials/user data exist.
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = root / f"installer-memory-{stamp}-{os.getpid()}.bin"
    with output.open("xb"):
        pass
    running = call("query-status")["running"]
    call("stop")
    try:
        call("pmemsave", {"val": 0x40000, "size": 0x7ffc0000, "filename": str(output)})
    finally:
        if running:
            call("cont")
    print(output)
elif args.action == "serial-attach":
    ports = {port["label"]: port for port in call("query-chardev")}
    port = ports.get("serial0")
    if not port:
        parser.error("Expected serial0 console is absent")
    if port["filename"] not in ("stdio", "ringbuf"):
        parser.error("Refusing to replace an already interactive console")
    if (root / "console.sock").exists() or (root / "console.sock").is_symlink():
        parser.error("Refusing to replace an existing console socket")
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = root / "logs" / f"{stamp}-{os.getpid()}.serial.log"
    with output.open("xb"):
        pass
    call("chardev-change", {"id": "serial0", "backend": {
        "type": "socket", "data": {"addr": {"type": "unix", "data": {
            "path": "console.sock"}}, "server": True, "wait": False,
            "logfile": str(output), "logappend": True}}})
    (root / "console.sock").chmod(0o600)
    print(f"UART output now goes to {output}; earlier console log is preserved")
    print("Connect locally from the instance directory: nc -U console.sock")
else:
    print("Forced power-off of disposable lab guest, not a graceful shutdown", flush=True)
    call("quit")
