#!/usr/bin/env python3
"""Run production wallpaper instructions for 15/16/24/32-bit formats.

CPU-only complement to qemu_test_wallpaper.py, not a BIOS or GPU test.
Run: uv run --with unicorn python scripts/test_wallpaper_pixels.py --output DIR
"""
import argparse
import hashlib
import json
import struct
import subprocess
from pathlib import Path

from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_INTR
from unicorn.x86_const import *


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fields = {'ui_vbe': 1, 'ui_comp_seg': 2, 'vc_bytes': 1, 'vc_info': 256,
              'ui_palette': 432, 'ui_composing': 1, 'ui_comp_top': 2,
              'ui_comp_bottom': 2, 'ui_comp_left': 2, 'ui_comp_right': 2,
              'ui_dialog': 1, 'ui_full_redraw': 1, 'ui_dialog_x': 2, 'ui_dialog_y': 2}
    names = ['stop', 'wp_color', 'wp_background', 'wp_seg', 'wp_width', 'wp_height', *fields]
    assembly = 'bits 16\norg 0x100\nVC_PALETTE_COLORS equ 144\n'
    assembly += 'ui_button: ret\nui_text: ret\nui_windows_open: ret\nstop: hlt\n'
    assembly += ''.join(f'{name}: times {size} db 0\n' for name, size in fields.items())
    assembly += '%include "src/com/shell_wallpaper.inc"\ndb "WPR1"\n'
    assembly += ''.join(f'dw {name}\ndb "{name}",0\n' for name in names) + 'dw 0\n'
    source = args.output / 'fixture.asm'
    target = args.output / 'fixture.bin'
    source.write_text(assembly)
    subprocess.run(['nasm', '-f', 'bin', str(source), '-o', str(target)], check=True)
    binary = target.read_bytes()
    position = binary.index(b'WPR1') + 4
    symbols = {}
    while address := struct.unpack_from('<H', binary, position)[0]:
        end = binary.index(0, position+2)
        symbols[binary[position+2:end].decode()] = address
        position = end + 1
    rgb = [(i, (i*73+19)&255, (i*151+47)&255) for i in range(256)]
    source_pixels = bytes(((x*7) ^ (y*11))&255 for y in range(256) for x in range(256))
    report = {'scope': 'Production NASM CPU instructions, not BIOS/GPU runtime',
              'source_sha256': hashlib.sha256(Path('src/com/shell_wallpaper.inc').read_bytes()).hexdigest(),
              'checks': []}
    formats = [(15, [(5,10),(5,5),(5,0)]), (16, [(5,11),(6,5),(5,0)]),
               (24, [(8,16),(8,8),(8,0)]), (32, [(8,16),(8,8),(8,0)]),
               (32, [(8,0),(8,8),(8,16)])]
    for depth, channels in formats:
        uc = Uc(UC_ARCH_X86, UC_MODE_16)
        uc.mem_map(0, 0x100000)
        uc.mem_write(0x10100, binary)
        uc.hook_add(UC_HOOK_INTR, lambda *_: (_ for _ in ()).throw(AssertionError('Paint invoked an interrupt')))

        def put(name, value, size=2):
            uc.mem_write(0x10000+symbols[name], value.to_bytes(size, 'little'))

        def call(name, **registers):
            for register in (UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_SS):
                uc.reg_write(register, 0x1000)
            uc.reg_write(UC_X86_REG_SP, 0xF000)
            uc.reg_write(UC_X86_REG_EFLAGS, 0x202)
            uc.mem_write(0x1F000, struct.pack('<H', symbols['stop']))
            for name_, value in registers.items():
                uc.reg_write(globals()['UC_X86_REG_' + name_.upper()], value)
            uc.emu_start(0x10000+symbols[name], 0x10000+symbols['stop'], count=3000000)
            assert uc.reg_read(UC_X86_REG_IP) == symbols['stop'], 'Instruction budget exhausted'

        size = (depth+7)//8
        put('vc_bytes', size, 1)
        info = bytearray(256)
        for index, pair in enumerate(channels):
            info[31+index*2:33+index*2] = bytes(pair)
        uc.mem_write(0x10000+symbols['vc_info'], bytes(info))
        uc.mem_write(0x30000, b''.join(bytes(color) for color in rgb))
        packed = [sum((color[index] >> (8-bits)) << shift
                      for index, (bits, shift) in enumerate(channels)) for color in rgb]
        for index in range(255, -1, -1):
            call('wp_color', es=0x3000, si=index*3, bp=index)
            actual = uc.reg_read(UC_X86_REG_EAX)
            assert actual == packed[index], (depth, index, actual, packed[index])
            assert uc.reg_read(UC_X86_REG_BP) == index
            uc.mem_write(0x30000+index*4, struct.pack('<I', actual))
        uc.mem_write(0x30400, source_pixels)
        put('wp_seg', 0x3000)
        put('wp_width', 256)
        put('wp_height', 256)
        put('ui_composing', 1, 1)
        put('ui_comp_seg', 0x5000)
        for left, top, width, height in [(255,255,317,5), (159,17,83,7), (0,0,0,3), (1,4,8,0)]:
            put('ui_comp_left', left)
            put('ui_comp_right', left+width)
            put('ui_comp_top', top)
            put('ui_comp_bottom', top+height)
            uc.mem_write(0x50000, b'\xA5'*65536)
            regs = {'eax': 0x01234567, 'ebx': 0x89ABCDEF, 'ecx': 0x13572468,
                    'edx': 0x24681357, 'esi': 0x67890123, 'edi': 0x56789012,
                    'ebp': 0x45678901, 'es': 0x2222, 'fs': 0x3333, 'gs': 0x4444}
            call('wp_background', **regs)
            for name, value in regs.items():
                assert uc.reg_read(globals()['UC_X86_REG_' + name.upper()]) == value, name
            assert uc.reg_read(UC_X86_REG_EFLAGS) & 0x200, 'Paint changed IF'
            expected = b''.join(packed[source_pixels[((y % 256)*256)+(x % 256)]].to_bytes(4, 'little')[:size]
                                for y in range(top, top+height) for x in range(left, left+width))
            assert bytes(uc.mem_read(0x50000, len(expected))) == expected, (depth,left,top)
            assert bytes(uc.mem_read(0x50000+len(expected), 65536-len(expected))) == b'\xA5'*(65536-len(expected)), 'Paint escaped band'
        report['checks'].append({'depth': depth, 'channel_masks': channels,
                                 'palette_entries_exact': 256, 'band_cases_exact': 4,
                                 'registers_and_segments_restored': True})
    (args.output/'result.json').write_text(json.dumps(report, indent=2)+'\n')
    print('[wallpaper-cpu] PASS', json.dumps(report['checks']))


if __name__ == '__main__':
    main()
