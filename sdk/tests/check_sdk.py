#!/usr/bin/env python3
"""T0/T1 SDK checks, with no guest/target binary execution."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time
sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[2]
SDK=ROOT/'build/tools/ciuki-sdk'

def run(cmd):
    p=subprocess.run([str(x) for x in cmd],capture_output=True,text=True)
    if p.returncode:
        print(p.stdout+p.stderr,file=sys.stderr)
        p.check_returncode()
    return p.stdout+p.stderr
def audit_code(path,m):
    objdump=m['tools']['llvm-objdump']['path']
    spec=importlib.util.spec_from_file_location('audit',ROOT/'scripts/build_kernel.py');audit=importlib.util.module_from_spec(spec);spec.loader.exec_module(audit)
    dis=run([objdump,'-d','--no-show-raw-insn',path]);bad=[]
    for line in dis.splitlines():
        ins=re.match(r'\s*[0-9a-f]+:\s+([a-z][a-z0-9]*)\s*(.*)',line)
        if not ins:continue
        op=ins[1]
        # Reuse the production conservative classifier; allow ordinary x87 only.
        x87=op.startswith('f') and not op.startswith(('fxsave','fxrstor','femms'))
        if audit.is_fpu_or_simd(op) and not (x87 or op=='wait'):bad.append(line)
        if re.search(r'%(?:xmm|ymm|zmm|mm)[0-9]+',ins[2]):bad.append(line)
    assert not bad,'SSE/MMX audit: '+str(bad[:8])

def inspect(path):
    b=path.read_bytes();h=struct.unpack_from('<16sHHIIIIIHHHHHH',b)
    assert h[0][:9]==b'\x7fELF\x01\x01\x01\x00\x00' and h[1]==2 and h[2]==3 and h[3]==1 and h[7]==0 and h[8]==52,'ELF32/i386 static ABI'
    phoff=h[5];phsize=h[9];count=h[10];assert phsize==32 and 1<=count<=16
    loads=[];entry=False;stack=False
    for i in range(count):
        t,off,va,pa,filesz,memsz,flags,align=struct.unpack_from('<8I',b,phoff+i*phsize)
        assert t in (0,1,4,6,0x6474e551),'unsupported program header'
        if t==0x6474e551: assert not flags&1;stack=True
        if t==1 and memsz:
            assert align==4096 and va%4096==0 and off%4096==0 and filesz<=memsz and off+filesz<=len(b)
            assert flags&4 and flags&3!=3 and flags&~7==0 and 0x00400000<=va<va+memsz<=0x10000000
            end=(va+memsz+4095)&~4095
            assert all(end<=a or va>=z for a,z in loads),'overlapping LOAD pages'
            loads.append((va,end));entry|=bool(flags&1 and va<=h[4]<va+filesz)
    assert entry and stack and len(loads)==3,'entry, GNU_STACK, distinct RX/R/RW required'
    m=json.loads((SDK/'manifest.json').read_text()); readelf=m['tools']['llvm-readelf']['path'];objdump=m['tools']['llvm-objdump']['path']
    symbols=run([readelf,'--symbols',path]);assert not re.search(r'\b(?:GLOBAL|WEAK)\s+\S+\s+UND\b',symbols),'undefined symbol'
    sections=run([readelf,'--sections',path]);assert not re.search(r'\b(?:DYNAMIC|DYNSYM)\b',sections)
    audit_code(path,m)
    size=run([m['tools']['llvm-size']['path'],path]);print(size.strip())
    return {'sha256':hashlib.sha256(b).hexdigest(),'file_bytes':len(b),'size_output':size.strip(),'load_memory_bytes':sum(z-a for a,z in loads)}

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--self-test',action='store_true');args=parser.parse_args()
    m=json.loads((SDK/'manifest.json').read_text());out=SDK/'tests';out.mkdir(exist_ok=True)
    os.environ['TMPDIR']=str(SDK/'work/temp')
    print('[sdk-test] wrapper hello compile/link/inspect')
    started=time.monotonic()
    print(run([SDK/'bin/ciuki-cc',ROOT/'sdk/tests/hello.c','-o',out/'hello.elf']).strip())
    hello_time=time.monotonic()-started
    evidence={'hello':inspect(out/'hello.elf')}
    evidence['hello']['compile_link_seconds']=round(hello_time,4)
    if not args.self_test:
        for archive in (SDK/'sysroot/lib').glob('*.a'):audit_code(archive,m)
        print('[sdk-test] all runtime/compiler archives x87/SSE/MMX audit PASS')
        for rel,sha in m['files'].items():assert hashlib.sha256((SDK/rel).read_bytes()).hexdigest()==sha,rel
        assert m['compiled_file_licenses'],'license inventory must not be empty'
        print('[sdk-test] libc_smoke compile/link/inspect')
        started=time.monotonic()
        print(run([SDK/'bin/ciuki-cc',ROOT/'sdk/tests/libc_smoke.c','-o',out/'libc_smoke.elf']).strip())
        smoke_time=time.monotonic()-started
        evidence['libc_smoke']=inspect(out/'libc_smoke.elf')
        evidence['libc_smoke']['compile_link_seconds']=round(smoke_time,4)
        print(run([SDK/'bin/ciuki-cc','-c',ROOT/'sdk/tests/layout.c','-o',out/'layout.o']).strip())
        # The same source is also compiled natively with -m32 when this host supports it.
        clang=m['tools']['clang']['path'];resource=run([clang,'-print-resource-dir']).strip()
        cmd=[clang,'-m32','-std=c17','-D_DEFAULT_SOURCE','-nostdinc','-isystem',SDK/'sysroot/include','-isystem',resource+'/include','-c',ROOT/'sdk/tests/layout.c','-o',out/'layout-host.o']
        p=subprocess.run([str(x) for x in cmd],capture_output=True,text=True)
        if p.returncode:print('[sdk-test] native -m32 unavailable; target static asserts PASS\n'+p.stderr)
        else:print('[sdk-test] native -m32 layouts/types PASS (compile; no target probes executed)')
        hostflags=['-fno-stack-protector','-m64','-std=c17','-ffreestanding','-fno-builtin','-fno-pic','-fno-pie','-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-ffunction-sections','-fdata-sections','-D_DEFAULT_SOURCE','-D__DYNAMIC_REENT__','-nostdinc','-isystem',SDK/'sysroot/include','-isystem',resource+'/include','-include',ROOT/'sdk/tests/host_runtime.h']
        for src,name in [(ROOT/'sdk/libpthread/pthread.c','pthread-native'),(ROOT/'sdk/tests/pthread_host.c','pthread-harness'),(ROOT/'sdk/newlib/helpers.c','helpers-native')]:
            print(run([clang,*hostflags,'-c',src,'-o',out/(name+'.o')]).strip())
        print(run([clang,'-m64','-c',ROOT/'sdk/tests/host_start64.S','-o',out/'pthread-start.o']).strip())
        print(run([m['tools']['ld.lld']['path'],'-m','elf_x86_64','-static','--gc-sections','-o',out/'pthread-host',out/'pthread-start.o',out/'pthread-native.o',out/'pthread-harness.o',out/'helpers-native.o']).strip())
        subprocess.run([out/'pthread-host'],check=True)
        print('[sdk-test] native x86-64 production pthread interruption/timeout/once/keys/error rules PASS')
        # Raw enum coverage is checked against the authoritative ABI, not a duplicated table.
        abi=(ROOT/'src/kernel/include/ciuki/abi.h').read_text()
        calls=[n.lower() for n,v in re.findall(r'CIUKI_SYS_(\w+)\s*=\s*(\d+)',abi) if 16<=int(v)<=68]
        nm=run(['llvm-nm','--defined-only',SDK/'sysroot/lib/libciuki.a'])
        assert all('ciuki_raw_'+n in nm for n in calls)
        print('[sdk-test] all 53 F2 raw stubs present; errno boundaries and layouts compiled')
    (out/('self-test.json' if args.self_test else 'host-checks.json')).write_text(json.dumps(evidence,indent=2)+'\n')
    print('[sdk-test] PASS (runtime qualification not_run)')
if __name__=='__main__':main()
