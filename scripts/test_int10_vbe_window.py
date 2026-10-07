#!/usr/bin/env python3
"""Execute the production INT 10h VBE mode/window handler in Unicorn.

The fixture is assembled from named source spans in floppy_stage1.asm. Only
the original BIOS far-call and local backing-store primitives are stubbed;
the production dispatch, mode bookkeeping, and bank-window logic stay intact.
"""
import argparse
from pathlib import Path
import re
import struct
import subprocess

from unicorn import Uc, UC_ARCH_X86, UC_MODE_16
from unicorn.x86_const import *

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x10000


def source_span(source, start, end):
    begin = source.index(start + ':\n')
    finish = source.index('\n' + end + ':\n', begin)
    return source[begin:finish]


def build_fixture(output):
    source = (ROOT/'src/boot/floppy_stage1.asm').read_text()
    handler = source_span(source, 'int10_handler', 'int10_call_original_bios')
    original_bios = source_span(source, 'int10_call_original_bios', 'int10_call_original_vbe')
    original_vbe = source_span(source, 'int10_call_original_vbe', 'int10_vbe_reset_state')
    mode_state = source_span(source, 'int10_vbe_reset_state', 'int10_vbe_activate_local_mode')
    activate_local = source_span(source, 'int10_vbe_activate_local_mode', 'int10_vbe_ensure_backing_store')
    local_bank = source_span(source, 'int10_vbe_set_window_local_bank', 'int10_vbe_clear_window')
    find_mode = source_span(source, 'int10_vbe_find_mode', 'int1a_handler')
    data_start = source.index('current_video_mode db 0x03\n')
    data_end = source.index('\nvbe_oem_string db 0', data_start)
    data = source[data_start:data_end]

    exports = (
        'stop int10_handler firmware_int10 firmware_success firmware_calls old_int10_seg '
        'current_video_mode current_vbe_mode current_vbe_bank_a current_vbe_bank_b '
        'current_vbe_mode_banks current_vbe_visible_bank current_vbe_visible_window '
        'current_vbe_backing_ready current_vbe_bios_mode local_ensure_calls '
        'local_save_calls local_load_calls a000_probe'
    ).split()
    header = [
        'bits 16', 'org 0x100', 'jmp stop', "db 'VEB5'",
        '%macro export 1', '    dw %1', '    db %str(%1),0', '%endmacro',
    ]
    header.extend(f'export {name}' for name in exports)
    header += [
        'dw 0', 'stop: hlt', '%define TRACE_CHILD_INT21 0',
        '%define VBE_BACKING_TARGET_PARAS 0',
        '%define VBE_BANK_WINDOW_PARAS 0x1000',
        '%define VBE_BANK_WINDOW_WORDS 0x8000',
        handler, original_bios, original_vbe, mode_state, local_bank, find_mode,
        # These routines only stand in for memory-manager and window-copy
        # services. The production local-mode activation stays in the fixture
        # because it clears DX while selecting bank zero.
        'int10_vbe_clear_backing_store:', '    ret',
        activate_local,
        'int10_vbe_ensure_backing_store:',
        '    inc byte [cs:local_ensure_calls]', '    ret',
        'int10_vbe_save_visible_bank:',
        '    inc byte [cs:local_save_calls]', '    ret',
        'int10_vbe_load_window_bank:',
        '    inc byte [cs:local_load_calls]', '    ret',
        'int10_vbe_clear_window:', '    ret',
        # The far-call target behaves like a BIOS response and returns through
        # the exact PUSHF/CALL FAR/IRET sequence used by production.
        'firmware_int10:',
        '    inc byte [cs:firmware_calls]',
        '    cmp byte [cs:firmware_success],0',
        '    jne .success',
        '    mov ax,0x014F', '    iret',
        '.success:', '    mov ax,0x004F', '    iret',
        'old_int10_off dw firmware_int10',
        'old_int10_seg dw 0',
        'firmware_success db 1', 'firmware_calls db 0',
        'local_ensure_calls db 0', 'local_save_calls db 0', 'local_load_calls db 0',
        'a000_probe times 8 db 0', 'vbe_oem_string db 0',
        data,
    ]
    fixture = output/'int10_vbe_window.asm'
    fixture.write_text('\n'.join(header)+'\n')
    binary = output/'int10_vbe_window.bin'
    subprocess.run(['nasm', '-f', 'bin', str(fixture), '-o', str(binary)],
                   cwd=ROOT, check=True)
    return binary.read_bytes()


class Machine:
    def __init__(self, binary):
        self.cpu = Uc(UC_ARCH_X86, UC_MODE_16)
        self.cpu.mem_map(0, 0x100000)
        self.cpu.mem_write(BASE+0x100, binary)
        at = binary.index(b'VEB5')+4
        self.symbols = {}
        while True:
            address, = struct.unpack_from('<H', binary, at)
            at += 2
            if address == 0:
                break
            end = binary.index(0, at)
            name = binary[at:end].decode()
            self.symbols[name] = BASE+address
            at = end+1
        self.code_segment = BASE//16
        self.set('old_int10_seg', self.code_segment)

    def set(self, name, value, size=2):
        self.cpu.mem_write(self.symbols[name], value.to_bytes(size, 'little'))

    def get(self, name, size=2):
        return int.from_bytes(self.cpu.mem_read(self.symbols[name], size), 'little')

    def run_int10(self, ax, bx=0, cx=0, dx=0):
        cpu = self.cpu
        for register in (UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS):
            cpu.reg_write(register, self.code_segment)
        sp = 0xEFF8
        cpu.reg_write(UC_X86_REG_SP, sp)
        cpu.reg_write(UC_X86_REG_EFLAGS, 0x202)
        cpu.reg_write(UC_X86_REG_AX, ax)
        cpu.reg_write(UC_X86_REG_BX, bx)
        cpu.reg_write(UC_X86_REG_CX, cx)
        cpu.reg_write(UC_X86_REG_DX, dx)
        stop_ip = self.symbols['stop']-BASE
        cpu.mem_write(self.code_segment*16+sp,
                      struct.pack('<HHH', stop_ip, self.code_segment, 0x0202))
        cpu.emu_start(self.symbols['int10_handler'], self.symbols['stop'], count=100000)
        assert cpu.reg_read(UC_X86_REG_CS) == self.code_segment
        assert cpu.reg_read(UC_X86_REG_IP) == stop_ip
        return (cpu.reg_read(UC_X86_REG_AX) & 0xFFFF,
                cpu.reg_read(UC_X86_REG_BX) & 0xFFFF,
                cpu.reg_read(UC_X86_REG_CX) & 0xFFFF,
                cpu.reg_read(UC_X86_REG_DX) & 0xFFFF)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    machine = Machine(build_fixture(output))

    # A successful firmware mode set must mark the current mode as BIOS-owned
    # and preserve DX while the local bookkeeping helper resolves BX's mode.
    machine.set('firmware_success', 1, 1)
    ax, _, _, dx = machine.run_int10(0x4F02, bx=0x0103, dx=0xBEEF)
    assert ax == 0x004F and dx == 0xBEEF, (hex(ax), hex(dx))
    assert machine.get('current_vbe_mode') == 0x0103
    assert machine.get('current_vbe_mode_banks') == 8
    assert machine.get('current_vbe_bios_mode', 1) == 1

    # BH is operation and BL is window. A BIOS-successful window-B set is
    # recorded as window B without invoking the local RAM-window emulation.
    ax, _, _, _ = machine.run_int10(0x4F05, bx=0x0001, dx=3)
    assert ax == 0x004F
    assert machine.get('current_vbe_bank_b') == 3
    assert machine.get('current_vbe_visible_bank') == 3
    assert machine.get('current_vbe_visible_window', 1) == 1
    assert machine.get('local_load_calls', 1) == 0

    # A failed BIOS bank switch while BIOS owns the mode must fail closed. It
    # cannot claim success by copying RAM into the still-unchanged A000 window.
    machine.set('firmware_success', 0, 1)
    a000 = 0xA0000
    machine.cpu.mem_write(a000, bytes.fromhex('1122334455667788'))
    ax, _, _, _ = machine.run_int10(0x4F05, bx=0x0001, dx=2)
    assert ax == 0x014F
    assert machine.get('current_vbe_bank_b') == 3
    assert machine.get('current_vbe_visible_bank') == 3
    assert machine.get('local_load_calls', 1) == 0
    assert machine.cpu.mem_read(a000, 8) == bytes.fromhex('1122334455667788')

    # Local banked modes may use the backing-store path. This also distinguishes
    # the operation byte in BH from the window byte in BL in both directions.
    machine.set('current_vbe_mode', 0x0103)
    machine.set('current_vbe_mode_banks', 8)
    machine.set('current_vbe_bank_a', 0)
    machine.set('current_vbe_bank_b', 0)
    machine.set('current_vbe_visible_bank', 0)
    machine.set('current_vbe_visible_window', 0, 1)
    machine.set('current_vbe_backing_ready', 1, 1)
    machine.set('current_vbe_bios_mode', 0, 1)
    ax, _, _, _ = machine.run_int10(0x4F05, bx=0x0001, dx=2)
    assert ax == 0x004F
    assert machine.get('current_vbe_bank_b') == 2
    assert machine.get('current_vbe_visible_bank') == 2
    assert machine.get('current_vbe_visible_window', 1) == 1
    assert machine.get('local_load_calls', 1) == 1
    ax, _, _, dx = machine.run_int10(0x4F05, bx=0x0101)
    assert ax == 0x004F and dx == 2, (hex(ax), hex(dx))
    ax, _, _, _ = machine.run_int10(0x4F05, bx=0x0201, dx=1)
    assert ax == 0x014F, hex(ax)

    # Firmware refusal cannot make the local emulator advertise an LFB mode.
    for name in ('current_vbe_mode', 'current_vbe_mode_banks', 'current_vbe_bank_a',
                 'current_vbe_bank_b', 'current_vbe_visible_bank'):
        machine.set(name, 0)
    machine.set('current_vbe_bios_mode', 0, 1)
    ax, _, _, _ = machine.run_int10(0x4F02, bx=0x4103)
    assert ax == 0x014F
    assert machine.get('current_vbe_mode') == 0
    assert machine.get('current_vbe_bios_mode', 1) == 0

    # A locally emulated banked mode also preserves DX on successful 4F02.
    machine.set('firmware_success', 0, 1)
    ax, _, _, dx = machine.run_int10(0x4F02, bx=0x0103, dx=0xBEEF)
    assert ax == 0x004F and dx == 0xBEEF, (hex(ax), hex(dx))
    assert machine.get('current_vbe_mode') == 0x0103
    assert machine.get('current_vbe_bios_mode', 1) == 0

    print('PASS extracted production INT10 VBE mode/window handler semantics')


if __name__ == '__main__':
    main()
