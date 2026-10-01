#!/usr/bin/env bash
# Project-local patched library: never replace the Homebrew installation.
set -Eeuo pipefail
here=$(cd "$(dirname "$0")" && pwd)
lab=$(cd "$here/../.." && pwd)
work=${DSM_QEMU_WORK:-"$lab/work/macos"}
mkdir -p "$work"
work=$(cd "$work" && pwd)
patch_cmd=patch
library=static
if [[ "$(uname -s)" == Darwin ]]; then
  # macOS GLib/iconv are dynamic dependencies. A shared patched libslirp avoids
  # requesting a partly static GLib link and is copied into the Mac bundle.
  library=shared
  if ! command -v gpatch >/dev/null 2>&1; then
    echo 'GNU patch is required on macOS (brew install gpatch)' >&2
    exit 69
  fi
  patch_cmd=$(command -v gpatch)
fi
version=4.9.5
archive="$work/libslirp-v$version.tar.gz"
source_dir="$work/libslirp-v$version"
prefix="$work/libslirp-hostfwd"
meson=${MESON:-meson}
command -v "$meson" >/dev/null || { echo 'Meson is required (brew install meson on macOS)' >&2; exit 69; }
expected=f43e68b60b580647574ec4a0e2b6c600a56281e6c39f79426510832dc810f483
if [[ ! -f "$archive" ]]; then
  curl -fL --retry 2 "https://gitlab.freedesktop.org/slirp/libslirp/-/archive/v$version/libslirp-v$version.tar.gz" -o "$archive.part"
  mv "$archive.part" "$archive"
fi
actual=$(openssl dgst -sha256 "$archive" | awk '{print $NF}')
[[ "$actual" == "$expected" ]] || { echo 'libslirp source SHA-256 mismatch' >&2; exit 65; }
if [[ ! -d "$source_dir" ]]; then
  tar -xzf "$archive" -C "$work"
fi
if ! grep -q 'flags & SS_HOSTFWD) ? 128 : 1' "$source_dir/src/socket.c"; then
  "$patch_cmd" -d "$source_dir" -p1 < "$here/qemu/slirp-hostfwd-backlog.patch"
fi
build="$work/libslirp-hostfwd-build"
if [[ ! -f "$build/build.ninja" ]]; then
  "$meson" setup "$build" "$source_dir" --prefix="$prefix" \
    --libdir=lib --default-library="$library" -Dbuildtype=release
else
  "$meson" configure "$build" -Ddefault_library="$library"
fi
"$meson" compile -C "$build"
"$meson" test -C "$build" --print-errorlogs
"$meson" install -C "$build"
printf 'PKG_CONFIG_PATH=%s/lib/pkgconfig\n' "$prefix"
