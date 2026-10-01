# Troubleshooting and recovery

Start with the launcher error and the instance's latest boot logs. Do not repair
an unknown failure by disabling validation or deleting flash. Preserve a stopped
copy before experimenting, and never start the same disk in two processes.

## The web interface does not open

The default URL is <http://127.0.0.1:15504/> on the machine running Pinology.
For a remote Linux host, forward that loopback port over SSH:

```sh
ssh -N -L 15504:127.0.0.1:15504 user@pi-host
```

Keep the SSH session open, then visit the URL on your computer. If that local
port is occupied, use `-L 15514:127.0.0.1:15504` and open port 15514 instead.
Do not solve a tunnel problem by exposing DSM publicly.

An API readiness check distinguishes starting, ready and unavailable; it does
not check all DSM services. During initial setup, use the browser wizard and
inspect boot progress. `NETWORK=N` deliberately prevents API readiness and
network-based shutdown. Check port conflicts and selected instance before
changing DNS or firewall rules.

## HVF or KVM fails

On Apple Silicon, use the native build and check its hypervisor entitlement.
A Docker guest on macOS is not the native HVF path. On Linux, check that the host
is ARM64 and `/dev/kvm` exists and is accessible to the container. KVM use must
be explicitly configured; the launcher does not substitute TCG silently.

CM4/Pi 4 require the experimental GICv2 option for this KVM path. This does not
make Pi 5 a tested target. Do not replace the host kernel or enable privileged
Docker merely to bypass an error; inspect the specific missing capability.

## Memory or disk validation fails

The guest requires 2 GiB plus emulator and host overhead. Linux checks available
RAM and cgroup-v2 limits when present. A missing memory controller means Docker
cannot enforce the requested memory limit; `0B/0B` is not zero guest usage.
macOS checks physical capacity, not a guarantee of available RAM under load.
Do not disable checks without assessing other workloads.

Initialization requires a new instance directory and a new sibling media cache.
An interrupted attempt is retained for inspection; use a new destination for a
retry rather than blindly deleting files. Hash mismatch means wrong, incomplete
or unexpected firmware. Check model and file provenance; do not change pinned
hashes merely to accept it. qcow2 backing files, physical host disks and symlink
substitution are not supported instance media.

## DSM does not return after an update

Inspect whether the flash now contains an unapproved boot profile. The launcher
intentionally refuses unknown firmware instead of booting an older cached
kernel against an updated system. A matching kernel alone is insufficient:
initrd, DTB, model and identity checks also matter.

Restore by cloning a verified **cold checkpoint to a new directory**, then
launch that clone. Keep the failed instance for diagnosis. Do not overwrite an
active instance or attach one disk to two VMs. A local copy-on-write checkpoint
is not an independent backup against host-drive failure.

## Shutdown times out

Prefer DSM's own shutdown action and wait for the launcher/container to exit.
Authenticated shutdown needs an accessible API and a private administrator
credentials file. If it fails, the launcher does not force poweroff. Docker's own
stop deadline can still cause SIGKILL, so killing the container is not a safe
replacement. Closing a terminal is not a guest shutdown either.

## Packages or sensors report errors

Check individual services rather than relying only on the login page. NPU,
physical power readings and some peripheral functions are unavailable. Serial
support only supplies a user-provided identity to the stock kernel; it does not
guarantee Photos, push notifications or cloud enrollment. Do not mask failed
services to turn a partial result into a healthy one.

## Reporting a problem

Include host OS/architecture, accelerator, model, source revision, build result,
exact command with secrets removed, and a short relevant error excerpt. State
whether this is a fresh instance or an upgrade and whether an independent copy
reproduces it. Do not upload PAT files, disk/flash images, passwords, accounts,
real serial numbers or complete logs without reviewing them. Even stock kernel
boot logs can contain the configured serial.

See [usage](usage.md) for safe stop/checkpoint commands and [architecture](architecture.md)
for the boundaries of the existing test evidence.
