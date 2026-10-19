#!/usr/bin/env python3
"""Build the freestanding CiukiOS core and census placeholders, never Wine binaries."""
# SPDX-License-Identifier: LGPL-2.1-or-later
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path: sys.path.insert(0,str(ROOT))
from scripts.win32 import stubs, kernel32, gdi32

def build(out=None):
    out=Path(out or ROOT/'build/apps/win32');out.mkdir(parents=True,exist_ok=True)
    core=ROOT/'sdk/win32'
    flags=['--target=i686-pc-windows-msvc','-march=pentiumpro','-ffreestanding','-fno-builtin',
           '-fno-stack-protector','-fno-omit-frame-pointer','-mno-sse','-mno-sse2','-mno-mmx',
           '-O1','-Wall','-Wextra','-Werror','-I'+str(core/'include'),'-I'+str(ROOT/'src/kernel/include')]
    objects=[]
    sources=sorted((core/'ldr').glob('*.c'))+sorted((core/'ntdll').glob('*.c'))+[
        core/'ntdll/context.S',core/'services/ciuki.c',core/'services/start.c']
    for source in sources:
        obj=out/(source.parent.name+'-'+source.stem+'.obj')
        subprocess.run(['clang',*flags,'-c',str(source),'-o',str(obj)],check=True);objects.append(str(obj))
    subprocess.run(['llvm-rc','/fo',str(out/'version.res'),str(core/'ntdll/version.rc')],check=True)
    subprocess.run(['lld-link','/machine:x86','/dll','/entry:CiukiDllMain@12','/nodefaultlib','/safeseh:no',
        '/base:0x70000000','/subsystem:windows,4.0','/osversion:4.0','/timestamp:0','/incremental:no',
        '/def:'+str(core/'ntdll/ntdll.def'),'/export:CiukiProcessStart=_CiukiProcessStart@4',
        '/out:'+str(out/'ntdll.dll'),*objects,str(out/'version.res')],check=True)
    raw=(out/'ntdll.dll').read_bytes();pe=struct.unpack_from('<I',raw,60)[0]
    record={'name':'ntdll.dll','path':'ntdll.dll','base':0x70000000,
        'size':struct.unpack_from('<I',raw,pe+80)[0],'sha256':hashlib.sha256(raw).hexdigest(),
        'version':stubs.image_version(raw)}
    # Keep core implementations authoritative if the census later adds ntdll.
    generated=out/'generated'
    stubs.generate([ROOT/'docs/design/research/win32-layer-2026-10-14.md'],generated)
    store=stubs.build(generated,out/'ntdll.lib')
    import shutil
    for dll in store['dlls']:shutil.copyfile(generated/dll['path'],out/dll['path'])
    kernel_base=max(d['base']+((d['size']+65535)&~65535) for d in store['dlls'])
    for i,dll in enumerate(store['dlls']):
        if dll['name']=='kernel32.dll':store['dlls'][i]=kernel32.build(out,kernel_base,flags)
        if dll['name']=='wsock32.dll':store['dlls'][i]=kernel32.build_offline(out,dll['base'],flags)
    gdi_base=max(d['base']+((d['size']+65535)&~65535) for d in store['dlls'])
    for i,dll in enumerate(store['dlls']):
        if dll['name']=='gdi32.dll':store['dlls'][i]=gdi32.build(out,gdi_base)
    store['dlls'].insert(0,record)
    store['msvcrt_policy']='builtin placeholder pending f5-14B measured replacement'
    manifest=out/'builtin-store.json';manifest.write_text(json.dumps(store,sort_keys=True,indent=2)+'\n')
    stubs.verify_store(manifest)
    binary=struct.pack('<II',1,len(store['dlls']))
    for dll in store['dlls']:
        binary+=struct.pack('<64s65s3xII4H',dll['name'].encode(),dll['sha256'].encode(),dll['base'],dll['size'],*dll['version'])
    (out/'BUILTIN.BIN').write_bytes(binary)
    from scripts.win32 import fixtures
    fixture_source=ROOT/'build/host/win32/image-fixtures'
    hashes=fixtures.build(fixture_source)
    (out/'fixtures').mkdir(exist_ok=True)
    for name in hashes:shutil.copyfile(fixture_source/name,out/'fixtures'/name)
    (out/'fixtures.json').write_text(json.dumps({'schema':1,'files':hashes},sort_keys=True,indent=2)+'\n')
    return manifest

if __name__=='__main__': print(build())
