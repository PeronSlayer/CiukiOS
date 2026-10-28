#!/usr/bin/env python3
"""Sealed SYSTEM LOGON and LOGON_UI images (sec-user-03):
/System/Services/logon and /System/Services/logon-ui (security_role_image).

PID 1 starts LOGON_UI then LOGON from its fixed manifest; with LOGON present
the interim uid-1000 desktop is no longer started and every session is
created by LOGON. The images are installed only together with an account
service that can publish accounts (manifest owner_backend other than the
interim refusing backend): without it nobody could log on, so the interim
desktop stays (see image_payloads)."""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/apps/logon'
SDK = ROOT / 'build/tools/ciuki-sdk'
LOGON_PATH = '/System/Services/logon'
UI_PATH = '/System/Services/logon-ui'
LOGON_SOURCES = ('apps/logon/main.c', 'apps/logon/sys_ciuki.c', 'apps/logon/client_ciuki.c', 'apps/logon/coordinator.c',
                 'apps/logon/links.c', 'apps/logon/logon_proto.c',
                 # the frozen relay/correlation core and codecs of the account service
                 'apps/accounts/logon.c', 'apps/accounts/auth.c', 'apps/accounts/protocol.c', 'apps/accounts/codec.c',
                 'apps/accounts/crypto.c', 'src/kernel/lib/sha256.c')
UI_SOURCES = ('apps/logon/ui_main.c', 'apps/logon/ui.c', 'apps/logon/logon_proto.c', 'apps/logon/client_ciuki.c',
              'apps/desktop/theme.c')
LIBDESKUI = ROOT / 'build/apps/desktop/libdeskui.a'
CFLAGS = ['-O2', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections']
INCLUDES = ['-I', str(ROOT / 'apps/logon'), '-I', str(ROOT / 'apps/accounts'), '-I', str(ROOT / 'apps/desktop'),
            '-I', str(ROOT / 'sdk/private'), '-idirafter' + str(ROOT / 'src/kernel/include')]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def inputs():
    paths = [p for p in (ROOT / 'apps/logon').iterdir() if p.is_file() and p.suffix in ('.c', '.h', '.py')]
    paths += [ROOT / s for s in (*LOGON_SOURCES, *UI_SOURCES)]
    paths += [ROOT / 'apps/accounts/accounts.h', ROOT / 'apps/accounts/adapter.h', ROOT / 'apps/accounts/wire.h',
              ROOT / 'apps/desktop/theme.h', ROOT / 'apps/desktop/ui_fonts.h', ROOT / 'src/kernel/include/ciuki/abi.h',
              ROOT / 'src/kernel/include/ciuki/sha256.h', SDK / 'manifest.json', LIBDESKUI]
    return {str(p.relative_to(ROOT)): digest(p) for p in sorted(set(paths))}


def validate_payloads():
    module('logon_sdk_validator', ROOT / 'apps/lua/build_lua.py').validate_sdk()
    record = json.loads((OUT / 'manifest.json').read_text())
    if record['inputs'] != inputs():
        raise ValueError('stale logon payload; run make apps')
    checker = module('logon_elf_check', ROOT / 'sdk/tests/check_sdk.py')
    for name, expected in record['files'].items():
        if digest(OUT / name) != expected:
            raise ValueError('logon payload changed: ' + name)
        if checker.inspect(OUT / name) != record['elf'][name]:
            raise ValueError('logon ELF evidence mismatch: ' + name)
    return record


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    try:
        record = validate_payloads()
        print('[logon] inputs and output hashes unchanged')
        return record
    except (ValueError, OSError, KeyError):
        pass
    if not LIBDESKUI.is_file():
        raise RuntimeError('LOGON_UI needs build/apps/desktop/libdeskui.a; build the desktop first')
    expected = inputs()
    commands = {
        'logon': [str(SDK / 'bin/ciuki-cc'), *CFLAGS, *INCLUDES, *(str(ROOT / s) for s in LOGON_SOURCES),
                  '-Wl,--gc-sections', '-o', str(OUT / 'logon')],
        'logon-ui': [str(SDK / 'bin/ciuki-cc'), *CFLAGS, *INCLUDES, *(str(ROOT / s) for s in UI_SOURCES),
                     '-Wl,--gc-sections', str(LIBDESKUI), '-o', str(OUT / 'logon-ui')],
    }
    for command in commands.values():
        subprocess.run(['nice', '-n', '19', *command], check=True, cwd=ROOT)
    checker = module('logon_elf_build', ROOT / 'sdk/tests/check_sdk.py')
    record = {'inputs': expected, 'files': {n: digest(OUT / n) for n in commands},
              'elf': {n: checker.inspect(OUT / n) for n in commands},
              'image_paths': {'logon': LOGON_PATH, 'logon-ui': UI_PATH}, 'commands': commands,
              'guest_qualification': 'not_run'}
    (OUT / 'manifest.json').write_text(json.dumps(record, indent=2, sort_keys=True) + '\n')
    return record


def install_gate(accounts_metadata):
    """LOGON replaces the interim desktop only beside an account service whose
    OWNER backend can publish accounts; the interim refusing backend answers
    every request with EIO and nobody could log on."""
    return accounts_metadata.get('owner_backend') not in (None, 'interim-refusing')


def image_payloads(accounts_metadata):
    record = validate_payloads()
    if not install_gate(accounts_metadata):
        return {}, set(), {'installed': False, 'reason': 'account service owner backend ' +
                           str(accounts_metadata.get('owner_backend')), 'manifest_sha256': digest(OUT / 'manifest.json')}
    sources = {LOGON_PATH: OUT / 'logon', UI_PATH: OUT / 'logon-ui'}
    return sources, {'/System', '/System/Services'}, {'installed': True, 'manifest_sha256': digest(OUT / 'manifest.json'),
                                                       'elf': record['elf'], 'image_paths': record['image_paths']}


if __name__ == '__main__':
    build()
