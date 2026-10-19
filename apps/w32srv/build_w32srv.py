#!/usr/bin/env python3
"""Native service, deterministic default hives and f5-07A PE payloads."""
# SPDX-License-Identifier: LGPL-2.1-or-later
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT/'build/apps/w32srv'
SOURCES = ('registry.c','storage.c','directory.c','defaults.c','protocol.c','posix_io.c','main.c')

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def inputs():
    paths=[p for d in (ROOT/'apps/w32srv',ROOT/'sdk/win32') for p in d.rglob('*') if p.is_file() and '__pycache__' not in p.parts]
    paths += [ROOT/'scripts/win32/facades.py',ROOT/'scripts/win32/stubs.py',ROOT/'scripts/win32/stub_signatures.py',ROOT/'scripts/win32/census.py',
              ROOT/'docs/design/research/win32-layer-2026-10-14.md', ROOT/'sdk/libreg/casefold.h',ROOT/'build/tools/ciuki-sdk/manifest.json',
              ROOT/'src/kernel/include/ciuki/abi.h']
    return {str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(paths)}

def validate_payloads():
    record=json.loads((OUT/'manifest.json').read_text())
    if record['inputs']!=inputs():raise ValueError('stale w32srv payloads; run make apps')
    for name,digest in record['files'].items():
        if hashlib.sha256((OUT/name).read_bytes()).hexdigest()!=digest:raise ValueError('w32srv payload changed: '+name)
    module('w32srv_store',ROOT/'scripts/win32/stubs.py').verify_store(OUT/'builtin/builtin-store.json')
    return record

def build():
    OUT.mkdir(parents=True,exist_ok=True)
    try:return validate_payloads()
    except (ValueError,OSError,KeyError):pass
    sdk=ROOT/'build/tools/ciuki-sdk'
    subprocess.run([str(sdk/'bin/ciuki-cc'),'-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(ROOT/'sdk/win32/include'),*(str(ROOT/'apps/w32srv'/s) for s in SOURCES),'-o',str(OUT/'w32srv')],check=True)
    checker=module('w32srv_elf',ROOT/'sdk/tests/check_sdk.py')
    evidence=checker.inspect(OUT/'w32srv')
    host=OUT/'seed-hives'
    subprocess.run(['clang','-std=c17','-O2','-Wall','-Wextra','-Werror','-I'+str(ROOT/'sdk/win32/include'),
                    *(str(ROOT/'apps/w32srv'/s) for s in SOURCES),'-o',str(host)],check=True)
    defaults=OUT/'defaults';defaults.mkdir(exist_ok=True)
    # A seed is a reproducible fresh database, never a previous build's state.
    for p in defaults.iterdir():p.unlink()
    subprocess.run([str(host),'--seed',str(defaults)],check=True)
    facades=module('w32srv_facades',ROOT/'scripts/win32/facades.py').build(OUT/'builtin')
    payloads=['w32srv',*(str(p.relative_to(OUT)) for p in sorted(defaults.iterdir())),
              *(f'builtin/{d["path"]}' for d in facades['dlls']),'builtin/builtin-store.json','builtin/coverage.json']
    record={'inputs':inputs(),'files':{n:hashlib.sha256((OUT/n).read_bytes()).hexdigest() for n in payloads},'elf':evidence,'guest_qualification':'not_run'}
    (OUT/'manifest.json').write_text(json.dumps(record,indent=2,sort_keys=True)+'\n')
    return record

def image_payloads():
    record=validate_payloads()
    sources={'/System/Win32/w32srv':OUT/'w32srv'}
    for name in record['files']:
        if name.startswith('defaults/'):sources['/WINDOWS/'+Path(name).name]=OUT/name
        elif name.startswith('builtin/'):sources['/System/Win32/builtin/'+Path(name).name]=OUT/name
    sources['/System/Win32/LICENSE.LIB']=ROOT/'sdk/win32/lz32/mspack/COPYING.LIB'
    sources['/System/Win32/MSPACK.TXT']=ROOT/'sdk/win32/lz32/NOTICE.md'
    return sources,{'/WINDOWS','/System/Win32','/System/Win32/builtin'},record

if __name__=='__main__':build()
