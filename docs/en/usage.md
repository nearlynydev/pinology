# Running and configuration

The public `./pinology` interface chooses native HVF on Apple Silicon and Docker
with KVM on Linux ARM64. It intentionally exposes a smaller, conservative set of
options than the development runtime. Use `./pinology COMMAND --help` for syntax.

## Commands

| Command | Purpose |
| --- | --- |
| `doctor` | Check basic host prerequisites; not a complete build or VM test |
| `build` | Compile a local Mac bundle or Linux Docker image |
| `init PATH` | Create new pinned media, flash and a virtual disk |
| `start PATH` | Run in the foreground; HTTP/SMB on loopback by default |
| `status PATH` | Inspect the instance; not a comprehensive DSM service audit |
| `stop PATH --credentials FILE` | Request authenticated DSM shutdown and wait |
| `backup PATH NEW_DESTINATION` | Create a verified cold checkpoint; source must be stopped |
| `menu` | Native Mac terminal menu; requires a built bundle |

`init` accepts `--model DS223|DS423`, `--disk-size 32G`, optional `--serial`,
`--pat` and `--artifacts`. Disk sizes use integer M/G/T units (binary sizes),
from 8G through 16T. The public path creates a sparse qcow2; choosing a larger
size does not reserve that much physical space and does not grow an existing
disk. Monitor the host's actual free space.

`start` accepts `--port` (default 5000), `--smb-port` (445), `--https-port` (5001),
`--network local|host`, `--bind-address`,
`--experimental-ds423` and `--experimental-gicv2`. Host ports must be distinct,
unused and in 1..65535; host low-port permissions still apply. GICv2 is a Linux-only opt-in; DS423 needs its own flag
regardless of host.

```sh
./pinology start instances/nas --port 5000 --smb-port 445
# In another terminal:
./pinology status instances/nas
```

On Mac, status reports QMP VM state. On Linux, it runs the readiness probe in the
verified running container's network namespace. A running VM is not necessarily
API-ready; an API-ready DSM is not necessarily healthy in every package.

## Network and lifecycle

Networking uses user-mode NAT. By default HTTP and SMB forwards listen on host
127.0.0.1. For LAN access through the host IP, explicitly select `--network host`
(defaults to `0.0.0.0`); `--bind-address` is an optional restriction.
See [network access](networking.md). There is no
automatic bridge, dedicated LAN IP or discovery
broadcast forwarding. The Docker guest's internal wildcard binding is confined
to its private network. Do not change the container to host networking.

Create a shared folder inside DSM before using `smb://127.0.0.1:445/SHARE`.
Use your own DSM credentials. In local mode, a remote host needs SSH forwards for each service:

```sh
ssh -N -L 5000:127.0.0.1:5000 -L 14445:127.0.0.1:445 user@pi-host
```

This tunnel exposes SMB at `smb://127.0.0.1:14445/SHARE` on the client.

The launcher enables persistent flash, a local serial socket and bounded reboot
supervision. Only a confirmed guest reset causes a restart; poweroff, crashes or
an unknown shutdown reason leave it stopped. Three restarts within five minutes
are allowed; another reset stops the loop. Each restart revalidates flash.
No host autostart or crash-restart policy is installed.

## Safe shutdown

The simplest method is **Shut down in DSM**, followed by waiting for the launcher
or container to exit. Closing the terminal, Ctrl-C, killing QEMU and `docker stop`
without configured guest shutdown are not equivalent to that action.

For the public stop command, create a private JSON file in an editor with exactly
two fields, `account` and `password`, containing your DSM administrator credentials.
Do not commit it. Protect it and pass only its path:

```sh
chmod 600 /absolute/private-shutdown.json
./pinology stop instances/nas --credentials /absolute/private-shutdown.json
```

The command authenticates locally and requests stock DSM poweroff. It waits for
the VM/supervisor locks to be released and does not force poweroff on failure.
If authentication, networking or shutdown fails, inspect the result and use DSM
to finish shutdown. Docker's own forced-stop deadline remains a separate risk.

## Checkpoint and restore

After shutdown, use a destination that does not exist and whose parent does:

```sh
./pinology backup instances/nas instances/nas-checkpoint
# Restore to a NEW instance; never overwrite the old one:
./pinology backup instances/nas-checkpoint instances/nas-restored
./pinology start instances/nas-restored
```

A checkpoint includes the disk, flash, manifest and original boot files. The
tool rejects a running source, checks qcow2 integrity and verifies copied bytes.
It does not include runtime logs. Copy-on-write clones protect against guest
mistakes, not host disk failure; keep a separate backup on independent media.

Before an update, checkpoint the stopped system, then start it again. Only use
an update whose resulting model-specific boot profile is approved by the current
source. Do not enable unattended arbitrary firmware updates. Follow the
[update guide](updates.md) for the complete procedure, version list and rollback.

## Identity and advanced settings

An optional serial is set only during `init`. New manifests pin a fingerprint of
the vendor identity, including its absence. A later unexpected change stops boot;
the launcher does not generate or restore a serial automatically. Back up flash
and manifest together. Serial support does not prove cloud-service entitlement.

The public CLI **clears inherited development-runtime settings**. It does not
accept `--config`, automatically read `settings.env`, or pass through settings
such as `ACCEL`, `SERIAL`, `DISK_FMT`, `MAC` and `USER_PORTS`. Do not assume those
environment variables change `./pinology start`.

Advanced developers can inspect the separate lower-level
[runtime example](../../kernel/stock-board/settings.env.example) and
[configuration parser](../../kernel/stock-board/settings.py), which support
additional disk/network options. Using that interface means taking responsibility
for accelerator, mount, port and shutdown configuration; it is not an additional
public CLI contract. CPU/RAM remain fixed at four vCPUs and 2 GiB.
