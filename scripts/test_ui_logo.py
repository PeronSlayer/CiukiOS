#!/usr/bin/env python3
"""Execute the production Ciuki sprite reader and check pixels, bands and ABI.

Run with: uv run --with unicorn python scripts/test_ui_logo.py
The rectangle callback is simulated; QEMU separately checks final UI pixels.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_CODE, UC_HOOK_MEM_READ
from unicorn.x86_const import *

ROOT = Path(__file__).resolve().parents[1]


def signed(value):
    return value if value < 32768 else value - 65536


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path,
                        default=ROOT/'build/full/ciuki-logo-approved-2026-09-25/logo-cpu')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = args.output/'logo.bin'
    subprocess.run(['nasm', '-f', 'bin', 'scripts/fixtures/ui_logo_cpu.asm',
                    '-o', str(fixture)], cwd=ROOT, check=True)
    code = fixture.read_bytes()
    (e16, e24, e64, callback, vbe, composing, top, bottom, origin_y,
     p16, p24, p64, fallback_ptr) = struct.unpack_from('<13H', code)
    fallback = (ROOT/'assets/brand/native/fallback.bin').read_bytes()
    assert len(fallback) == 64 and max(fallback) < 16
    assert code[fallback_ptr:fallback_ptr+64] == fallback
    report = []
    for size, entry, source in ((16,e16,p16), (24,e24,p24), (64,e64,p64)):
        pixels = (ROOT/f'assets/brand/native/ciuki-{size}.bin').read_bytes()
        assert code[source:source+size*size] == pixels
        assert all(value == 255 or 16 <= value < 80 for value in pixels)
        for packed in (False, True):
            for ox,oy,viewport_y in ((5,2,0), (-75,-58,60), (300,90,0)):
                for compose, band in ((False,(0,600)), (True,(0,600)),
                                      (True,(oy+viewport_y+3,oy+viewport_y+9)),
                                      (True,(oy+viewport_y-size,oy+viewport_y)),
                                      (True,(oy+viewport_y+size,oy+viewport_y+size+8))):
                    uc = Uc(UC_ARCH_X86,UC_MODE_16)
                    uc.mem_map(0,0x100000)
                    uc.mem_write(0x10000,code)
                    uc.mem_write(0x10000+vbe,bytes([int(packed)]))
                    uc.mem_write(0x10000+composing,bytes([int(compose)]))
                    for address,value in ((top,band[0]), (bottom,band[1]), (origin_y,viewport_y)):
                        uc.mem_write(0x10000+address,struct.pack('<H',value&65535))
                    # Set nontrivial upper halves, a separate stack segment,
                    # and DF=1 to expose accidental register/string-ABI changes.
                    regs = {UC_X86_REG_EAX:0x12345678,
                            UC_X86_REG_EBX:0xABCD0000|(ox&65535),
                            UC_X86_REG_ECX:0x88889999,
                            UC_X86_REG_EDX:0xABEF0000|(oy&65535),
                            UC_X86_REG_ESI:0x77778888, UC_X86_REG_EDI:0x55556666,
                            UC_X86_REG_EBP:0x33334444, UC_X86_REG_ESP:0xFFF0,
                            UC_X86_REG_DS:0x1000, UC_X86_REG_ES:0x4000,
                            UC_X86_REG_CS:0x1000, UC_X86_REG_SS:0x7000,
                            UC_X86_REG_FS:0x5000, UC_X86_REG_GS:0x6000,
                            UC_X86_REG_EFLAGS:0xE43}
                    for reg,value in regs.items():
                        uc.reg_write(reg,value)
                    visible_rows = {y for y in range(size)
                                    if not compose or band[0] <= oy+y+viewport_y < band[1]}
                    expected = {}
                    for y in visible_rows:
                        for x in range(size):
                            value = pixels[y*size+x]
                            if value != 255:
                                expected[x,y] = value if packed else fallback[value-16]
                    seen = {}; runs = []; reads = []

                    def draw(cpu,address,instruction_size,data):
                        if address != 0x10000+callback:
                            return
                        x = signed(cpu.reg_read(UC_X86_REG_BX))
                        y = signed(cpu.reg_read(UC_X86_REG_DX))
                        width = cpu.reg_read(UC_X86_REG_CX)
                        height = cpu.reg_read(UC_X86_REG_SI)
                        color = cpu.reg_read(UC_X86_REG_AL)
                        assert height == 1 and 0 < width <= size
                        assert 0 <= x-ox < size and x-ox+width <= size
                        assert y-oy in visible_rows
                        assert (16 <= color < 80) if packed else (0 <= color < 16)
                        for px in range(x,x+width):
                            assert (px-ox,y-oy) not in seen, 'overlapping sprite runs'
                            seen[px-ox,y-oy] = color
                        runs.append((x,y,width,color))
                        # Rectangle primitives preserve registers, not flags.
                        cpu.reg_write(UC_X86_REG_EFLAGS,0x202)

                    def read(cpu,access,address,read_size,value,data):
                        relative = address - 0x10000 - source
                        assert 0 <= relative and relative+read_size <= size*size
                        assert all((relative+i)//size in visible_rows for i in range(read_size))
                        reads.append(relative)

                    uc.hook_add(UC_HOOK_CODE,draw)
                    uc.hook_add(UC_HOOK_MEM_READ,read,
                                begin=0x10000+source, end=0x10000+source+size*size-1)
                    uc.emu_start(0x10000+entry,0x10000+len(code),count=500000)
                    assert uc.reg_read(UC_X86_REG_IP) == entry+4, 'sprite did not return through HLT'
                    assert seen == expected, (size,packed,ox,oy,compose,band,'pixel mismatch')
                    for reg,value in regs.items():
                        assert uc.reg_read(reg) == value, (size,reg,uc.reg_read(reg),value)
                    if not visible_rows:
                        assert not runs and not reads
                    # Apart from caller-provided fixture variables, code and
                    # embedded asset bytes must remain immutable.
                    assert bytes(uc.mem_read(0x10000+source,len(pixels))) == pixels
                    report.append({'size':size, 'packed':packed, 'origin':[ox,oy],
                                   'viewport_y':viewport_y, 'composing':compose,
                                   'band':list(band), 'pixels':len(seen), 'runs':len(runs),
                                   'source_reads':len(reads), 'preserves_full_abi':True,
                                   'matches_compiled_source':True})
        print(f'PASS {size}px source sprite: packed/planar, origins, band skips, registers/flags',flush=True)
    (args.output/'results.json').write_text(json.dumps(
        {'passed':True, 'scope':'assembled sprite reader with simulated rectangle callback',
         'fixture_sha256':hashlib.sha256(code).hexdigest(),
         'source_sha256':hashlib.sha256((ROOT/'src/com/ciuki_logo.inc').read_bytes()).hexdigest(),
         'checks':report},indent=2)+'\n')


if __name__ == '__main__':
    main()
