#!/usr/bin/env python3
"""Create a new relocatable local Mac bundle; never include DSM or user disks."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parents[1]


def dependencies(path):
    lines = subprocess.check_output(['otool', '-L', str(path)], text=True).splitlines()[1:]
    return [line.strip().split(' (compatibility')[0] for line in lines]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('qemu', type=Path)
    p.add_argument('destination', type=Path)
    p.add_argument('--qemu-img', type=Path, default=Path(shutil.which('qemu-img') or 'qemu-img'))
    a = p.parse_args()
    root = a.destination.absolute()
    root.mkdir(mode=0o700)  # refuse any existing destination
    for name in ('bin', 'lib', 'runtime', 'source', 'docs'):
        (root / name).mkdir()
    originals = {}
    copied = {}
    def copy_binary(src, directory):
        src = src.resolve(strict=True)
        key = src.name
        if key in copied:
            if originals[key] != src:
                raise ValueError(f'Conflicting library basenames: {key}')
            return copied[key]
        dst = root / directory / key
        shutil.copy2(src, dst)
        originals[key], copied[key] = src, dst
        for dep in dependencies(src):
            if dep.startswith(('/System/', '/usr/lib/')) or dep == str(src):
                continue
            if not dep.startswith('/'):
                raise ValueError(f'Unresolved library reference: {dep}')
            lib = Path(dep).resolve(strict=True)
            if lib == src:  # dylib ID, not another dependency
                continue
            target = copy_binary(lib, 'lib')
            new = ('@executable_path/../lib/' if directory == 'bin' else '@loader_path/') + target.name
            subprocess.run(['install_name_tool', '-change', dep, new, str(dst)], check=True)
        if directory == 'lib':
            subprocess.run(['install_name_tool', '-id', '@loader_path/' + key, str(dst)], check=True)
        sign = ['codesign', '--force', '--sign', '-']
        if key == 'qemu-system-aarch64':
            sign += ['--entitlements', str(HERE / 'macos-hvf.entitlements')]
        subprocess.run(sign + [str(dst)], check=True)
        return dst
    copy_binary(a.qemu, 'bin')
    copy_binary(a.qemu_img, 'bin')
    for name in ('instance.py', 'boot_from_flash.py', 'lifecycle.py', 'checkpoint.py', 'instance-qmp.py',
                 'settings.py', 'profiles.py', 'guest_control.py', 'prepare_media.py',
                 'flash-media.py', 'vendor_identity.py', 'storage.py', 'lease.py', 'menu.py', 'settings.env.example'):
        shutil.copy2(HERE / name, root / 'runtime' / name)
    source = root / 'source' / 'kernel' / 'stock-board'
    source.mkdir(parents=True)
    for name in ('build-qemu.sh', 'build-slirp.sh', 'macos-hvf.entitlements'):
        shutil.copy2(HERE / name, source / name)
    shutil.copytree(HERE / 'qemu', source / 'qemu')
    for document in HERE.glob('*.md'):
        shutil.copy2(document, root / 'docs' / document.name)
    if (PROJECT / 'docs').is_dir():
        shutil.copytree(PROJECT / 'docs', root / 'docs', dirs_exist_ok=True)
    for name in ('LICENSE', 'NOTICE.md', 'CONTRIBUTING.md', 'SECURITY.md'):
        if (PROJECT / name).is_file():
            shutil.copy2(PROJECT / name, root / name)
    if (PROJECT / 'LICENSES').is_dir():
        shutil.copytree(PROJECT / 'LICENSES', root / 'LICENSES')
    shutil.copy2(HERE / 'DSM.command', root / 'DSM.command')
    (root / 'DSM.command').chmod(0o755)
    launcher = root / 'dsm'
    launcher.write_text('''#!/bin/bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
python_bin=
for candidate in "${DSM_PYTHON:-}" /opt/homebrew/bin/python3 /usr/local/bin/python3 "$(command -v python3 || true)"; do
  if [[ -n "$candidate" && -x "$candidate" ]] && "$candidate" -c 'import sys; sys.exit(sys.version_info < (3, 11))' 2>/dev/null; then
    python_bin=$candidate; break
  fi
done
if [[ -z "$python_bin" ]]; then echo 'Install Python 3.11+ for Apple Silicon (or set DSM_PYTHON).' >&2; exit 69; fi
export PATH="$(dirname "$python_bin"):/opt/homebrew/bin:/usr/local/bin:$PATH"
config_file=
if [[ "${1:-}" == --config ]]; then config_file=${2:?Config file required}; shift 2; fi
action=${1:-menu}
if [[ $# -gt 0 ]]; then
shift
fi
case "$action" in
  menu) exec "$python_bin" "$here/runtime/menu.py" --bundle "$here" "$@" ;;
  run) if [[ -z "$config_file" ]]; then export ACCEL=${ACCEL:-hvf}; fi; set -- run "$@" --qemu "$here/bin/qemu-system-aarch64" ;;
  init|prepare|config|health|stop|grow) set -- "$action" "$@" ;;
  checkpoint) exec "$python_bin" "$here/runtime/checkpoint.py" "$@" --qemu-img "$here/bin/qemu-img" ;;
  *) echo 'Use menu, init, prepare, run, config, health, stop, grow or checkpoint' >&2; exit 64 ;;
esac
if [[ -n "$config_file" ]]; then set -- --config "$config_file" "$@"; fi
exec "$python_bin" "$here/runtime/instance.py" --qemu-img "$here/bin/qemu-img" "$@"
''')
    launcher.chmod(0o755)
    hashes = {str(f.relative_to(root)): hashlib.sha256(f.read_bytes()).hexdigest()
              for f in root.rglob('*') if f.is_file()}
    (root / 'SHA256.json').write_text(json.dumps(hashes, indent=2) + '\n')
    print(json.dumps({'bundle': str(root), 'files': len(hashes), 'libraries': len(copied) - 2,
                      'requires': 'Apple Silicon macOS and Python >=3.11; local ad-hoc signature, not notarized'}))


if __name__ == '__main__':
    main()
