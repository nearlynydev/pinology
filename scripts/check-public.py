#!/usr/bin/env python3
"""Guard the tracked public tree; complements (does not replace) manual review."""
from pathlib import Path
import re
import subprocess
import sys
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[1]
BLOCKED_DIRS = {'work', 'dist', 'data', 'instances', 'artifacts', '__pycache__', '.venv'}
BLOCKED_SUFFIXES = {'.pat', '.qcow2', '.raw', '.img', '.bin', '.dtb', '.spk', '.iso', '.log', '.pem', '.key', '.pyc', '.zip', '.txz'}
PATTERNS = [
    rb'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----',
    rb'\bgh[pousr]_[A-Za-z0-9]{30,}\b',
    rb'\bgithub_pat_[A-Za-z0-9_]{30,}\b',
    rb'\bAKIA[0-9A-Z]{16}\b',
    rb'/(?:Users|Volumes)/[A-Za-z0-9]',
    rb'\b192\.168\.[0-9]+\.[0-9]+\b',
]


def main():
    files = subprocess.check_output(['git', 'ls-files', '-z'], cwd=ROOT).decode().split('\0')
    files = [name for name in files if name]
    if not files:
        raise SystemExit('No tracked files: stage the reviewed source tree first')
    errors = []
    for name in files:
        path = ROOT / name
        if path.is_symlink() or set(path.relative_to(ROOT).parts) & BLOCKED_DIRS or path.suffix in BLOCKED_SUFFIXES:
            errors.append(f'Forbidden public path: {name}')
            continue
        data = path.read_bytes()
        if len(data) > 2 * 1024**2 or b'\0' in data:
            errors.append(f'Unexpected large/binary file: {name}')
        # This scanner contains its own pattern strings, not actual credentials.
        if any(re.search(pattern, data) for pattern in PATTERNS):
            errors.append(f'Possible sensitive content: {name}')
        if path.suffix == '.md':
            for target in re.findall(r'\[[^\]\n]*\]\(([^)\s]+)\)', data.decode()):
                if re.match(r'^[a-zA-Z][a-zA-Z0-9+.-]*:', target) or target.startswith('#'):
                    continue
                target = unquote(target.split('#', 1)[0])
                if target and not (path.parent / target).exists():
                    errors.append(f'Broken local documentation link: {name}: {target}')
    for error in errors:
        print(error, file=sys.stderr)
    print(f'Public-tree guard: {len(files)} tracked files, {len(errors)} findings')
    return bool(errors)


if __name__ == '__main__':
    raise SystemExit(main())
