# Installation

Build locally, create a new instance and complete DSM's own setup wizard.
The public CLI is `./pinology`; run commands below from the repository root.
Use test storage only. Existing instance directories are never overwritten.

## Apple Silicon macOS

Use an ARM64 terminal, Python 3.11 or newer, Xcode/Command Line Tools 16.2+
(macOS SDK 15.2 or newer) and Homebrew. Older SDKs lack the Hypervisor headers
required by the pinned QEMU; `doctor` and `build` check this before compilation.
Install build dependencies yourself; the launcher does not change system packages:

```sh
xcode-select --install
brew install python meson ninja pkg-config glib dtc qemu openssl@3 gpatch
./pinology doctor
./pinology build
```

Skip `xcode-select --install` if the tools are already installed. Homebrew QEMU
provides `qemu-img`; the guest uses the project's patched emulator, not an
unmodified Homebrew QEMU. The build downloads hash-pinned QEMU/libslirp sources,
compiles them and creates `dist/pinology-macos`. Existing output is not overwritten;
retain or move it before making another bundle. Local ad-hoc signing enables HVF;
this is not a notarized application or a published binary release.

Automatic PAT extraction also needs an installed, running Docker engine. Docker
is only a helper at this stage: Mac runtime is native HVF. To avoid the extraction
helper, supply both a matching local PAT and its previously extracted artifacts.

## Linux ARM64 and Raspberry Pi

Use a 64-bit Linux userspace/kernel, Python 3.11+, a working Docker engine and
accessible `/dev/kvm`. Allow RAM for the 2 GiB guest **plus** the emulator, Docker,
OS and existing workloads. No host kernel replacement or privileged container is
required by the documented workflow.

```sh
./pinology doctor
./pinology build
```

This builds the local `pinology:local` image from source; it does not pull a
prebuilt Pinology release. Build dependencies and the PAT extractor are obtained
inside the image build. Native package-manager installation of Docker/KVM is
host-specific and is not performed by Pinology.

The CM4 baseline uses KVM with `--experimental-gicv2`. This is also the intended
Pi 4 path, not a general promise for every board or distribution. Pi 5 has not
been verified. Check `/dev/kvm` access and memory-controller availability rather
than assuming Docker's requested limits are enforced.

## Create and install

```sh
mkdir -p instances
./pinology init instances/nas --model DS223 --disk-size 32G
./pinology start instances/nas
```

On CM4/Pi 4 use `./pinology start instances/nas --experimental-gicv2`.
Initialization downloads the pinned official DSM 7.2.2-72806 PAT, verifies its
hash, extracts the model's boot files and creates one new disk plus flash.
The sibling `instances/nas.media` retains installation media. Both directories
must be new; failed attempts remain available for inspection.

Open <http://127.0.0.1:15504/>. On a remote host use an
[SSH tunnel](troubleshooting.md). Complete the DSM wizard and choose your own
administrator credentials. If it requests an installation file, use the same
verified PAT from `instances/nas.media/boot.pat`. No shared default login is set.

To use existing verified media instead of downloading/extracting:

```sh
./pinology init instances/offline-nas --model DS223 \
  --pat /absolute/DSM_DS223_72806.pat --artifacts /absolute/extracted-pat
```

This avoids a PAT download and extraction helper; it does not make the initial
emulator build offline. An arbitrary DSM version or another model's PAT will fail
validation. Obtain and use firmware only under the applicable Synology terms.

## DS423 and identity

Create DS423 separately with `--model DS423`; launch it with
`--experimental-ds423`. On CM4/Pi 4 combine this with `--experimental-gicv2`.
Do not repurpose an installed DS223 disk. DS423 is experimental despite the
creation-time model selector.

An optional `init --serial YOUR_SERIAL` writes a user-provided identity to the
new flash. Omit it to leave identity unset; nothing is generated. The value must
be uppercase ASCII letters/digits and fit the vendor field including checksum.
It grants no license or cloud entitlement. Arguments can appear in shell history
and process listings, and the stock boot log can contain the serial. Keep the
instance, media and logs private. The boot fingerprint prevents silent identity
changes; see [usage](usage.md).
