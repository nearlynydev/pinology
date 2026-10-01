# Pinology

[Русский](docs/ru/README.md) · [Documentation](docs/README.md)

Run ARM DSM on Apple Silicon or Linux ARM64 using a custom QEMU board model.
Pinology provides verified firmware profiles, a virtual disk, native Mac
acceleration and a Docker delivery path for Linux.

**Experimental research software, not a production NAS.** It is not affiliated
with Synology and does not include DSM, a DSM license or cloud-service entitlement.
Use disposable data and keep independent backups.

## What works and what is experimental

| Host or feature | Status |
| --- | --- |
| Apple Silicon macOS | Native QEMU with HVF; recommended Mac path |
| Linux ARM64 | Docker-packaged QEMU with explicit KVM access |
| CM4 / Raspberry Pi 4 | Experimental GICv2 KVM path; CM4 development baseline tested |
| Raspberry Pi 5 | Not verified |
| DS223 | Default model profile |
| DS423 | Selectable at creation; launch requires experimental opt-in |
| CPU / RAM / disk | Four vCPUs, 2 GiB RAM, one virtual disk; no host-disk passthrough |
| NPU / physical power sensor | Unavailable |

The development baseline includes real Mac HVF and CM4 KVM boots, checksum-verified
SMB transfers and clean-reboot checks. That is not certification of every package
or every build of this public CLI. See [compatibility](docs/en/architecture.md).

## Quick start

This is a **source-only project**. Install the [host prerequisites](docs/en/installation.md)
first, then run from the repository root:

```sh
git clone https://github.com/nearlynydev/pinology.git
cd pinology
./pinology doctor
./pinology build
mkdir -p instances
./pinology init instances/nas --model DS223 --disk-size 32G
./pinology start instances/nas
```

On a CM4/Pi 4 Linux host, use `./pinology start instances/nas --experimental-gicv2`
instead of the last command. Start stays in the foreground. Open
<http://127.0.0.1:15504/> on the host, or use an [SSH tunnel](docs/en/troubleshooting.md).
Complete DSM's installation wizard; initialization alone does not create an
installed NAS or an administrator account.

`init` obtains the model-specific, SHA-256-pinned DSM 7.2.2-72806 PAT from Synology.
On Mac, automatic PAT extraction uses a pinned Docker helper, while the guest
itself runs natively with HVF. You can instead provide your own matching PAT and
already-extracted artifacts. No firmware or prebuilt emulator is distributed here.

For DS423, use `--model DS423` with a **new** instance and
`--experimental-ds423` on `start`. Model selection is not an in-place conversion.

## Operate safely

Power off from DSM and wait for the launcher to exit before copying or moving
the instance. Then create a cold checkpoint:

```sh
./pinology backup instances/nas instances/nas-checkpoint
```

Never mount one instance into two VMs. Do not publish the test system to the
internet. Host listeners default to loopback, and no host autostart is installed.
DSM updates are restricted by verified boot profiles, not guaranteed for arbitrary
future releases. Follow the [DSM and Pinology update guide](docs/en/updates.md)
for supported versions, online/manual installation, checks and rollback.

Read [installation](docs/en/installation.md), [usage](docs/en/usage.md),
[architecture](docs/en/architecture.md) and [troubleshooting](docs/en/troubleshooting.md).
Development and review practices, including AI assistance, are described in
[CONTRIBUTING.md](CONTRIBUTING.md). See [LICENSE](LICENSE) and
[NOTICE.md](NOTICE.md) for source and dependency licensing.
