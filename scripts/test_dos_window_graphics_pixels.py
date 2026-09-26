#!/usr/bin/env python3
"""Exact production 320x200/2x band instructions; no BIOS/GPU/FPS claim."""
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
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    names = ['cg_draw_band', 'cg_descriptor', 'cg_native_palette',
             'DW_CELL_LEFT', 'DW_CELL_RIGHT', 'DW_CELL_TOP', 'DW_CELL_BOTTOM',
             'DW_CELL_STRIDE', 'DW_CELL_BYTES', 'DW_CELL_BAND_SEG']
    asm = '''bits 16
org 0
%include "src/com/dos_window_abi.inc"
times DW_HEADER_BYTES db 0
%include "src/com/dos_window_graphics_draw.inc"
cg_descriptor times 48 db 0
cg_native_palette times 256 dd 0
db "CGTEST1"
'''
    asm += ''.join(f'dw {name}\ndb "{name}",0\n' for name in names)+'dw 0\n'
    (args.output/'fixture.asm').write_text(asm)
    subprocess.run(['nasm', '-f', 'bin', str(args.output/'fixture.asm'), '-o', str(args.output/'fixture.bin')], check=True)
    binary = (args.output/'fixture.bin').read_bytes()
    pos = binary.index(b'CGTEST1')+7
    symbols = {}
    while address := struct.unpack_from('<H', binary, pos)[0]:
        end = binary.index(0, pos+2)
        symbols[binary[pos+2:end].decode()] = address
        pos = end+1
    uc = Uc(UC_ARCH_X86, UC_MODE_16)
    uc.mem_map(0, 0x100000)
    uc.mem_write(0x20000, binary)
    uc.mem_write(0x10100, b'\x9A'+struct.pack('<HH',symbols['cg_draw_band'],0x2000)+b'\xF4')
    uc.hook_add(UC_HOOK_INTR, lambda *_: (_ for _ in ()).throw(AssertionError('Interrupt during paint')))

    def put(name, value, size=2):
        uc.mem_write(0x20000+symbols[name], value.to_bytes(size, 'little'))

    source = bytes(((x*17) ^ (y*43) ^ (x>>3))&255 for y in range(200) for x in range(320))
    image_memory = bytearray(b'\xC3'*65536)
    image_memory[:64000] = source
    uc.mem_write(0x40000, bytes(image_memory))
    uc.mem_write(0x20000+symbols['cg_descriptor']+12, struct.pack('<HH',0,0x4000))
    put('DW_CELL_BAND_SEG', 0x6000)
    # All active bands fit the actual60KiB allocation, including row padding.
    cases = [(0,0,16,8), (96,72,650,16), (101,78,17,9),
             (102,79,16,8), (99,76,4,4), (739,475,5,5),
             (740,100,6,6), (100,477,640,2), (105,85,0,7),
             (105,85,9,0), (100,77,1,1), (101,78,1,1),
             (100,476,640,1), (100,77,640,20), (103,78,3,3)]
    report = {'scope': 'Exact production CPU instructions, not firmware/GPU or measured frame rate',
              'source_sha256': hashlib.sha256(Path('src/com/dos_window_graphics_draw.inc').read_bytes()).hexdigest(),
              'cases': [], 'all_registers_segments_and_if_preserved': True}
    for size in (1,2,3,4):
        colors = [(0xAD000000|((i*0x912B47)^0x31BFA9))&0xFFFFFFFF for i in range(256)]
        uc.mem_write(0x20000+symbols['cg_native_palette'], b''.join(struct.pack('<I',c) for c in colors))
        put('DW_CELL_BYTES', size, 1)
        for left, top, width, height in cases:
            stride = max(width*size+3, 4)
            assert stride*height <= 61440
            for field, value in [('LEFT',left),('RIGHT',left+width),('TOP',top),
                                 ('BOTTOM',top+height),('STRIDE',stride)]:
                put('DW_CELL_'+field, value)
            uc.mem_write(0x60000, b'\xA5'*65536)
            initial = dict(EAX=0x12345678, EBX=0xABCD0064, ECX=0x87654321,
                           EDX=0x5678004D, ESI=0x24681357, EDI=0x13572468,
                           EBP=0xF0F1F2F3, DS=0x1000, ES=0x3000, FS=0x2000, GS=0x5555)
            for reg, value in initial.items():
                uc.reg_write(globals()['UC_X86_REG_'+reg], value)
            uc.reg_write(UC_X86_REG_CS, 0x1000)
            uc.reg_write(UC_X86_REG_SS, 0x8000)
            uc.reg_write(UC_X86_REG_SP, 0xF000)
            uc.reg_write(UC_X86_REG_EFLAGS, 0x202)
            uc.emu_start(0x10100, 0x10105, count=1000000)
            assert uc.reg_read(UC_X86_REG_IP) == 0x105, 'Instruction budget exhausted'
            for reg, value in initial.items():
                assert uc.reg_read(globals()['UC_X86_REG_'+reg]) == value, reg
            assert uc.reg_read(UC_X86_REG_EFLAGS) & 0x200
            expected = bytearray(b'\xA5'*65536)
            for y in range(max(top,77),min(top+height,477)):
                for x in range(max(left,100),min(left+width,740)):
                    color = colors[source[((y-77)//2)*320+(x-100)//2]]
                    off = (y-top)*stride+(x-left)*size
                    expected[off:off+size] = color.to_bytes(4,'little')[:size]
            actual = bytes(uc.mem_read(0x60000,65536))
            assert actual == expected, (size,left,top,width,height,
                next((i for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),None))
            assert bytes(uc.mem_read(0x40000,65536)) == image_memory, 'Source framebuffer changed'
            report['cases'].append(dict(bytes_per_pixel=size,band=[left,top,width,height],stride=stride,exact=True))
    report['passed'] = True
    (args.output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print(f'[graphics-pixels] PASS {len(report["cases"])} exact bands; registers/segments/IF and guards preserved')


if __name__ == '__main__':
    main()
