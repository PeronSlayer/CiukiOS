#!/usr/bin/env python3
"""Sealed SYSTEM account service image (sec-int-01): /System/Services/accounts."""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/apps/accounts'
SDK = ROOT / 'build/tools/ciuki-sdk'
# The kernel's fixed role image (security_role_image, sec-k01) and the
# SYSTEM-only metadata directory the service owns (K-Q1).
IMAGE_PATH = '/System/Services/accounts'
STORE_DIRECTORY = '/System/Security'
SERVICE_SOURCES = ('crypto.c', 'codec.c', 'owner.c', 'bootstrap.c', 'protocol.c', 'store.c', 'service.c', 'migration.c',
                   'persist.c', 'results.c', 'auth.c', 'enrollment.c', 'manifest.c', 'checkpoint.c', 'logon.c',
                   'firstboot.c')  # sec-user-02 first-boot adapter
PRODUCTION_SOURCES = ('kernel_adapter.c', 'server.c', 'kernel_sys_ciuki.c', 'owner_interim.c', 'storage_posix.c', 'main.c',
                      'firstboot_posix.c')
SHARED_SOURCES = ('apps/desktop/users.c', 'src/kernel/lib/sha256.c', 'sdk/private/accounts_link.c')
CFLAGS = ['-O2', '-Wall', '-Wextra', '-Werror']


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def inputs():
    paths = [p for p in (ROOT / 'apps/accounts').iterdir() if p.is_file() and p.suffix in ('.c', '.h', '.py')]
    paths += [ROOT / 'sdk/private/accounts_link.c', ROOT / 'sdk/private/accounts_link.h', ROOT / 'apps/desktop/users.c',
              ROOT / 'apps/desktop/users.h', ROOT / 'src/kernel/lib/sha256.c', ROOT / 'src/kernel/include/ciuki/abi.h',
              ROOT / 'src/kernel/include/ciuki/sha256.h', SDK / 'manifest.json']
    return {str(p.relative_to(ROOT)): digest(p) for p in sorted(set(paths))}


def validate_payloads():
    module('accounts_sdk_validator', ROOT / 'apps/lua/build_lua.py').validate_sdk()
    record = json.loads((OUT / 'manifest.json').read_text())
    if record['inputs'] != inputs():
        raise ValueError('stale account service payload; run make apps')
    for name, expected in record['files'].items():
        if digest(OUT / name) != expected:
            raise ValueError('account service payload changed: ' + name)
    actual = module('accounts_elf_check', ROOT / 'sdk/tests/check_sdk.py').inspect(OUT / 'accounts')
    if actual != record['elf']:
        raise ValueError('account service ELF evidence mismatch')
    return record


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    expected = inputs()
    try:
        record = validate_payloads()
        print('[accounts] inputs and output hashes unchanged')
        return record
    except (ValueError, OSError, KeyError):
        pass
    sources = [str(ROOT / 'apps/accounts' / s) for s in (*SERVICE_SOURCES, *PRODUCTION_SOURCES)]
    sources += [str(ROOT / s) for s in SHARED_SOURCES]
    command = [str(SDK / 'bin/ciuki-cc'), *CFLAGS, '-ffunction-sections', '-fdata-sections',
               '-I', str(ROOT / 'apps/accounts'), '-I', str(ROOT / 'sdk/private'), '-I', str(ROOT / 'apps/desktop'),
               # users.c includes the kernel's ciuki/sha256.h; the SDK headers win (joined: ciuki-cc forwards "-" arguments).
               '-idirafter' + str(ROOT / 'src/kernel/include'),
               *sources, '-o', str(OUT / 'accounts')]
    subprocess.run(command, check=True, cwd=ROOT)
    evidence = module('accounts_elf_build', ROOT / 'sdk/tests/check_sdk.py').inspect(OUT / 'accounts')
    record = {'inputs': expected, 'files': {'accounts': digest(OUT / 'accounts')}, 'elf': evidence,
              'image_path': IMAGE_PATH, 'store_directory': STORE_DIRECTORY, 'owner_backend': 'interim-refusing',
              'command': command, 'guest_qualification': 'not_run'}
    (OUT / 'manifest.json').write_text(json.dumps(record, indent=2, sort_keys=True) + '\n')
    return record


def image_payloads():
    record = validate_payloads()
    sources = {IMAGE_PATH: OUT / 'accounts'}
    directories = {'/System', '/System/Services', STORE_DIRECTORY, STORE_DIRECTORY + '/Pictures'}
    return sources, directories, {'manifest_sha256': digest(OUT / 'manifest.json'), 'elf': record['elf'],
                                  'image_path': IMAGE_PATH, 'owner_backend': record['owner_backend']}


if __name__ == '__main__':
    build()
