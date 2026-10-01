# Architecture and compatibility

Pinology runs stock ARM DSM with a custom QEMU model of the parts of the Realtek
RTD1619B platform needed by the guest. It is not Synology Virtual DSM for x86, a
DSM container sharing the host kernel, or a complete hardware replica.

## Execution layers

The launcher validates an instance manifest, original boot artifacts and the
current emulated flash. QEMU then boots the approved stock ARM kernel and initrd
directly, using the model's device tree and emulated peripherals. U-Boot is kept
in flash for the stock layout but is not the executing bootloader.

On Apple Silicon, native QEMU uses HVF. On Linux ARM64, QEMU can use KVM inside
Docker; the container packages the emulator, not a replacement host kernel.
TCG is a slower software-emulation alternative. Accelerator failure does not
silently fall back to TCG. Docker on macOS does not give this guest native HVF.

The guest has four vCPUs, 2 GiB RAM and one virtual data disk. CPU virtualization
does not accelerate all emulated devices. Host CPU quotas and memory limits are
separate from the guest's configuration.

## Model selection

Choose DS223 or DS423 when creating an instance. Each profile selects its own
official PAT, initrd, device tree, U-Boot and SHA-256 allowlist. An installed model
cannot be converted by editing its name or attaching its disk to another model.

DS223 uses the integrated SATA controller. DS423 uses a modeled PCIe host and
PCI AHCI path; it remains explicitly experimental. Its optional second PCI NIC
is a separate prototype, not a promise of physical-network equivalence. The
default workflow attaches one disk even for the four-bay DS423 profile.

## Firmware and updates

Initial installation is pinned to DSM 7.2.2-72806. The repository includes approved
boot profiles, including tested 7.4.1-90080 profiles, rather than accepting every
firmware version. Before each restart the launcher extracts the current flash
and validates the payloads. Unknown firmware stops the launch instead of falling
back to an old kernel. A successful past update is not a guarantee for future
releases: take a cold checkpoint first and inspect the current allowlist.

DSM/PAT files are obtained separately from Synology and are not redistributed
here. The project's source license does not grant a DSM license or entitlement
to Synology services. Pinology is not affiliated with Synology.

## Validation boundaries

The earlier development baseline was exercised on native Apple Silicon HVF and
on a CM4 with KVM and the experimental GICv2 path. Tests included HTTP, checksum-
verified SMB transfers, Btrfs checks and clean-reboot persistence. These results
do not certify every package, indefinite uptime, all host distributions or every
new build of this public launcher. Raspberry Pi 5 is not a verified target.

The CM4 run did not test crash durability. Photos had a service failure associated
with missing serial identity; later serial-reader tests do not certify Photos,
Synology Account, QuickConnect or push notifications. API readiness alone says
nothing about the health of every package.

NPU acceleration is unavailable: neither Realtek NPU commands nor Apple Neural
Engine offload is implemented. A physical current sensor is unavailable; no
fabricated Mac or board power-consumption value is returned. USB, PMIC and other
peripheral behavior is incomplete. Use [troubleshooting](troubleshooting.md) to
separate these limitations from new failures.
