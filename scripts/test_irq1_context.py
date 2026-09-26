#!/usr/bin/env python3
"""Execute the real IRQ1 wrapper against a modeled legacy BIOS context leak.

The firmware fixture preserves 16-bit registers but clobbers their upper halves
and FS/GS, as a legacy handler can do without a 386-aware wrapper. This is an
instruction-level preservation check, not evidence of a specific laptop BIOS.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile

from unicorn import UC_HOOK_INSN
from unicorn.x86_const import *
from test_input_fallback import cpu, call, BASE, SEG
from test_ps2_boot import symbol


def execute(kernel, listing, bios, *, reentrant=False, flags=0x602):
    machine = cpu()
    machine.mem_write(BASE, kernel)
    address = lambda name: symbol(listing, name)
    machine.mem_write(BASE+address('old_int09_off'), struct.pack('<HH', 0x300, 0xF000))
    machine.mem_write(0xF0300, bios)
    machine.mem_write(9*4, struct.pack('<HH', address('int09_handler'), SEG))
    machine.mem_write(0x41A, struct.pack('<HH', 0x1E, 0x1E))
    machine.mem_write(0x480, struct.pack('<HH', 0x1E, 0x3E))
    if reentrant:
        machine.mem_write(BASE+address('irq1_external_owner'), b'\x80')
    machine.hook_add(UC_HOOK_INSN, lambda *args: 1, None, 1, 0, UC_X86_INS_IN)
    registers = (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX,
                 UC_X86_REG_EDX, UC_X86_REG_ESI, UC_X86_REG_EDI,
                 UC_X86_REG_EBP, UC_X86_REG_DS, UC_X86_REG_ES,
                 UC_X86_REG_FS, UC_X86_REG_GS)
    expected = [0x12341000+i for i in range(7)] + [0x1357, 0x2468, 0x3456, 0x4567]
    for register, value in zip(registers, expected):
        machine.reg_write(register, value)
    call(machine, address('int09_handler'), flags=flags, interrupt=True)
    actual = [machine.reg_read(register) for register in registers]
    assert actual == expected, {'expected': [hex(x) for x in expected],
                                'actual': [hex(x) for x in actual]}
    assert machine.reg_read(UC_X86_REG_EFLAGS) & 0x600 == flags & 0x600
    owner = machine.mem_read(BASE+address('irq1_external_owner'), 1)[0]
    assert owner == (0x80 if reentrant else 0), 'nested IRQ changed outer ownership'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', required=True, type=Path)
    parser.add_argument('--listing', required=True, type=Path)
    parser.add_argument('--old-kernel', type=Path)
    parser.add_argument('--old-listing', type=Path)
    parser.add_argument('--report', required=True, type=Path)
    args = parser.parse_args()
    source = 'bits 16\n'
    for name in ('ax', 'bx', 'cx', 'dx', 'si', 'di', 'bp'):
        source += f'push {name}\nmov e{name},0xA5A55A5A\npop {name}\n'
    source += 'push ax\nmov ax,0x5555\nmov fs,ax\nmov ax,0x6666\nmov gs,ax\npop ax\niret\n'
    with tempfile.TemporaryDirectory(prefix='ciukios-irq1-') as work:
        path = Path(work)
        (path/'bios.asm').write_text(source)
        subprocess.run(['nasm', '-f', 'bin', str(path/'bios.asm'), '-o', str(path/'bios.bin')], check=True)
        bios = (path/'bios.bin').read_bytes()
    negative = None
    if args.old_kernel:
        try:
            execute(args.old_kernel.read_bytes(), args.old_listing.read_text(), bios)
        except AssertionError as error:
            negative = str(error)
        assert negative, 'old negative control unexpectedly preserved the fixture context'
    cases = []
    for nested in (False, True):
        for flags in (0x002, 0x202, 0x602):
            execute(args.kernel.read_bytes(), args.listing.read_text(), bios,
                    reentrant=nested, flags=flags)
            cases.append({'nested': nested, 'flags': flags})
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps({'status': 'pass', 'cases': cases,
        'old_negative_control': negative,
        'scope': 'Production IRQ1 assembly; modeled hostile legacy BIOS, not physical hardware'}, indent=2)+'\n')
    print(f'IRQ1 context: {len(cases)} cases passed')


if __name__ == '__main__':
    main()
