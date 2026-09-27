#!/usr/bin/env python3
"""Execute the actual assembled scheduler IRQ/validation routines in Unicorn.

Only the descriptor, page-table memory, flat segments and nested CALL are modeled.
The test injects a nested callback at the real validator entry; it does not claim
that QEMU or physical hardware produced a reentrant IRQ. No production routine
instructions are replaced or copied into a second implementation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.x86_const import *

ROOT = Path(__file__).resolve().parents[1]
BASE, DESCRIPTOR, PAGE_TABLE = 0x100000, 0x200000, 0x210000
CALLER_STACK, STOP, NESTED_STOP = 0x300000, 0x700000, 0x700100
GPRS = (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX,
        UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP)
SEGMENTS = (UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_FS,
            UC_X86_REG_GS, UC_X86_REG_SS)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def section(source, first, last):
    start = source.index(first)
    return source[start:source.index(last, start) + len(last)]


def command(args):
    return subprocess.check_output(args, text=True)


def build(source, output, jwasm):
    text = source.read_text()
    routines = '\n'.join(section(text, name+' proc', name+' endp')
                         for name in ('scheduler_validate', 'vm_scheduler_irq'))
    storage = section(text, 'scheduler_storage_start label byte',
                      'scheduler_storage_end label byte')
    exported = ('scheduler_descriptor', 'scheduler_busy', 'scheduler_saved_esp',
                'scheduler_internal_ticks', 'scheduler_internal_reentries',
                'scheduler_stack', 'scheduler_stack_end')
    assembly = ('.386p\n.model flat\noption casemap:none\n'
                'include session_abi.inc\ninclude session_scheduler_abi.inc\n'
                f'VGA_PTES equ {PAGE_TABLE}\n.data\npublic '+','.join(exported)
                +'\n'+storage+'\n.code\npublic scheduler_validate,vm_scheduler_irq\n'
                +routines+'\nend\n')
    output.mkdir(parents=True, exist_ok=True)
    asm, obj, elf, binary = [output/('scheduler'+suffix)
                            for suffix in ('.asm', '.o', '.elf', '.bin')]
    asm.write_text(assembly)
    link = output/'link.ld'
    link.write_text('SECTIONS { . = 0x100000; .text : { *(.text*) } '
                    '.data : { *(.data*) } .bss : { *(.bss*) } }\n')
    log = command([str(jwasm), '-nologo', '-elf', '-Cp',
                   '-I'+str(ROOT/'src/vm'), '-Fo'+str(obj), str(asm)])
    (output/'assemble.log').write_text(log)
    command(['ld', '-m', 'elf_i386', '-T', str(link), '-o', str(elf), str(obj)])
    command(['objcopy', '-O', 'binary', str(elf), str(binary)])
    symbols = {fields[2]: int(fields[0], 16)
               for line in command(['nm', str(elf)]).splitlines()
               if len(fields := line.split()) == 3}
    return binary.read_bytes(), symbols, digest(routines.encode())


def segment(access):
    return struct.pack('<HHBBBB', 0xffff, 0, 0, access, 0xcf, 0)


class Machine:
    def __init__(self, binary, symbols):
        self.symbols = symbols
        self.cpu = cpu = Uc(UC_ARCH_X86, UC_MODE_32)
        cpu.mem_map(0, 0x800000)
        cpu.mem_write(BASE, binary)
        cpu.mem_write(0x8000, b'\0'*8 + segment(0x9a) + segment(0x92)*5)
        cpu.reg_write(UC_X86_REG_GDTR, (0, 0x8000, 55, 0))
        cpu.reg_write(UC_X86_REG_CR0, 1)
        for reg, value in zip(SEGMENTS, (8, 16, 24, 32, 40, 48)):
            cpu.reg_write(reg, value)
        self.write('scheduler_descriptor', DESCRIPTOR)
        # Actual ABI constants are used in the assembly; these independently
        # laid-out bytes exercise the published 256-byte descriptor contract.
        cpu.mem_write(DESCRIPTOR, b'CVSC'+struct.pack('<HHII', 0x100, 256, 1, 7))
        self.ptes = struct.pack('<32I', *(0x170003+i*0x1000 for i in range(32)))
        cpu.mem_write(DESCRIPTOR+96, self.ptes)
        cpu.mem_write(PAGE_TABLE, self.ptes)
        cpu.mem_write(DESCRIPTOR+224, struct.pack('<I', 0x13579bdf))
        self.break_at_validator = False
        self.stopped_at_validator = False
        self.safe_gate_releases = 0
        cpu.hook_add(UC_HOOK_CODE, self.hook)
        cpu.hook_add(UC_HOOK_MEM_WRITE, self.write_hook)

    def read(self, address):
        if isinstance(address, str):
            address = self.symbols[address]
        return int.from_bytes(self.cpu.mem_read(address, 4), 'little')

    def write(self, address, value):
        if isinstance(address, str):
            address = self.symbols[address]
        self.cpu.mem_write(address, struct.pack('<I', value))

    def hook(self, cpu, address, size, user):
        if self.break_at_validator and address == self.symbols['scheduler_validate']:
            self.break_at_validator = False
            self.stopped_at_validator = True
            cpu.emu_stop()

    def registers(self):
        return {r: self.cpu.reg_read(r) for r in GPRS + SEGMENTS + (UC_X86_REG_EFLAGS,)}

    def write_hook(self, cpu, access, address, size, value, user):
        if address == self.symbols['scheduler_busy'] and value == 0:
            stack = cpu.reg_read(UC_X86_REG_ESP)
            assert not self.symbols['scheduler_stack'] <= stack < self.symbols['scheduler_stack_end'], \
                'busy gate released before restoring the caller stack'
            self.safe_gate_releases += 1

    def execute(self, entry, stop):
        self.cpu.emu_start(entry, stop, count=10000)

    def invoke(self, nested=False):
        cpu = self.cpu
        for reg, value in zip(GPRS, (0x13579bdf, 0x2468ace0, 0x1234fedc,
                                   0x76543210, 0x91827364, 0xfedcba98, 0xabcdef01)):
            cpu.reg_write(reg, value)
        cpu.reg_write(UC_X86_REG_EFLAGS, 0xed7)
        cpu.reg_write(UC_X86_REG_ESP, CALLER_STACK)
        cpu.mem_write(CALLER_STACK, struct.pack('<I', STOP))
        before = self.registers()
        self.break_at_validator = nested
        self.execute(self.symbols['vm_scheduler_irq'], STOP)
        if nested:
            assert self.stopped_at_validator, 'outer callback did not reach validator'
            outer_sp = cpu.reg_read(UC_X86_REG_ESP)
            assert self.symbols['scheduler_stack'] <= outer_sp < self.symbols['scheduler_stack_end']
            frame = bytes(cpu.mem_read(outer_sp, self.symbols['scheduler_stack_end']-outer_sp))
            saved_sp = self.read('scheduler_saved_esp')
            nested_before = self.registers()
            cpu.reg_write(UC_X86_REG_ESP, outer_sp-4)
            cpu.mem_write(outer_sp-4, struct.pack('<I', NESTED_STOP))
            self.execute(self.symbols['vm_scheduler_irq'], NESTED_STOP)
            assert cpu.reg_read(UC_X86_REG_EIP) == NESTED_STOP, 'nested callback did not return'
            assert cpu.reg_read(UC_X86_REG_ESP) == outer_sp, 'nested callback changed outer ESP'
            assert self.registers() == nested_before, 'nested callback changed GPR/segments/flags'
            assert self.read('scheduler_saved_esp') == saved_sp, 'nested callback overwrote saved outer ESP'
            assert bytes(cpu.mem_read(outer_sp, len(frame))) == frame, 'nested callback corrupted live outer frame'
            assert self.read('scheduler_internal_reentries') == 1
            assert self.read('scheduler_internal_ticks') == 1
            self.execute(self.symbols['scheduler_validate'], STOP)
        assert cpu.reg_read(UC_X86_REG_EIP) == STOP, 'outer callback did not return'
        assert cpu.reg_read(UC_X86_REG_ESP) == CALLER_STACK+4, 'outer callback changed caller stack'
        assert self.registers() == before, 'outer callback changed GPR/segments/flags'
        assert cpu.mem_read(self.symbols['scheduler_busy'], 1) == b'\0', 'callback left gate busy'
        assert bytes(cpu.mem_read(PAGE_TABLE, 128)) == self.ptes
        assert self.read(DESCRIPTOR+228) == 0x13579bdf


def qualify(binary, symbols):
    machine = Machine(binary, symbols)
    machine.invoke(nested=True)
    assert machine.read(DESCRIPTOR+36) == 1
    assert machine.read(DESCRIPTOR+24) == machine.read(DESCRIPTOR+32) == machine.read(DESCRIPTOR+64) == 1
    assert machine.read('scheduler_internal_reentries') == 0
    cases = ['nested_callback_preserves_live_outer_frame_GPR_segments_flags_and_saved_ESP',
             'one_reentry_recorded_with_one_outer_service_and_PTE_check']
    machine.invoke()
    assert machine.read(DESCRIPTOR+36) == 1
    assert machine.read(DESCRIPTOR+24) == machine.read(DESCRIPTOR+32) == machine.read(DESCRIPTOR+64) == 2
    assert machine.read('scheduler_internal_ticks') == 2
    assert machine.safe_gate_releases == 2
    cases.append('subsequent_callback_preserves_state_and_services_normally')
    cases.append('busy_gate_released_only_after_restoring_caller_stack')
    return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT/'src/vm/session_scheduler.inc')
    parser.add_argument('--output', type=Path, default=ROOT/'build/tests/scheduler-guard-2026-09-27/current')
    parser.add_argument('--jwasm', type=Path, default=ROOT/'build/external/JWasm/build/GccUnixR/jwasm')
    args = parser.parse_args()
    report = {'result': 'RUNNING', 'scope': 'assembled production routines; modeled descriptor/PTEs and nested CALL, not hardware IRQ qualification',
              'source': str(args.source), 'source_sha256': digest(args.source.read_bytes()),
              'jwasm_sha256': digest(args.jwasm.read_bytes()),
              'abi_sources': {name: digest((ROOT/'src/vm'/name).read_bytes())
                              for name in ('session_abi.inc', 'session_scheduler_abi.inc')}}
    try:
        binary, symbols, routine_hash = build(args.source, args.output, args.jwasm)
        report.update(binary_sha256=digest(binary), routines_sha256=routine_hash)
        report['cases'] = qualify(binary, symbols)
        report['result'] = 'PASS'
    except Exception as exc:
        report.update(result='FAIL', error=str(exc))
        raise
    finally:
        args.output.mkdir(parents=True, exist_ok=True)
        (args.output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
