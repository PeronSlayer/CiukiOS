#!/usr/bin/env python3
"""Native newlib/SDK reference; target layouts and guest evidence are separate."""
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[2]
SDK=ROOT/'build/tools/ciuki-sdk'
OUT=SDK/'tests/native-libc'


def build():
    OUT.mkdir(parents=True,exist_ok=True)
    source=SDK/'work/newlib-4.5.0.20241231'
    if not source.is_dir():raise RuntimeError('build the SDK before the native libc reference')
    inputs=[ROOT/'sdk/newlib/recipe.json',ROOT/'sdk/crt/start.c',ROOT/'sdk/libpthread/pthread.c',
            ROOT/'sdk/tests/libc_host.c',ROOT/'sdk/tests/libc_smoke.c',ROOT/'sdk/tests/libc_start64.S',
            ROOT/'sdk/tests/host_runtime.h',Path(__file__),*sorted((ROOT/'sdk/libciuki').glob('*.c'))]
    hashes={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    hashes['sdk-manifest']=hashlib.sha256((SDK/'manifest.json').read_bytes()).hexdigest()
    evidence=OUT/'inputs.json'
    if evidence.is_file() and json.loads(evidence.read_text())==hashes and (OUT/'libc-host').is_file():return OUT/'libc-host'
    tools=json.loads((SDK/'manifest.json').read_text())['tools']
    clang=tools['clang']['path'];lld=tools['ld.lld']['path']
    resource=subprocess.check_output([clang,'-print-resource-dir'],text=True).strip()
    # The native size_t is 64-bit; preserve the port's explicit ssize_t alias.
    types=OUT/'host-types.h'
    types.write_text('#ifndef __ASSEMBLER__\ntypedef int _ssize_t;\n#define __machine_ssize_t_defined 1\n#endif\n')
    cc=OUT/'host-cc'
    flags=['-m64','-std=c17','-ffreestanding','-fno-builtin','-fno-pie','-fno-pic','-fno-stack-protector',
           '-ffunction-sections','-fdata-sections','-O2','-g','-D_DEFAULT_SOURCE','-D__DYNAMIC_REENT__',
           '-nostdinc','-isystem',resource+'/include','-include',str(types)]
    cc.write_text('#!/bin/sh\nexec '+shlex.join([clang,'--target=x86_64-unknown-elf',*flags,
        '-isystem',str(source/'newlib/libc/include'),'-Wno-error','-Wno-deprecated-non-prototype','-Dasm=__asm__'])+' "$@"\n')
    cc.chmod(0o755)
    env={**os.environ,'TMPDIR':str(OUT),'CIUKI_NEWLIB_PORT':'yes','CC':str(cc),'CC_FOR_TARGET':str(cc),
         'AR':tools['llvm-ar']['path'],'AR_FOR_TARGET':tools['llvm-ar']['path'],
         'RANLIB':tools['llvm-ranlib']['path'],'RANLIB_FOR_TARGET':tools['llvm-ranlib']['path'],
         'AS':str(cc),'AS_FOR_TARGET':str(cc),'LD':lld,'LD_FOR_TARGET':lld,'CFLAGS':'-O2 -g'}
    native=OUT/'newlib-build';native.mkdir(exist_ok=True)
    args=json.loads((ROOT/'sdk/newlib/recipe.json').read_text())['configure']
    args=[a.replace('i686-unknown-elf','x86_64-unknown-elf') for a in args]
    with (OUT/'build.log').open('w') as log:
        for cmd in ([source/'newlib/configure',*args],['make','-j2','V=1']):
            subprocess.run(list(map(str,cmd)),cwd=native,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
        # Substitute the architectural GS load using the existing TCB boundary.
        start=(ROOT/'sdk/crt/start.c').read_text()
        old='    struct _reent *r; __asm__ volatile("movl %%gs:%c1,%0":"=r"(r):"i"(__builtin_offsetof(struct ciuki_tcb,reent))); return r;'
        if start.count(old)!=1:raise RuntimeError('CRT TCB substitution boundary changed')
        (OUT/'start.c').write_text(start.replace(old,'    return (void *)(uintptr_t)__ciuki_host_tcb()->reent;'))
        signals=(ROOT/'sdk/libciuki/signals.c').read_text()
        old='_Static_assert(sizeof(struct sigaction)==sizeof(struct ciuki_sigaction),"signal action wire size");'
        if signals.count(old)!=1:raise RuntimeError('signal native/target assertion boundary changed')
        # Native function pointers are 64-bit; this model never sends its
        # native sigaction to CiukiOS. Target check_sdk/layout retains the assert.
        (OUT/'signals.c').write_text(signals.replace(old,''))
        sources=[OUT/'start.c',ROOT/'sdk/tests/libc_start64.S',ROOT/'sdk/tests/libc_smoke.c',
                 ROOT/'sdk/tests/libc_host.c',ROOT/'sdk/libpthread/pthread.c',
                 *[OUT/'signals.c' if p.name=='signals.c' else p for p in sorted((ROOT/'sdk/libciuki').glob('*.c'))]]
        objects=[]
        for index,path in enumerate(sources):
            obj=OUT/f'{index}.o';objects.append(obj)
            compile_flags=[*flags,'-I',str(native),'-I',str(SDK/'sysroot/include'),
                           '-include',str(ROOT/'sdk/tests/host_runtime.h')] if path.suffix=='.c' else ['-m64']
            subprocess.run([clang,*compile_flags,'-c',str(path),'-o',str(obj)],env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
        ld=(ROOT/'sdk/ciuki.ld').read_text().replace('elf32-i386','elf64-x86-64').replace('OUTPUT_ARCH(i386)','OUTPUT_ARCH(i386:x86-64)')
        (OUT/'host.ld').write_text(ld)
        subprocess.run([lld,'-m','elf_x86_64','-static','--gc-sections','-L',str(SDK/'sysroot/lib'),'-T',str(OUT/'host.ld'),
            '--wrap=__ciuki_start','--wrap=__getreent','--wrap=ciuki_syscall','--wrap=ciuki_raw_probe_report','-o',str(OUT/'libc-host'),
            *map(str,objects),'--start-group',str(native/'libc.a'),str(native/'libm.a'),'--end-group'],
            env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
    evidence.write_text(json.dumps(hashes,indent=2)+'\n')
    return OUT/'libc-host'


if __name__=='__main__':print(build())
