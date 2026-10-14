#!/usr/bin/env python3
"""Verify local Win32 tools and pinned archives without downloading anything.

Exit 0: complete; 1: missing/mismatched required tool or bad archive; 2:
freestanding toolchain verified, archive download(s) pending. --fixtures-only
allows the explicitly authorized offline subset, while still checking any
archive present on disk. No pin is silently updated.
"""
# SPDX-License-Identifier: MIT
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def sha256(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def verify(root=ROOT, probe=True):
    pins = json.loads((root/'config/toolchain.json').read_text())['win32']
    lines, errors, pending = [], [], []
    for name, pin in pins['tools'].items():
        path = shutil.which(name)
        if not path:
            errors.append(f'{name}: missing')
            continue
        if sha256(Path(path)) != pin['sha256']:
            errors.append(f'{name}: executable SHA-256 mismatch (lead must requalify replacement)')
        version_program = shutil.which(pin['version_probe'])
        if not version_program:
            errors.append(f'{name}: missing version probe {pin["version_probe"]}')
            continue
        p = subprocess.run([version_program, '--version'], text=True, capture_output=True, timeout=10)
        versions = re.findall(r'\b\d+\.\d+\.\d+\b', p.stdout+p.stderr)
        if p.returncode or pin['version'] not in versions:
            errors.append(f'{name}: version mismatch, expected {pin["version"]}')
        else:
            lines.append(f'{name}: {pin["version"]}, pinned executable checked')
    for name, pin in pins['archives'].items():
        path = root/pin['path']
        if not path.is_file():
            pending.append(f'{name}: {pin["status"]}; {pin["url"]}; '
                           f'{pin["size"]} bytes; SHA256 {pin["sha256"]}')
        elif path.stat().st_size != pin['size'] or sha256(path) != pin['sha256']:
            errors.append(f'{name}: archive size/SHA-256 mismatch')
        else:
            lines.append(f'{name}: archive verified (not an installed CRT/toolchain)')
    if probe and not errors:
        parent = root/'build/host/win32'
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='tool-probe-', dir=parent) as tmp:
            tmp = Path(tmp)
            source = tmp/'probe.c'
            source.write_text('int entry(void) { return 0; }\n')
            for target in pins['targets']:
                obj = tmp/(target+'.obj')
                commands = [
                    ['clang', '--target='+target, '-march='+pins['march'], '-ffreestanding',
                     '-fno-stack-protector', '-c', str(source), '-o', str(obj)],
                    ['lld-link', '/machine:x86', '/nodefaultlib', '/entry:entry', '/subsystem:console',
                     '/timestamp:0', '/out:'+str(obj.with_suffix('.exe')), str(obj)]]
                for command in commands:
                    p = subprocess.run(command, text=True, capture_output=True, timeout=30)
                    if p.returncode:
                        errors.append(f'{target}: compile/link probe failed: {p.stderr.strip()}')
                        break
                else:
                    lines.append(f'{target}: freestanding COFF compile and PE32 link verified')
    return lines, errors, pending


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--fixtures-only', action='store_true')
    args = ap.parse_args()
    lines, errors, pending = verify()
    for line in lines:
        print('OK '+line)
    for line in pending:
        print('PENDING '+line)
    for line in errors:
        print('ERROR '+line)
    return 1 if errors else (2 if pending and not args.fixtures_only else 0)


if __name__ == '__main__':
    raise SystemExit(main())
