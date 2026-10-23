#!/usr/bin/env python3
"""Compile libsetup using an existing SDK, without invoking its writable wrapper."""
# SPDX-License-Identifier: GPL-2.0-only
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--sdk', type=Path, required=True)
ap.add_argument('--link-setup', action='store_true', help='also compile/link the production wizard using read-only SDK libraries')
ap.add_argument('--out', type=Path, default=Path('build/host/fs/libsetup/target'))
a = ap.parse_args()
sdk = a.sdk.resolve()
m = json.loads((sdk / 'manifest.json').read_text())
cc = m['tools']['clang']['path']
assert hashlib.sha256(Path(cc).read_bytes()).hexdigest() == m['tools']['clang']['sha256'], 'pinned clang changed'
resource = subprocess.check_output([cc, '-print-resource-dir'], text=True).strip()
a.out.mkdir(parents=True, exist_ok=True)
flags = [cc, *m['flags'], '--sysroot=' + str(sdk / 'sysroot'), '-nostdinc',
         '-isystem', str(sdk / 'sysroot/include'), '-isystem', resource + '/include',
         '-Isdk/sysroot-overlay/include', '-Isrc/kernel/include', '-Isdk/libdisk/include', '-Isdk/libsetup/include',
         '-DCIUKI_CONFIG_USER', '-O2', '-Wall', '-Wextra', '-Werror']
units = ('txn', 'manifest', 'native', 'native_media')
for unit in units:
    subprocess.run([*flags, '-c', f'sdk/libsetup/{unit}.c', '-o', str(a.out / f'{unit}.o')], check=True)
for source in ('apps/control/configuration.c','src/kernel/net/config.c','apps/desktop/settings_io.c'):
    obj = a.out / (Path(source).stem + '.o')
    subprocess.run([*flags, '-c', source, '-o', str(obj)], check=True)
subprocess.run(['ar', 'rcs', str(a.out / 'libsetup.a'), *(str(a.out / f'{u}.o') for u in (*units, 'configuration', 'config', 'settings_io'))], check=True)
nm = subprocess.check_output(['nm', '-g', '--defined-only', str(a.out / 'libsetup.a')], text=True)
seen = set()
for line in nm.splitlines():
    fields = line.split()
    if len(fields) == 3:
        symbol = fields[2]
        assert symbol not in seen, f'duplicate external symbol: {symbol}'
        seen.add(symbol)
print(f'PASS i686 libsetup.a: {len(units)+3} production units, pinned SDK flags, no duplicate external symbols')

if a.link_setup:
    import importlib.util
    import runpy
    root = Path.cwd()
    disk_objects = []
    boot = a.out / 'libdisk_boot.c'
    subprocess.run(['python3','sdk/libdisk/gen_boot.py','--out',str(boot)],check=True)
    for source in (*sorted(Path('sdk/libdisk').glob('*.c')),boot):
        obj=a.out / ('disk_'+source.stem+'.o')
        subprocess.run([*flags,'-c',str(source),'-o',str(obj)],check=True)
        disk_objects.append(obj)
    disk_library=a.out / 'libdisk.a'
    subprocess.run(['ar','rcs',str(disk_library),*map(str,disk_objects)],check=True)
    recipe=runpy.run_path('apps/setup/build_setup.py')
    objects=[]
    for index,source in enumerate(recipe['SOURCES']):
        obj=a.out / ('setup_'+str(index)+'.o')
        subprocess.run([*flags,'-c',source,'-o',str(obj)],check=True)
        objects.append(obj)
    deskui=sdk.parents[1] / 'apps/desktop/libdeskui.a'
    libraries=(deskui,a.out/'libsetup.a',disk_library,sdk/'sysroot/lib/libreg.a')
    globals_seen={}
    for library in libraries:
        symbols=subprocess.check_output(['nm','-g','--defined-only',str(library)],text=True)
        for line in symbols.splitlines():
            fields=line.split()
            if len(fields)==3:
                name=fields[2]
                assert name not in globals_seen, f'duplicate external symbol {name}: {globals_seen.get(name)} / {library}'
                globals_seen[name]=str(library)
    output=a.out/'setup'
    lib=sdk/'sysroot/lib'
    subprocess.run([m['tools']['ld.lld']['path'],'-m','elf_i386','-static','--no-undefined','--gc-sections',
                    '-z','max-page-size=4096','-L',str(lib),'-T',str(lib/'ciuki.ld'),'-Map',str(output)+'.map',
                    '-o',str(output),str(lib/'crt0.o'),*map(str,objects),'--start-group',
                    '-lpthread','-lciuki','-lc','-lm','-lcompiler',*map(str,libraries),'--end-group'],check=True)
    spec=importlib.util.spec_from_file_location('setup_target_checker','sdk/tests/check_sdk.py')
    checker=importlib.util.module_from_spec(spec);spec.loader.exec_module(checker)
    checker.SDK=sdk
    checker.inspect(output)
    print('PASS production Setup ELF: native adapters, real archives, no undefined or duplicate library symbols')
