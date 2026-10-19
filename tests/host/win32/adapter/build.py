#!/usr/bin/env python3
"""Build the shared freestanding core for Linux i386 and both pinned PE targets."""
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright 2026 CiukiOS contributors.
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[4]
CORE = ROOT / 'sdk/win32'
ADAPTER = Path(__file__).resolve().parent

def run(args):
    subprocess.run(list(map(str,args)),check=True,cwd=ROOT,timeout=60)

def build(out, main=None, windows=True):
    out=Path(out);out.mkdir(parents=True,exist_ok=True)
    sources=sorted((CORE/'ldr').glob('*.c'))+sorted((CORE/'ntdll').glob('*.c'))+[CORE/'ntdll/context.S',CORE/'services/ciuki_stub.c']
    common=['-march=pentiumpro','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-omit-frame-pointer',
            '-mno-sse','-mno-sse2','-mno-mmx','-fno-ident','-O1','-Wall','-Wextra','-Werror','-I'+str(CORE/'include'),'-I'+str(ROOT/'src/kernel/include')]
    targets=['i386-unknown-linux-gnu']
    if windows: targets+=['i686-pc-windows-msvc','i686-w64-mingw32']
    for target in targets:
        directory=out/target;directory.mkdir(exist_ok=True)
        objects=[]
        for source in sources:
            obj=directory/(source.stem+'.o')
            run(['clang','--target='+target,*common,'-fno-pic','-fno-pie','-c',source,'-o',obj]);objects.append(obj)
        run(['llvm-ar','rcs',directory/'libw32core.a',*objects])
        if target.endswith('linux-gnu'):
            for source in [ADAPTER/'linux.c',ADAPTER/'start.S',main or ADAPTER/'main.c']:
                obj=directory/(Path(source).stem+'.o')
                run(['clang','--target='+target,*common,'-fno-pic','-fno-pie','-I'+str(ADAPTER),'-c',source,'-o',obj]);objects.append(obj)
            run(['ld.lld','-m','elf_i386','-static','--entry=_start','--image-base=0x60000000','-o',out/'w32host',*objects])
        else:
            exports=CORE/'ntdll/ntdll.def'
            run(['llvm-rc','/fo',directory/'version.res',CORE/'ntdll/version.rc'])
            run(['lld-link','/machine:x86','/dll','/noentry','/nodefaultlib','/safeseh:no','/base:0x70000000',
                 '/subsystem:windows,4.0','/osversion:4.0','/timestamp:0','/incremental:no',
                 '/def:'+str(exports),'/out:'+str(directory/'ntdll.dll'),*objects,directory/'version.res'])
            raw=(directory/'ntdll.dll').read_bytes();offset=int.from_bytes(raw[60:64],'little')
            manifest={'schema':1,'dlls':[{'name':'ntdll.dll','path':'ntdll.dll','base':0x70000000,
                'size':int.from_bytes(raw[offset+80:offset+84],'little'),
                'sha256':hashlib.sha256(raw).hexdigest(),'version':[0,0,0,0]}]}
            (directory/'builtin-store.json').write_text(json.dumps(manifest,sort_keys=True,indent=2)+'\n')
    return out/'w32host'

SHIM_BASE=0x70800000

def build_shim(directory):
    """H1 test-only kernel32 subset over the PE ntdll (never a builtin of an image)."""
    directory=Path(directory)
    run(['clang','--target=i686-pc-windows-msvc','-march=pentiumpro','-ffreestanding','-fno-builtin',
         '-fno-stack-protector','-mno-sse','-mno-sse2','-mno-mmx','-O1','-Wall','-Wextra','-Werror',
         '-c',ADAPTER/'kernel32_shim.c','-o',directory/'kernel32_shim.obj'])
    run(['llvm-dlltool','-m','i386','-k','-d',ADAPTER/'kernel32_shim_imports.def','-l',directory/'shim-ntdll.lib'])
    run(['lld-link','/machine:x86','/dll','/noentry','/nodefaultlib','/safeseh:no','/fixed',
         '/base:'+hex(SHIM_BASE),'/subsystem:windows,4.0','/timestamp:0','/incremental:no',
         '/def:'+str(ADAPTER/'kernel32_shim.def'),'/out:'+str(directory/'kernel32.dll'),
         directory/'kernel32_shim.obj',directory/'shim-ntdll.lib'])
    return directory/'kernel32.dll'

def build_model(out,main=None):
    out=Path(out);out.mkdir(parents=True,exist_ok=True)
    sources=sorted((CORE/'ldr').glob('*.c'))+sorted((CORE/'ntdll').glob('*.c'))
    binary=out/'w32model'
    run(['clang','-DW32_HOST_MODEL','-ffreestanding','-fno-builtin','-fno-omit-frame-pointer',
         '-O1','-Wall','-Wextra','-Werror','-pthread','-I'+str(CORE/'include'),'-I'+str(ADAPTER),
         *sources,ADAPTER/'model.c',main or ADAPTER/'main.c','-o',binary])
    return binary

if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output',type=Path,default=ROOT/'build/host/win32/core')
    ap.add_argument('--linux-only',action='store_true')
    ap.add_argument('--main',type=Path)
    ap.add_argument('--model',action='store_true',help='H0 native model; never PE execution or ABI evidence')
    args=ap.parse_args()
    try: print(build_model(args.output,args.main) if args.model else build(args.output,args.main,not args.linux_only))
    except subprocess.CalledProcessError as e: sys.exit(e.returncode)
