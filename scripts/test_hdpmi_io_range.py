#!/usr/bin/env python3
"""Execute the shipped HDPMI range allocator, assembled unchanged by JWasm.

Requires Unicorn (e.g. uv run --with unicorn python3 scripts/test_hdpmi_io_range.py).
This is a focused instruction-level check with modeled descriptors/TSS, not
a DPMI client, V86 integration, device test or sound qualification.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import (
    UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS,
    UC_X86_REG_FS, UC_X86_REG_GS, UC_X86_REG_GDTR, UC_X86_REG_CR0,
    UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX,
    UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP, UC_X86_REG_ESP,
    UC_X86_REG_EIP, UC_X86_REG_EFLAGS,
)

ROOT = Path(__file__).resolve().parents[1]
COMMIT = 'f2276db9accfc57facf2588bc016a27130597bb1'
REGS = [UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX,
        UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP]
SEGMENTS = [UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS,
            UC_X86_REG_FS, UC_X86_REG_GS]


def section(text, first, last):
    start = text.index(first)
    return text[start:text.index(last, start) + len(last)]


def run(command, **kwargs):
    return subprocess.check_output(command, text=True, **kwargs)


def descriptor(base, access):
    limit = 0xfffff
    return struct.pack('<HHBBBB', limit & 0xffff, base & 0xffff,
                       (base >> 16) & 255, access, 0xc0 | (limit >> 16),
                       base >> 24)


class RangeMachine:
    def __init__(self, image, symbols, bits):
        self.bits, self.symbols = bits, symbols
        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        self.uc.mem_map(0, 0x800000)
        self.uc.mem_write(0x100000, image)
        # Host flat CS/SS and writable code alias; client DS has a nonzero base.
        self.uc.mem_write(0x8000, b'\0' * 8 + descriptor(0, 0x9a)
                          + descriptor(0, 0x92) + descriptor(0x400000, 0xf2))
        self.uc.reg_write(UC_X86_REG_GDTR, (0, 0x8000, 31, 0))
        self.uc.reg_write(UC_X86_REG_CR0, 1)
        for register, value in [(UC_X86_REG_CS, 8), (UC_X86_REG_SS, 16),
                                (UC_X86_REG_DS, 27), (UC_X86_REG_ES, 27),
                                (UC_X86_REG_FS, 27), (UC_X86_REG_GS, 27)]:
            self.uc.reg_write(register, value)
        self.entry_size = 16 if bits == 32 else 12
        self.handlers = (struct.pack('<IHIH', 0x12345678, 0x2f, 0x87654321, 0x37)
                         if bits == 32 else struct.pack('<HHHH', 0x5678, 0x2f, 0x4321, 0x37))
        self.pointer = 0x11200 if bits == 32 else 0x1200
        self.uc.mem_write(0x400000 + self.pointer, self.handlers)
        self.uc.mem_write(self.symbols['iobitmap'] + 8192, b'\xff')

    def bytes(self, address, length):
        return bytes(self.uc.mem_read(address, length))

    def state(self):
        return (self.bytes(self.symbols['taskseg'], 104 + 8193),
                self.bytes(self.symbols['traphdl'], self.entry_size * 8))

    def invoke(self, function, start=0, count=0, handle=0):
        uc = self.uc
        values = {UC_X86_REG_EAX: 6 if function == 'is0006' else 7,
                  UC_X86_REG_EBX: 0x87654321, UC_X86_REG_ECX: 0xaabb0000 | count,
                  UC_X86_REG_EDX: 0xccdd0000 | start,
                  UC_X86_REG_ESI: self.pointer if self.bits == 32 else 0xfefe0000 | self.pointer,
                  UC_X86_REG_EDI: 0x10203040, UC_X86_REG_EBP: 0x18273645}
        if function == 'is0007':
            values[UC_X86_REG_EDX] = handle
        for register, value in values.items():
            uc.reg_write(register, value)
        before = {r: uc.reg_read(r) for r in REGS + SEGMENTS}
        stack, stop = 0x300000, 0x700000
        uc.mem_write(stack, struct.pack('<I', stop))
        uc.reg_write(UC_X86_REG_ESP, stack)
        # DF=1 deliberately: the handler must copy the client table forwards.
        uc.reg_write(UC_X86_REG_EFLAGS, 0x603)
        uc.emu_start(self.symbols[function], stop, count=2_000_000)
        assert uc.reg_read(UC_X86_REG_EIP) == stop, 'handler failed to return'
        assert uc.reg_read(UC_X86_REG_ESP) == stack + 4, 'stack imbalance'
        for register, value in before.items():
            assert uc.reg_read(register) == value, ('register changed', register)
        return bool(uc.reg_read(UC_X86_REG_EFLAGS) & 1), uc.reg_read(UC_X86_REG_EAX)

    def allocate(self, start, count):
        old = self.state()
        failed, handle = self.invoke('is0006', start, count)
        if failed:
            assert self.state() == old, 'failed range changed trap table or IOPB'
        else:
            assert self.symbols['traphdl'] <= handle < self.symbols['traphdl'] + 8*self.entry_size
            expected = struct.pack('<HH', count, start) + self.handlers
            assert self.bytes(handle, self.entry_size) == expected, 'client DS/handler table copy differs'
            bitmap = self.bytes(self.symbols['iobitmap'], 8193)
            original = old[0][104:]
            for port in range(65536):
                actual = (bitmap[port // 8] >> (port % 8)) & 1
                previous = (original[port // 8] >> (port % 8)) & 1
                assert actual == (1 if start <= port < start + count else previous), port
            assert bitmap[-1] == 255, 'IOPB terminator overwritten'
        return failed, handle


def build(source, jwasm, output, bits):
    text = (source/'Src/HDPMI/I2FHDPMI.ASM').read_text()
    includes = (source/'Src/HDPMI/HDPMI.INC').read_text()
    structures = '\n'.join(section(includes, name+' struct', name+' ends')
                           for name in ('PUSHADS', 'TSSSEG'))
    types = section(text, 'NUMTRAP equ', 'TRAPH ends')
    procedures = '\n'.join(section(text, name+' proc', name+' endp')
                             for name in ('checkrange', 'is0006', 'is0007'))
    assembly = ('.386p\n.model flat\noption casemap:none\n'
                f'?32BIT equ {int(bits == 32)}\n_CSALIAS_ equ 10h\n'
                + structures + '\n' + types
                + '\n.data\npublic traphdl, taskseg, iobitmap\n'
                + 'traphdl TRAPH NUMTRAP dup (<>)\ntaskseg TSSSEG <>\n'
                + 'iobitmap db 8193 dup (0)\n.code\npublic is0006, is0007\n'
                + procedures + '\nend\n')
    output.mkdir(parents=True, exist_ok=True)
    asm, obj, elf, binary = [output/('range'+suffix) for suffix in ('.asm','.o','.elf','.bin')]
    asm.write_text(assembly)
    (output/'link.ld').write_text('SECTIONS { . = 0x100000; .text : { *(.text*) } .data : { *(.data*) } .bss : { *(.bss*) } }\n')
    log = run([str(jwasm), '-nologo', '-elf', '-Cp', '-Fo'+str(obj), str(asm)])
    (output/'assemble.log').write_text(log)
    run(['ld', '-m', 'elf_i386', '-T', str(output/'link.ld'), '-o', str(elf), str(obj)])
    run(['objcopy', '-O', 'binary', str(elf), str(binary)])
    symbols = {fields[2]: int(fields[0],16) for line in run(['nm', str(elf)]).splitlines()
               if len(fields := line.split()) == 3}
    return binary.read_bytes(), symbols


def qualify(image, symbols, bits):
    factory = lambda: RangeMachine(image, symbols, bits)
    cases = []
    for start, count in [(0x3c0,0),(0xffff,2),(0xfff0,0x20),(0x9000,0xffff)]:
        machine=factory()
        assert machine.allocate(start,count)[0]
        cases.append(f'reject_{start:04x}_{count:04x}_unchanged')
    for start,count in [(0,1),(0x3b0,0x30),(0xffff,1),(0xfffe,2),(1,65535)]:
        machine=factory();before=machine.state()
        failed,handle=machine.allocate(start,count)
        assert not failed
        assert not machine.invoke('is0007',handle=handle)[0]
        assert machine.state()[0] == before[0], 'untrap did not restore IOPB/TSS'
        assert struct.unpack('<H', machine.bytes(handle,2))[0] == 0
        snapshot=machine.state()
        assert machine.invoke('is0007',handle=handle)[0]
        assert machine.state()==snapshot
        cases.append(f'allocate_release_{start:04x}_{count:04x}')
    machine=factory()
    handles=[]
    for slot in range(8):
        failed,handle=machine.allocate(0x220+slot*0x20,0x10)
        assert not failed
        handles.append(handle)
    assert machine.allocate(0x3c0,0x20)[0]
    cases.append('full_table_failure_preserves_ports')
    assert not machine.invoke('is0007',handle=handles[3])[0]
    assert machine.allocate(0x3c0,0x20)==(False,handles[3])
    cases.append('released_slot_reused_with_client_callbacks')
    for start,count in [(0x21f,2),(0x220,1),(0x22f,2),(0x218,0x20)]:
        machine=factory();assert not machine.allocate(0x220,0x10)[0]
        assert machine.allocate(start,count)[0]
        cases.append(f'overlap_rejected_{start:04x}_{count:04x}')
    machine=factory()
    address=symbols['iobitmap']+0x3c9//8
    machine.uc.mem_write(address,bytes([1<<(0x3c9%8)]))
    assert machine.allocate(0x3c0,0x20)[0]
    cases.append('foreign_iopb_owner_preserved')
    machine=factory();assert not machine.allocate(0x220,0x10)[0]
    before=machine.state()
    for handle in [0,symbols['traphdl']+1,symbols['traphdl']+8*machine.entry_size]:
        assert machine.invoke('is0007',handle=handle)[0]
        assert machine.state()==before
    cases.append('invalid_release_preserves_ownership')
    return cases


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=ROOT/'build/external/audio-compat'/('HX-'+COMMIT))
    parser.add_argument('--jwasm',type=Path,default=ROOT/'build/external/JWasm/build/GccUnixR/jwasm')
    parser.add_argument('--output',type=Path,default=ROOT/'build/full/hdpmi-io-range-2026-09-26')
    args=parser.parse_args()
    actual=run(['git','-C',str(args.source),'rev-parse','HEAD']).strip()
    assert actual==COMMIT, 'test is pinned to the packaged official HDPMI source'
    file=args.source/'Src/HDPMI/I2FHDPMI.ASM'
    assert not run(['git','-C',str(args.source),'diff','HEAD','--','Src/HDPMI/I2FHDPMI.ASM']).strip(), 'range source is modified'
    report={'status':'running','upstream_commit':COMMIT,'source_sha256':hashlib.sha256(file.read_bytes()).hexdigest(),
            'scope':'Unmodified official allocator instructions; modeled GDT/TSS; no device or DPMI integration claim','variants':{}}
    for bits in (16,32):
        image,symbols=build(args.source,args.jwasm,args.output/str(bits),bits)
        report['variants'][str(bits)]={'status':'PASS','binary_sha256':hashlib.sha256(image).hexdigest(),
                                      'cases':qualify(image,symbols,bits)}
    report['status']='PASS'
    (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':
    main()
