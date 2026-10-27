#!/usr/bin/env python3
"""Sealed SYSTEM registry broker (sec-k06): /System/Services/registry."""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/apps/registry'
SDK = ROOT / 'build/tools/ciuki-sdk'
# The kernel's fixed role image (security_role_image, sec-k01) and the
# SYSTEM-only metadata directory the service owns (K-Q1).
IMAGE_PATH = '/System/Services/registry'
STORE_DIRECTORY = '/System/Security'
SERVICE_SOURCES = ('broker.c','storage.c','accounts.c','kernel_adapter.c','server.c','restart.c','restart_store.c','restart_quarantine.c','restart_posix.c','main.c')
SHARED_SOURCES = ('apps/w32srv/registry.c','apps/w32srv/storage.c','apps/w32srv/defaults.c',
                  'apps/w32srv/directory.c','apps/w32srv/posix_io.c',
                  'apps/accounts/kernel_sys_ciuki.c','src/kernel/lib/sha256.c')
CFLAGS = ['-O2', '-Wall', '-Wextra', '-Werror']


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def inputs():
    paths = [p for p in (ROOT / 'apps/registry').iterdir() if p.is_file() and p.suffix in ('.c','.h','.py')]
    paths += [ROOT/p for p in SHARED_SOURCES]
    paths += [ROOT/'apps/w32srv/registry.h',ROOT/'apps/accounts/kernel_sys.h',
              ROOT/'sdk/private/registry_protocol.h',ROOT/'sdk/libreg/transport.h',
              ROOT/'src/kernel/include/ciuki/abi.h',SDK/'manifest.json']
    return {str(p.relative_to(ROOT)): digest(p) for p in sorted(set(paths))}


def validate_payloads():
    module('registry_sdk_validator', ROOT / 'apps/lua/build_lua.py').validate_sdk()
    record = json.loads((OUT / 'manifest.json').read_text())
    if record['inputs'] != inputs():
        raise ValueError('stale registry broker payload; run make apps')
    for name, expected in record['files'].items():
        if digest(OUT / name) != expected:
            raise ValueError('registry broker payload changed: ' + name)
    actual = module('registry_elf_check', ROOT / 'sdk/tests/check_sdk.py').inspect(OUT / 'registry')
    if actual != record['elf']:
        raise ValueError('registry broker ELF evidence mismatch')
    return record


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    expected = inputs()
    try:
        record = validate_payloads()
        print('[registry] inputs and output hashes unchanged')
        return record
    except (ValueError, OSError, KeyError):
        pass
    sources = [str(ROOT / 'apps/registry' / s) for s in SERVICE_SOURCES]
    sources += [str(ROOT / s) for s in SHARED_SOURCES]
    command = [str(SDK / 'bin/ciuki-cc'), *CFLAGS, '-ffunction-sections', '-fdata-sections',
               '-I'+str(ROOT/'sdk/win32/include'),
               # SDK headers precede kernel-only declarations (including sha256.h).
               '-idirafter' + str(ROOT / 'src/kernel/include'),
               *sources, '-lreg', '-o', str(OUT / 'registry')]
    subprocess.run(command, check=True, cwd=ROOT)
    evidence = module('registry_elf_build', ROOT / 'sdk/tests/check_sdk.py').inspect(OUT / 'registry')
    record = {'inputs': expected, 'files': {'registry': digest(OUT / 'registry')}, 'elf': evidence,
              'image_path': IMAGE_PATH, 'store_directory': STORE_DIRECTORY,
              'command': command, 'guest_qualification': 'not_run'}
    (OUT / 'manifest.json').write_text(json.dumps(record, indent=2, sort_keys=True) + '\n')
    return record


def image_payloads():
    record = validate_payloads()
    sources = {IMAGE_PATH: OUT / 'registry'}
    directories = {'/System', '/System/Services', STORE_DIRECTORY, STORE_DIRECTORY + '/Registry', STORE_DIRECTORY + '/Registry/Machine', STORE_DIRECTORY + '/Restart', STORE_DIRECTORY + '/Restart/Legacy'}
    return sources, directories, {'manifest_sha256': digest(OUT / 'manifest.json'), 'elf': record['elf'],
                                  'image_path': IMAGE_PATH, 'retained_peers': True}


if __name__ == '__main__':
    build()
