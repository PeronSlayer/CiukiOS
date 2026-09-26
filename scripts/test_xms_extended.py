#!/usr/bin/env python3
"""Execute XMS services in a complete compiled CiukiDOS kernel under Unicorn.

No XMS instructions are copied or replaced. Only port 92h is modeled. This
checks allocator/API behavior, not memory moves, BIOS memory discovery or a
working Jemm/DPMI session. Requires Unicorn (uv run --with unicorn python3 ...).
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_INSN, UC_HOOK_INTR
from unicorn.x86_const import (
    UC_X86_INS_IN, UC_X86_INS_OUT, UC_X86_REG_CS, UC_X86_REG_DS,
    UC_X86_REG_ES, UC_X86_REG_FS, UC_X86_REG_GS, UC_X86_REG_SS,
    UC_X86_REG_SP, UC_X86_REG_IP, UC_X86_REG_EFLAGS, UC_X86_REG_EAX,
    UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_ESI,
    UC_X86_REG_EDI, UC_X86_REG_EBP,
)

ROOT=Path(__file__).resolve().parents[1]


def symbol(listing,name):
    lines=listing.splitlines()
    pattern=re.compile(r'\b'+re.escape(name)+r'(?=:|\s+(?:d[bwd]|times)\b)')
    for i,line in enumerate(lines):
        if pattern.search(line):
            for candidate in lines[i:]:
                match=re.match(r'\s*\d+\s+([0-9A-F]{8})\s',candidate)
                if match:return int(match[1],16)
    raise AssertionError('missing assembled symbol '+name)


class Machine:
    def __init__(self,binary,listing):
        self.binary=binary
        self.offsets={name:symbol(listing,name) for name in
                      ['xms_entrypoint','xms_free_kb','xms_tail_kb',
                       'xms_handle_size_table','xms_handle_base_table','boot_drive']}
        self.base,self.segment=0x9000,0x900
        self.cpu=Uc(UC_ARCH_X86,UC_MODE_16)
        self.cpu.mem_map(0,0x100000)
        self.cpu.mem_write(self.base,binary)
        self.port92=0
        self.port_reads,self.port_writes=[],[]
        self.cpu.hook_add(UC_HOOK_INSN,self.read_port,None,1,0,UC_X86_INS_IN)
        self.cpu.hook_add(UC_HOOK_INSN,self.write_port,None,1,0,UC_X86_INS_OUT)
        self.cpu.hook_add(UC_HOOK_INTR,self.interrupt)
        self.initial=self.word('xms_free_kb')
        assert self.initial==0xfbc0,hex(self.initial)

    def interrupt(self,_cpu,number,_data):
        raise AssertionError(f'unexpected interrupt {number:02x}')

    def read_port(self,_cpu,port,size,_data):
        assert (port,size)==(0x92,1),'unexpected I/O input'
        self.port_reads.append(self.port92)
        return self.port92

    def write_port(self,_cpu,port,size,value,_data):
        assert (port,size)==(0x92,1),'unexpected I/O output'
        assert value&1==0,'A20 update asserted reset'
        self.port92=value
        self.port_writes.append(value)

    def memory(self,name,size):
        return bytes(self.cpu.mem_read(self.base+self.offsets[name],size))

    def word(self,name):return int.from_bytes(self.memory(name,2),'little')

    def state(self):
        return (self.memory('xms_free_kb',2),self.memory('xms_tail_kb',2),
                self.memory('xms_handle_size_table',32),self.memory('xms_handle_base_table',32))

    def call(self,function,edx=0):
        cpu=self.cpu
        for reg,value in [(UC_X86_REG_CS,self.segment),(UC_X86_REG_DS,0x5000),
                          (UC_X86_REG_ES,0x5100),(UC_X86_REG_FS,0x5200),
                          (UC_X86_REG_GS,0x5300),(UC_X86_REG_SS,0x7000),
                          (UC_X86_REG_SP,0xfff0),(UC_X86_REG_EFLAGS,0x202),
                          (UC_X86_REG_EAX,0xa5a50000|function<<8),
                          (UC_X86_REG_EBX,0xb6b612fe),(UC_X86_REG_ECX,0xc7c73456),
                          (UC_X86_REG_EDX,edx),(UC_X86_REG_ESI,0xe9e95678),
                          (UC_X86_REG_EDI,0xfafa6789),(UC_X86_REG_EBP,0xdbdb789a)]:
            cpu.reg_write(reg,value)
        preserved=[UC_X86_REG_DS,UC_X86_REG_ES,UC_X86_REG_FS,UC_X86_REG_GS,
                   UC_X86_REG_SS,UC_X86_REG_ESI,UC_X86_REG_EDI,UC_X86_REG_EBP]
        previous={reg:cpu.reg_read(reg) for reg in preserved}
        cpu.mem_write(0x7fff0,struct.pack('<HH',0x1234,0x6000))
        cpu.emu_start(self.base+self.offsets['xms_entrypoint'],0x61234,count=100000)
        assert (cpu.reg_read(UC_X86_REG_CS),cpu.reg_read(UC_X86_REG_IP))==(0x6000,0x1234)
        assert cpu.reg_read(UC_X86_REG_SP)==0xfff4
        assert cpu.reg_read(UC_X86_REG_EFLAGS)&0x200
        for reg,value in previous.items():assert cpu.reg_read(reg)==value,('register',reg)
        code_end=self.offsets['boot_drive']
        assert bytes(cpu.mem_read(self.base,code_end))==self.binary[:code_end], 'kernel code changed'
        assert self.port92&2,'global A20 repair omitted'
        return {name:cpu.reg_read(reg) for name,reg in
                [('eax',UC_X86_REG_EAX),('ebx',UC_X86_REG_EBX),
                 ('ecx',UC_X86_REG_ECX),('edx',UC_X86_REG_EDX)]}

    def query(self,free=None):
        expected=self.word('xms_free_kb') if free is None else free
        result=self.call(0x88,0xffffffff)
        assert result['eax']==expected and result['edx']==expected,result
        assert result['ecx']==0x03ffffff,result
        assert result['ebx']&255==0,result
        legacy=self.call(8,0xd8d8ffff)
        assert legacy['eax']&0xffff==expected and legacy['edx']&0xffff==expected,legacy
        assert legacy['eax']>>16==0xa5a5 and legacy['edx']>>16==0xd8d8,legacy
        assert legacy['ecx']==0xc7c73456 and legacy['ebx']&255==0,legacy
        return result

    def allocate(self,size,function=0x89):
        result=self.call(function,size)
        assert result['eax']&0xffff==1 and result['ebx']&255==0,result
        return result['edx']&0xffff

    def free(self,handle):
        result=self.call(0x0a,handle)
        assert result['eax']&0xffff==1 and result['ebx']&255==0,result


def qualify(binary,listing):
    factory=lambda:Machine(binary,listing)
    cases=[]
    machine=factory();machine.query(machine.initial)
    cases.append('extended_query_zero_extends_and_bounds_pool')
    for request in [0x10000,0x180a0,0xffff0000,0xffffffff,0xffff]:
        machine=factory();before=machine.state()
        result=machine.call(0x89,request)
        assert result['eax']&0xffff==0 and result['ebx']&255==0xa0,result
        assert machine.state()==before,'rejected request changed allocation state'
        machine.query(machine.initial)
        cases.append(f'extended_reject_{request:08x}_without_truncation')
    for size in [0,1,32928]:
        extended=factory();legacy=factory()
        ext_handle=extended.allocate(size)
        old_handle=legacy.allocate(size,9)
        assert ext_handle==old_handle==1
        assert extended.state()==legacy.state()
        extended.query(extended.initial-size)
        result=extended.call(0x0e,ext_handle)
        assert result['eax']&0xffff==1 and result['edx']&0xffff==size,result
        extended.free(ext_handle)
        extended.query(extended.initial)
        assert extended.word('xms_tail_kb')==0
        cases.append(f'extended_matches_legacy_allocate_query_free_{size}')
    machine=factory();before=machine.state()
    handle=machine.allocate(0xcafe0001,9)
    assert handle==1 and machine.word('xms_free_kb')==machine.initial-1
    machine.free(handle);assert machine.state()==before
    cases.append('legacy_dx_request_still_ignores_edx_high_word')
    machine=factory();handles=[machine.allocate(0) for _ in range(16)]
    assert handles==list(range(1,17))
    assert machine.memory('xms_handle_size_table',32)==b'\xff'*32
    machine.query(machine.initial)
    snapshot=machine.state()
    for function in [9,0x89]:
        result=machine.call(function,0)
        assert result['eax']&0xffff==0 and result['ebx']&255==0xa1,result
        assert machine.state()==snapshot
    machine.free(handles[7]);assert machine.allocate(1)==8
    machine.query(machine.initial-1)
    for handle in reversed(handles):machine.free(handle)
    machine.query(machine.initial)
    cases.append('sixteen_handles_zero_size_limit_reuse_and_recovery')
    machine=factory();first=machine.allocate(1);second=machine.allocate(32928)
    machine.free(first);machine.query(machine.initial-32929)
    third=machine.allocate(1);assert third==first
    assert struct.unpack('<H',machine.memory('xms_handle_base_table',2))[0]==32929
    machine.free(second);machine.query(machine.initial-32930)
    machine.free(third);machine.query(machine.initial)
    cases.append('non_lifo_free_preserves_live_high_water_and_locked_addresses')
    machine=factory();machine.port92=0xa4
    machine.query();assert machine.port92==0xa6
    assert machine.port_reads and machine.port_writes
    cases.append('A20_repair_preserves_other_port_bits')
    return cases


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    default=ROOT/'build/full/vm-session-2026-09-26'
    parser.add_argument('--kernel',type=Path,default=default/'ciukidos.sys')
    parser.add_argument('--listing',type=Path,default=default/'ciukidos.lst')
    parser.add_argument('--output',type=Path,default=default/'xms-extended-report.json')
    args=parser.parse_args()
    binary=args.kernel.read_bytes();listing=args.listing.read_text()
    assert len(binary)<=0xa900,'kernel size guard exceeded'
    report={'status':'running','kernel':str(args.kernel),'kernel_bytes':len(binary),
            'kernel_sha256':hashlib.sha256(binary).hexdigest(),
            'listing_sha256':hashlib.sha256(args.listing.read_bytes()).hexdigest(),
            'scope':'Full compiled XMS entry instructions; modeled port92; no Jemm/DPMI/device integration claim'}
    try:
        report['cases']=qualify(binary,listing)
        report['status']='PASS'
    except Exception as error:
        report.update(status='FAIL',error=str(error))
        raise
    finally:
        args.output.parent.mkdir(parents=True,exist_ok=True)
        args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':main()
