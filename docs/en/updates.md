# Updating DSM and Pinology

[Русский](../ru/updates.md) · [Documentation](../README.md)

DSM can update through its own web interface, including downloading an update
over the network. Pinology and its emulator are updated separately on the host.
This remains an experimental system: **make a cold checkpoint before either
kind of update**, and keep an independent backup of important files.

## Which DSM versions can boot?

The source at Pinology v0.1.0 allows these model-specific boot payloads:

| DSM release | DS223 | DS423 |
| --- | --- | --- |
| 7.2.2-72806 | Initial installation profile | Initial installation profile; experimental |
| 7.2.2-72806 Update 9 | Recognized flash payload | Recognized flash payload; experimental |
| 7.4.1-90080 | Recognized boot profile | Recognized boot profile; experimental |

Update 9 carries the flash payload from Update 4, so the launcher's internal
profile is named `7.2.2-72806-flash4`. This is **not** a promise that every Update
number is tested. Boot hashes do not identify all installed userspace/packages;
check the actual version in DSM after updating.

The authoritative lists are [installation profiles](../../kernel/stock-board/profiles.py)
and [flash boot profiles](../../kernel/stock-board/boot_from_flash.py). DS223 and
DS423 have separate ramdisk/DTB hashes even where their kernel matches. A newer
version appearing in DSM is not evidence of Pinology compatibility. Recognition
of a boot profile is not certification of all services, packages or host hardware.

## Before updating

1. Check your model, current DSM version and the exact target build, including
   its Update suffix. Read the release notes and any required intermediate steps.
   Every intermediate boot must also be supported; do not force a rejected PAT
   or bypass an unsupported intermediate release.
2. Confirm the runtime you actually launch contains the required boot profile.
   Updating this Git checkout alone does not update an existing Mac bundle or
   Docker image. See [Updating Pinology](#updating-pinology) below when needed.
3. In **Control Panel → Update & Restore → DSM Update → Update Settings**, choose
   notification/manual installation rather than unattended installation. This
   Pinology precaution differs from the usual recommendation for a supported NAS;
   do not leave an outdated experimental guest exposed to the internet.
4. Shut down **inside DSM**, then wait for the launcher/container to exit. Create
   a checkpoint using a new destination; its parent directory must already exist:

```sh
./pinology backup instances/nas instances/nas-before-update
./pinology start instances/nas
```

For DS423 add `--experimental-ds423` to every `start` command in this guide.
On CM4/Pi 4 add `--experimental-gicv2`; use both flags when applicable. Reuse your
custom HTTP/SMB port options too. Keep the original checkpoint stopped and intact.
A DSM configuration export alone does not replace this disk-and-flash checkpoint.

## Apply the update in DSM

1. Open DSM at <http://127.0.0.1:5000/> (or your configured port/SSH tunnel).
   Go to **Control Panel → Update & Restore → DSM Update**.
2. If the offered version is the verified target, download it and follow the
   installation prompts. If it is not offered, use **Manual DSM Update** with an
   official PAT for the **same model and exact supported target version**.
   The original `nas.media/boot.pat` is the initial installation image, not
   automatically the correct update file. Review and accept any license terms
   yourself. See [Synology's update instructions](https://kb.synology.com/index.php/en-us/DSM/help/DSM/AdminCenter/system_dsmupdate?version=7).
3. Leave the launcher, Docker engine (on Linux) and host running. Do not close
   the terminal, stop the container or interrupt power while DSM installs/reboots.
   Browser disconnection during reboot is expected; it does not prove completion.

Pinology reads the updated persistent flash on restart and validates kernel,
ramdisk and DTB. A confirmed guest reset is supervised automatically, subject to
the [reboot limit](usage.md#network-and-lifecycle). If the launcher exits or reports
an unverified profile, inspect its output and instance logs; do not repeatedly
restart or disable the checks. There is no need to run `init` again or manually
replace boot files during a supported DSM update.

## Verify the result

After DSM is reachable, run `./pinology status instances/nas`, then check inside
DSM: the target version, storage pool/volume health, shared files, SMB access,
network connectivity and the packages you use. A running QEMU or HTTP 200 alone
does not establish success. Allow package migrations/indexing to finish and
investigate reported failures. Keep the pre-update checkpoint until these checks
and a subsequent normal reboot succeed.

The source manifest may still show the original installation build. It is not an
inventory of the current installed DSM version: do not edit `instance.json` just
to make its build label match DSM.

## If the update fails

Keep the failed instance and its logs for diagnosis. Confirm it is no longer
running before starting a recovery clone; do not reuse its disk or ports while
it remains active. If shutdown is stuck, follow [troubleshooting](troubleshooting.md)
rather than treating a forced container stop as a clean shutdown.

Restore the complete **pre-update** checkpoint into another new directory:

```sh
./pinology backup instances/nas-before-update instances/nas-restored
./pinology start instances/nas-restored
```

This restores the old disk, flash and manifest together. Changes made after the
checkpoint are not included. Do not try to downgrade only the kernel/PAT, mix
old flash with the updated disk, overwrite the failed instance, or change hashes
to accept unknown firmware. Keep a separate backup on independent storage.

## Updating Pinology

This updates the launcher/emulator, **not DSM**. With DSM stopped and checkpointed,
keep the previous checkout and built runtime, then obtain the desired Pinology
release in a separate directory. Read its release notes and run `./pinology doctor`
and `./pinology build` there. On Mac this creates a new `dist/pinology-macos` without
overwriting the previous checkout's bundle. On Linux, preserve the existing image
before building because the build reuses the `pinology:local` tag:

```sh
# Linux only; choose a backup tag that is not already in use:
docker image tag pinology:local pinology:before-update
```

Use the new checkout's CLI with the **absolute path to the existing instance**;
do not initialize or convert it. Test a cold clone first when possible, with the
original stopped. To revert the Linux runtime, first stop the guest and retag
`pinology:before-update` as `pinology:local`; use the previous checkout too. On Mac,
use the previous checkout and bundle. If the new runtime or DSM changed instance
state incompatibly, also restore the pre-update checkpoint rather than assuming
that switching executables reverses those changes.
