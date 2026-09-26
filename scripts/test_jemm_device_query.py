#!/usr/bin/env python3
"""Execute the assembled adapter with modeled DOS and a real-mode test device.

The fixture device executes actual strategy/interrupt instructions, but is not
Jemm. This checks argument/ownership/failure handling; QEMU must qualify Jemm
load, registered device queries, JLOAD and real unload independently.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from unicorn import Uc,UC_ARCH_X86,UC_MODE_16,UC_HOOK_INTR
from unicorn.x86_const import *

ROOT=Path(__file__).resolve().parents[1]

DEVICE=r'''
bits 16
org 0x300
strategy:
 mov [cs:0x500],bx
 mov [cs:0x502],es
 retf
times 0x100-($-$$) db 0
interrupt:
 pushad
 push ds
 push es
 lds si,[cs:0x500]
 cmp byte [si],20
 jne broken
 cmp byte [si+2],3
 jne broken
 les di,[si+14]
 mov word [es:di],0x28
 mov word [si+3],0x100
 mov al,[cs:0x504]
 cmp al,1
 jne mode2
 mov word [si+3],0x810b
mode2:
 cmp al,2
 jne mode3
 mov word [si+3],0
mode3:
 cmp al,3
 jne done
 inc word [si+18]
 jmp done
broken:
 mov word [si+3],0x8103
done:
 pop es
 pop ds
 popad
 retf
'''


class Machine:
    def __init__(self,adapter,device,name=b'EMMXXXX0',open_error=2):
        self.cpu=Uc(UC_ARCH_X86,UC_MODE_16);self.cpu.mem_map(0,0x100000)
        self.cpu.mem_write(0x20000,adapter);self.cpu.mem_write(0x12300,device)
        self.cpu.mem_write(0x29000,name+b'\0')
        self.cpu.mem_write(0x10222,struct.pack('<HHHHH8s',0x100,0x1200,0x8004,0,0,b'NUL     '))
        self.cpu.mem_write(0x12100,struct.pack('<HHHHH8s',0xffff,0xffff,0xc000,0x300,0x400,name))
        self.open_error=open_error;self.calls=[];self.normal_handle=7
        self.cpu.hook_add(UC_HOOK_INTR,self.dos)

    def dos(self,cpu,number,_):
        assert number==0x21
        ax=cpu.reg_read(UC_X86_REG_AX);bx=cpu.reg_read(UC_X86_REG_BX)
        self.calls.append((ax,bx))
        flags=cpu.reg_read(UC_X86_REG_EFLAGS)
        if ax==0x3d00:
            cpu.reg_write(UC_X86_REG_AX,self.open_error or self.normal_handle)
            cpu.reg_write(UC_X86_REG_EFLAGS,(flags&~1)|bool(self.open_error))
        elif ax>>8==0x52:
            cpu.reg_write(UC_X86_REG_ES,0x1000);cpu.reg_write(UC_X86_REG_BX,0x200)
        elif ax>>8==0x3e:
            assert bx!=0xfffe,'private handle leaked to DOS close'
            cpu.reg_write(UC_X86_REG_AX,0);cpu.reg_write(UC_X86_REG_EFLAGS,flags&~1)
        else:
            cpu.reg_write(UC_X86_REG_AX,1);cpu.reg_write(UC_X86_REG_EFLAGS,flags|1)

    def call(self,ax,bx=0,cx=6,dx=0x9000,ds=0x2000):
        cpu=self.cpu
        for reg,val in [(UC_X86_REG_CS,0x2000),(UC_X86_REG_DS,ds),(UC_X86_REG_SS,0x2000),
                        (UC_X86_REG_ES,0x3300),(UC_X86_REG_FS,0x3400),(UC_X86_REG_GS,0x3500),
                        (UC_X86_REG_SP,0xfff0),(UC_X86_REG_EAX,0xa5a50000|ax),
                        (UC_X86_REG_EBX,0xb6b60000|bx),(UC_X86_REG_ECX,0xc7c70000|cx),
                        (UC_X86_REG_EDX,0xd8d80000|dx),(UC_X86_REG_ESI,0x12345678),
                        (UC_X86_REG_EDI,0x23456789),(UC_X86_REG_EBP,0x3456789a),
                        (UC_X86_REG_EFLAGS,0x202)]:cpu.reg_write(reg,val)
        preserved=[UC_X86_REG_EBX,UC_X86_REG_ECX,UC_X86_REG_EDX,UC_X86_REG_ESI,
                   UC_X86_REG_EDI,UC_X86_REG_EBP,UC_X86_REG_DS,UC_X86_REG_ES,
                   UC_X86_REG_FS,UC_X86_REG_GS,UC_X86_REG_SS]
        before={r:cpu.reg_read(r) for r in preserved}
        cpu.mem_write(0x2fff0,struct.pack('<H',0xf000))
        cpu.emu_start(0x20000,0x2f000,count=50000)
        assert cpu.reg_read(UC_X86_REG_IP)==0xf000,'loop failed to terminate'
        assert cpu.reg_read(UC_X86_REG_SP)==0xfff2
        for r,v in before.items():assert cpu.reg_read(r)==v,('register changed',r)
        assert cpu.reg_read(UC_X86_REG_EAX)>>16==0xa5a5
        return cpu.reg_read(UC_X86_REG_AX),bool(cpu.reg_read(UC_X86_REG_EFLAGS)&1)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,default=ROOT/'build/full/jemm-device-query-2026-09-26')
    p.add_argument('--jwasm',type=Path,default=ROOT/'build/external/JWasm/build/GccUnixR/jwasm')
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    asm=a.output/'adapter.asm';binary=a.output/'adapter.bin'
    asm.write_text('.model tiny\n.386\n.code\norg 0\ninclude jemm_device_query.inc\nend\n')
    subprocess.run([str(a.jwasm),'-nologo','-bin','-I'+str(ROOT/'src/vm'),'-Fo'+str(binary),str(asm)],check=True)
    (a.output/'device.asm').write_text(DEVICE)
    subprocess.run(['nasm','-f','bin',str(a.output/'device.asm'),'-o',str(a.output/'device.bin')],check=True)
    adapter=binary.read_bytes();device=(a.output/'device.bin').read_bytes()
    factory=lambda **kw:Machine(adapter,device,**kw)
    cases=[]
    for name in [b'EMMXXXX0',b'EMMQXXX0']:
        m=factory(name=name);assert m.call(0x3d00)==(0xfffe,False)
        m.cpu.mem_write(0x29200,b'\0'*6)
        assert m.call(0x4402,bx=0xfffe,dx=0x9200)==(6,False)
        assert bytes(m.cpu.mem_read(0x29200,2))==b'\x28\0'
        calls=len(m.calls)
        assert m.call(0x4403,bx=0xfffe,dx=0x9200)==(1,True)
        assert len(m.calls)==calls
        assert m.call(0x3e00,bx=0xfffe)==(0,False)
        assert m.call(0x3e00,bx=0xfffe)==(6,True)
        assert m.call(0x4402,bx=0xfffe,dx=0x9200)==(6,True)
        assert len(m.calls)==calls
        assert m.call(0x3d00)==(0xfffe,False)
        cases.append(name.decode()+'_genuine_requests_and_close_reopen')
    for error in [0,5]:
        m=factory(open_error=error)
        assert m.call(0x3d00)==(error or 7,bool(error))
        assert len(m.calls)==1
        cases.append(f'normal_DOS_open_result_{error}_unchanged')
    m=factory(name=b'NOTEMM00');assert m.call(0x3d00)==(2,True);assert len(m.calls)==1
    cases.append('unrelated_filename_never_walks_chain')
    for mode in ['no_ioctl','cycle','bad_pointer','bad_strategy']:
        m=factory()
        if mode=='no_ioctl':m.cpu.mem_write(0x12104,struct.pack('<H',0x8000))
        if mode=='cycle':
            m.cpu.mem_write(0x12100,struct.pack('<HH',0x100,0x1200))
            m.cpu.mem_write(0x1210a,b'OTHERDEV')
        if mode=='bad_pointer':m.cpu.mem_write(0x10222,struct.pack('<HH',0,0xf000))
        if mode=='bad_strategy':
            # A valid header near the top of DOS RAM can have an invalid far entry.
            header=struct.pack('<HHHHH8s',0xffff,0xffff,0xc000,0xffff,0x400,b'EMMXXXX0')
            m.cpu.mem_write(0x99000,header)
            m.cpu.mem_write(0x10222,struct.pack('<HH',0,0x9900))
        assert m.call(0x3d00)==(2,True)
        cases.append(mode+'_rejected')
    for mode in [1,2,3]:
        m=factory();assert m.call(0x3d00)==(0xfffe,False)
        m.cpu.mem_write(0x12504,bytes([mode]))
        assert m.call(0x4402,bx=0xfffe,dx=0x9200)==(13,True)
        cases.append(f'device_status_or_count_{mode}_rejected')
    for cx,dx,ds in [(0,0x9200,0x2000),(0x200,0xff00,0x2000),(6,0x9200,0x2100)]:
        m=factory();assert m.call(0x3d00)==(0xfffe,False)
        assert m.call(0x4402,bx=0xfffe,cx=cx,dx=dx,ds=ds)==(13,True)
        cases.append(f'transfer_bounds_{cx}_{dx}_{ds}_rejected')
    report={'status':'PASS','scope':'assembled adapter; modeled DOS and fixture character device, not Jemm integration',
            'adapter_sha256':hashlib.sha256(adapter).hexdigest(),'cases':cases}
    (a.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':main()
