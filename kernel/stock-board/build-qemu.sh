#!/usr/bin/env bash
set -Eeuo pipefail

here=$(cd "$(dirname "$0")" && pwd)
lab=$(cd "$here/../.." && pwd)
work=${DSM_QEMU_WORK:-"$lab/work/macos"}
mkdir -p "$work"
work=$(cd "$work" && pwd)
patch_cmd=patch
if [[ "$(uname -s)" == Darwin ]]; then
  if ! command -v gpatch >/dev/null 2>&1; then
    echo 'GNU patch is required on macOS (brew install gpatch)' >&2
    exit 69
  fi
  patch_cmd=$(command -v gpatch)
fi
version=11.1.1
archive="$work/qemu-$version.tar.xz"
source_dir="$work/qemu-$version"
build_dir=${DSM_QEMU_BUILD:-"$work/qemu-ds223-build"}
expected=079ffbff8a7111bbc89022107cbabf3bbfd614d5fc9d7cc675991196aca12482

if [[ ! -f "$archive" ]]; then
  curl -fL --retry 2 "https://download.qemu.org/qemu-$version.tar.xz" -o "$archive.part"
  mv "$archive.part" "$archive"
fi
actual=$(openssl dgst -sha256 "$archive" | awk '{print $NF}')
[[ "$actual" == "$expected" ]] || { echo 'QEMU source SHA-256 mismatch' >&2; exit 65; }
if [[ ! -d "$source_dir" ]]; then
  tar -xJf "$archive" -C "$work"
fi
[[ "$(< "$source_dir/VERSION")" == "$version" ]] || exit 65

if ! grep -q 'DS223-NCQ: a fresh queue' "$source_dir/hw/ide/ahci.c"; then
  "$patch_cmd" -d "$source_dir" -p1 < "$here/qemu/integration-ahci-ncq.patch"
fi
if ! grep -q 'AHCI num-ports must' "$source_dir/hw/ide/ich.c"; then
  "$patch_cmd" -d "$source_dir" -p1 < "$here/qemu/integration-pci-ahci-ports.patch"
fi

if ! grep -q '^config DS223$' "$source_dir/hw/arm/Kconfig"; then
  "$patch_cmd" -d "$source_dir" -p1 < "$here/qemu/integration.patch"
fi
if ! grep -q "rtd1619b-crg.c" "$source_dir/hw/arm/meson.build"; then
  "$patch_cmd" -d "$source_dir" -p1 < "$here/qemu/integration-crg.patch"
fi
if ! grep -q "rtd1619b-gpio.c" "$source_dir/hw/arm/meson.build"; then
  "$patch_cmd" -d "$source_dir" -p1 < "$here/qemu/integration-gpio.patch"
fi
if ! grep -q "rtd1619b-otp.c" "$source_dir/hw/arm/meson.build"; then
  "$patch_cmd" -d "$source_dir" -p1 < "$here/qemu/integration-otp.patch"
fi
# Copy our maintained board source into the disposable upstream build tree.
if ! grep -q "rtd1619b-sb2.c" "$source_dir/hw/arm/meson.build"; then
  "$patch_cmd" -d "$source_dir" -p1 < "$here/qemu/integration-sb2.patch"
fi
cp "$here/qemu/rtd1619b-sb2.c" "$source_dir/hw/arm/rtd1619b-sb2.c"
if ! grep -q "rtd1619b-mcp.c" "$source_dir/hw/arm/meson.build"; then
  "$patch_cmd" -d "$source_dir" -p1 < "$here/qemu/integration-mcp.patch"
fi
cp "$here/qemu/rtd1619b-mcp.c" "$source_dir/hw/arm/rtd1619b-mcp.c"
if ! grep -q "rtd1619b-sata.c" "$source_dir/hw/arm/meson.build"; then
  "$patch_cmd" -d "$source_dir" -p1 < "$here/qemu/integration-sata.patch"
fi
cp "$here/qemu/rtd1619b-sata.c" "$source_dir/hw/arm/rtd1619b-sata.c"
if ! grep -q "rtd1619b-net.c" "$source_dir/hw/arm/meson.build"; then
  "$patch_cmd" -d "$source_dir" -p1 < "$here/qemu/integration-net.patch"
fi
cp "$here/qemu/rtd1619b-net.c" "$source_dir/hw/arm/rtd1619b-net.c"
if ! grep -q "../net/net_tx_pkt.c" "$source_dir/hw/arm/meson.build"; then
  "$patch_cmd" -d "$source_dir" -p1 < "$here/qemu/integration-net-dma.patch"
fi
if ! grep -q "rtd1619b-usb.c" "$source_dir/hw/arm/meson.build"; then
  "$patch_cmd" -d "$source_dir" -p1 < "$here/qemu/integration-usb.patch"
fi
cp "$here/qemu/rtd1619b-usb.c" "$source_dir/hw/arm/rtd1619b-usb.c"
if ! grep -q "rtd1619b-sfc.c" "$source_dir/hw/arm/meson.build"; then
  "$patch_cmd" -d "$source_dir" -p1 < "$here/qemu/integration-sfc.patch"
fi
cp "$here/qemu/rtd1619b-sfc.c" "$source_dir/hw/arm/rtd1619b-sfc.c"
if ! grep -q 'rtd1619b-pcie.c' "$source_dir/hw/arm/meson.build"; then
  "$patch_cmd" -d "$source_dir" -p1 < "$here/qemu/integration-pcie.patch"
fi
cp "$here/qemu/rtd1619b-pcie.c" "$source_dir/hw/arm/rtd1619b-pcie.c"
cp "$here/qemu/ds223.c" "$source_dir/hw/arm/ds223.c"
cp "$here/qemu/rtd1619b-crg.c" "$source_dir/hw/arm/rtd1619b-crg.c"
cp "$here/qemu/rtd1619b-gpio.c" "$source_dir/hw/arm/rtd1619b-gpio.c"
cp "$here/qemu/rtd1619b-otp.c" "$source_dir/hw/arm/rtd1619b-otp.c"
cp "$here/qemu/ds223.mak" "$source_dir/configs/devices/aarch64-softmmu/ds223.mak"
mkdir -p "$build_dir"
cd "$build_dir"

if [[ ! -f build.ninja || "${DSM_QEMU_RECONFIGURE:-0}" == 1 ]]; then
  accel=()
  case "$(uname -s):$(uname -m)" in
    Darwin:arm64) accel=(--enable-hvf) ;;
    Linux:aarch64|Linux:arm64) accel=(--enable-kvm) ;;
  esac
  network=()
  if [[ "${DSM_QEMU_SLIRP:-0}" == 1 ]]; then
    network=(--enable-slirp)
  fi
  "$source_dir/configure" --target-list=aarch64-softmmu \
    --with-devices-aarch64=ds223 --without-default-devices \
    --without-default-features --enable-tcg --enable-fdt --disable-werror \
    ${accel[@]+"${accel[@]}"} ${network[@]+"${network[@]}"}
fi
ninja -j "${DSM_BUILD_JOBS:-6}" qemu-system-aarch64
printf 'QEMU=%s/qemu-system-aarch64\n' "$build_dir"
