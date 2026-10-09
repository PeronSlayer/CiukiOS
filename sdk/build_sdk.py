#!/usr/bin/env python3
"""Build a pinned, offline CiukiOS SDK; never execute target configure probes."""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tarfile
import time
import sys
sys.dont_write_bytecode=True

ROOT=Path(__file__).resolve().parent.parent
OUT=ROOT/'build/tools/ciuki-sdk'
ABI=ROOT/'src/kernel/include/ciuki/abi.h'

def digest(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def run(cmd, **kw): return subprocess.run([str(x) for x in cmd],check=True,**kw)
def flags():
    spec=importlib.util.spec_from_file_location('kernel_build',ROOT/'scripts/build_kernel.py')
    m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    return [x for x in m.CFLAGS if x not in ('-mno-80387',)]+['-ffunction-sections','-fdata-sections','-D__CIUKIOS__=1','-D_DEFAULT_SOURCE=1','-D__DYNAMIC_REENT__',f'-ffile-prefix-map={ROOT}=.',f'-fdebug-prefix-map={ROOT}=.','-fdebug-compilation-dir=.']

def generate(inc,lib,work):
    abi=ABI.read_text(); shutil.copy2(ABI,inc/'ciuki/abi.h')
    calls=re.findall(r'CIUKI_SYS_(\w+)\s*=\s*(\d+)',abi)
    defs=['/* Generated from ciuki/abi.h. */']
    for n,v in calls: defs.append(f'#define CIUKI_SYS_{n} {v}')
    for name in ('CIUKI_IMAGE_BASE','CIUKI_PAGE_SIZE'):
        value=re.search(r'^#define '+name+r' (\S+)',abi,re.M)[1]
        (lib/'abi.ld').open('a').write(f'{name} = {value};\n')
    (work/'abi-asm.h').write_text('\n'.join(defs)+'\n')
    asm=['#include "abi-asm.h"','.text']
    hdr=['#ifndef CIUKI_RAW_H','#define CIUKI_RAW_H','#include <stdint.h>']
    for name,value in calls:
        if name.startswith('RESERVED'): continue
        symbol='ciuki_raw_'+name.lower()
        hdr.append(f'uint32_t {symbol}(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);')
        asm+= [f'.section .text.{symbol},"ax",@progbits',f'.globl {symbol}',f'.type {symbol},@function',symbol+':',
            'pushl %ebp','pushl %edi','pushl %esi','pushl %ebx',f'movl $CIUKI_SYS_{name},%eax']
        for reg,off in zip(('ebx','ecx','edx','esi','edi','ebp'),range(20,44,4)): asm.append(f'movl {off}(%esp),%{reg}')
        asm+=['int $0x80','popl %ebx','popl %esi','popl %edi','popl %ebp','ret',f'.size {symbol},.-{symbol}']
    asm+=['.section .text.ciuki_syscall,"ax",@progbits','.globl ciuki_syscall','.type ciuki_syscall,@function','ciuki_syscall:',
        'pushl %ebp','pushl %edi','pushl %esi','pushl %ebx','movl 20(%esp),%eax']
    for reg,off in zip(('ebx','ecx','edx','esi','edi','ebp'),range(24,48,4)): asm.append(f'movl {off}(%esp),%{reg}')
    asm+=['int $0x80','popl %ebx','popl %esi','popl %edi','popl %ebp','ret','.size ciuki_syscall,.-ciuki_syscall',
        '.section .text.__ciuki_sigreturn,"ax",@progbits','.globl __ciuki_sigreturn','.type __ciuki_sigreturn,@function','__ciuki_sigreturn:',
        'leal -4(%esp),%ebx','movl $CIUKI_SYS_SIGRETURN,%eax','int $0x80','ud2','.size __ciuki_sigreturn,.-__ciuki_sigreturn',
        '.section .note.GNU-stack,"",@progbits']
    (work/'syscalls.S').write_text('\n'.join(asm)+'\n'); (inc/'ciuki/raw.h').write_text('\n'.join(hdr+['#endif'])+'\n')

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--archive',type=Path,default=ROOT/'build/downloads/newlib/newlib-4.5.0.20241231.tar.gz')
    ap.add_argument('--jobs',type=int,choices=(1,2),default=2)
    args=ap.parse_args(); start=time.monotonic()
    pins=json.loads((ROOT/'config/sdk-pins.json').read_text()); source=pins['newlib']
    if not args.archive.is_file() or digest(args.archive)!=source['sha256']:
        ap.error('missing newlib archive or SHA-256 mismatch; no download/fallback is permitted')
    if not re.fullmatch('[0-9a-f]{40}',source['commit']): ap.error('upstream commit must be immutable')
    patch_paths=sorted((ROOT/'sdk/newlib/patches').glob('*.patch'))
    hashes={p.name:digest(p) for p in patch_paths}
    if hashes!=source['patches']: ap.error('unrecorded or changed newlib patch')
    tool_pins=json.loads((ROOT/'config/toolchain.json').read_text()); tool_info={}
    for tool in ('clang','ld.lld','nasm','llvm-ar','llvm-ranlib','llvm-readelf','llvm-objdump','llvm-size'):
        path=Path(shutil.which(tool) or tool).absolute()
        version=run([path,'-v' if tool=='nasm' else '--version'],capture_output=True,text=True).stdout.strip()
        major=re.search(r'(?:version |LLD |LLVM version )(\d+)',version)
        expected=tool_pins['nasm_major' if tool=='nasm' else 'ld_lld_major' if tool=='ld.lld' else 'clang_major']
        if not major or int(major[1])!=expected: ap.error(f'{tool} does not match pinned major {expected}')
        if version!=pins['tools'][tool]['version'] or digest(path)!=pins['tools'][tool]['sha256']:
            ap.error(f'{tool} differs from config/sdk-pins.json; lead must qualify an updated pin')
        tool_info[tool]={'path':str(path),'version':version,'sha256':digest(path)}
    if OUT.exists(): shutil.rmtree(OUT)
    work=OUT/'work'; work.mkdir(parents=True)
    tmp=work/'temp'; tmp.mkdir(); os.environ['TMPDIR']=str(tmp)
    inc=OUT/'sysroot/include'; lib=OUT/'sysroot/lib'; lib.mkdir(parents=True); inc.mkdir(parents=True)
    with tarfile.open(args.archive) as archive: archive.extractall(work,filter='data')
    src=work/source['directory']; build=work/'newlib-build'; build.mkdir()
    env=os.environ.copy(); env['TMPDIR']=str(tmp); env['CIUKI_NEWLIB_PORT']='yes'
    env['SOURCE_DATE_EPOCH']=source['source_date_epoch'].__str__()
    env.pop('CPATH',None); env.pop('C_INCLUDE_PATH',None); env.pop('LIBRARY_PATH',None)
    for patch in patch_paths: run(['patch','--batch','--fuzz=0','-p1','-i',patch],cwd=src,stdout=subprocess.DEVNULL)
    shutil.copytree(ROOT/'sdk/sysroot-overlay/include',src/'newlib/libc/include',dirs_exist_ok=True)
    shutil.copy2(ABI,src/'newlib/libc/include/ciuki/abi.h')
    cflags=flags()
    cc=work/'target-cc'; cc.write_text('#!/bin/sh\nexec '+shlex.join([tool_info['clang']['path'],*cflags,'-fuse-ld=lld','-nostdinc','-isystem',str(Path(run([tool_info['clang']['path'],'-print-resource-dir'],capture_output=True,text=True).stdout.strip())/'include'),'-isystem',str(src/'newlib/libc/include'),'-Wno-error','-Wno-deprecated-non-prototype','-Dasm=__asm__'])+' "$@"\n'); cc.chmod(0o755)
    env.update(CC=str(cc),CC_FOR_TARGET=str(cc),AR=tool_info['llvm-ar']['path'],AR_FOR_TARGET=tool_info['llvm-ar']['path'],RANLIB=tool_info['llvm-ranlib']['path'],RANLIB_FOR_TARGET=tool_info['llvm-ranlib']['path'],AS=str(cc),AS_FOR_TARGET=str(cc),LD=tool_info['ld.lld']['path'],LD_FOR_TARGET=tool_info['ld.lld']['path'],CFLAGS='-O2 -g')
    configure=json.loads((ROOT/'sdk/newlib/recipe.json').read_text())['configure']+[f'--prefix={OUT}/work/install']
    log=OUT/'build.log'
    with log.open('w') as output:
        for cmd in ([src/'newlib/configure',*configure],['make',f'-j{args.jobs}','V=1'],['make','install']):
            print('[sdk]',cmd[0],flush=True); run(cmd,cwd=build,env=env,stdout=output,stderr=subprocess.STDOUT)
    shutil.copytree(src/'newlib/libc/include',inc,dirs_exist_ok=True)
    shutil.copy2(build/'newlib.h',inc/'newlib.h'); shutil.copy2(src/'newlib/libc/include/_newlib_version.h',inc/'_newlib_version.h')
    for name in ('libc.a','libm.a'): shutil.copy2(build/name,lib/name)
    generate(inc,lib,work)
    shutil.copy2(ROOT/'sdk/ciuki.ld',lib/'ciuki.ld')
    def compile(path,obj):
        run([tool_info['clang']['path'],*cflags,'-Werror','-nostdinc','-isystem',str(Path(run([tool_info['clang']['path'],'-print-resource-dir'],capture_output=True,text=True).stdout.strip())/'include'),'-I',inc,'-I',work,'-c',path,'-o',obj],env=env)
    compile(ROOT/'sdk/crt/crt0.S',lib/'crt0.o')
    objects={}
    for family in ('crt','libciuki','libpthread'):
        objects[family]=[]
        for path in sorted((ROOT/'sdk'/family).glob('*.c')):
            obj=work/(family+'_'+path.stem+'.o'); compile(path,obj); objects[family].append(obj)
    obj=work/'syscalls.o'; compile(work/'syscalls.S',obj); objects['libciuki'].append(obj)
    for name,objs in [('libciuki',objects['crt']+objects['libciuki']),('libpthread',objects['libpthread'])]:
        run([tool_info['llvm-ar']['path'],'rcsD',lib/(name+'.a'),*objs])
    helpers=work/'helpers.o'; compile(ROOT/'sdk/newlib/helpers.c',helpers)
    run([tool_info['llvm-ar']['path'],'rcsD',lib/'libcompiler.a',helpers])
    binpath=OUT/'bin'; binpath.mkdir(); shutil.copy2(ROOT/'sdk/bin/ciuki-cc',binpath/'ciuki-cc'); (binpath/'ciuki-cc').chmod(0o755)
    # Persist the actual recipe before wrapper inspection.
    manifest={'tools':tool_info,'newlib':source,'configure':configure,'flags':cflags,'newlib_compile_extra_flags':['-Wno-error','-Wno-deprecated-non-prototype','-Dasm=__asm__'],'port':'CiukiOS equivalent configure.host + libciuki hooks',
              'sdk_repository':str(ROOT),'abi_sha256':digest(ABI),'patches':hashes,'build_seconds':None,'runtime_qualification':'not_run'}
    notice_dir=OUT/'licenses'; notice_dir.mkdir()
    shutil.copy2(src/'COPYING.NEWLIB',notice_dir/'COPYING.NEWLIB')
    shutil.copy2(ROOT/'sdk/LICENSE',notice_dir/'SDK-MIT.txt')
    inventory=[]
    # make's compile commands identify every compiled newlib file (a superset of linked members).
    dependency_text='\n'.join(p.read_text() for p in build.rglob('*.Po'))
    paths=set(re.findall(re.escape(str(src))+r'/([^\s]+\.[cSh])(?:\s|$)',log.read_text()+'\n'+dependency_text))
    paths={str((src/p).resolve().relative_to(src)) for p in paths if (src/p).is_file() and (src/p).resolve().is_relative_to(src)}
    for rel in sorted(paths):
        path=src/rel
        if not path.is_file(): continue
        contents=path.read_text(errors='replace')
        comments=re.findall(r'/\*.*?\*/',contents,re.S)
        notice='\n\n'.join(c for c in comments if re.search('copyright|licens|permission|public domain|warranty|redistribut',c,re.I))
        dst=notice_dir/'newlib'/rel; dst.parent.mkdir(parents=True,exist_ok=True)
        dst.write_text(notice or 'No inline notice; consult COPYING.NEWLIB and retained upstream source.\n')
        inventory.append({'source':rel,'sha256':digest(path),'notice':str(dst.relative_to(OUT)),'notice_sha256':digest(dst)})
    manifest['compiled_file_licenses']=inventory
    manifest['sdk_compiled_file_licenses']=[{'source':str(p.relative_to(ROOT)),'sha256':digest(p),'notice':'licenses/SDK-MIT.txt'} for p in sorted((ROOT/'sdk').rglob('*')) if p.suffix in ('.c','.S') and 'tests' not in p.parts]
    manifest['generated_stub_license']={'source':'work/syscalls.S','abi_sha256':digest(ABI),'notice':'licenses/SDK-MIT.txt'}
    manifest['license_scope']={'runtime':'newlib per-file notices; SDK MIT; exact ABI header GPL-2.0-only (contract issue documented)',
         'tools_build_only':'LLVM Apache-2.0 WITH LLVM-exception; NASM BSD; upstream configure/build licenses retained in source',
         'excluded':'libgloss, libnosys, libc/sys board ports, POSIX/unix/signal sample wrappers'}
    manifest['files']={str(p.relative_to(OUT)):digest(p) for p in sorted((OUT/'sysroot').rglob('*')) if p.is_file()}
    manifest['sdk_sources']={str(p.relative_to(ROOT)):digest(p) for p in sorted((ROOT/'sdk').rglob('*')) if p.is_file() and '__pycache__' not in str(p)}
    (OUT/'manifest.json').write_text(json.dumps(manifest,indent=2,sort_keys=True)+'\n')
    run([binpath/'ciuki-cc','--self-test'],env=env)
    run(['python3',ROOT/'sdk/tests/check_sdk.py'],env=env)
    manifest['build_seconds']=round(time.monotonic()-start,3)
    linked={}
    for mapfile in (OUT/'tests').glob('*.elf.map'):
        members=sorted(set(re.findall(r'([^\s/]+\.a)\(([^)]+)\)',mapfile.read_text())))
        linked[mapfile.name.removesuffix('.map')]=[{'archive':a,'member':o,'source_notices':['licenses/SDK-MIT.txt'] if a in ('libciuki.a','libpthread.a','libcompiler.a') else [entry['notice'] for entry in inventory if Path(entry['source']).stem==o.removeprefix('libc_a-').removeprefix('libm_a-').removesuffix('.o')]} for a,o in members]
    manifest['linked_file_licenses']=linked
    manifest['test_evidence']=json.loads((OUT/'tests/host-checks.json').read_text())
    manifest['installed_bytes']=sum(p.stat().st_size for p in (OUT/'sysroot').rglob('*') if p.is_file())
    manifest['license_files']={str(p.relative_to(OUT)):digest(p) for p in notice_dir.rglob('*') if p.is_file()}
    (OUT/'manifest.json').write_text(json.dumps(manifest,indent=2,sort_keys=True)+'\n')
    print(f"[sdk] complete: {manifest['build_seconds']} seconds; {manifest['installed_bytes']} installed bytes",flush=True)

if __name__=='__main__': main()
