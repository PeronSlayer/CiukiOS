#!/usr/bin/env python3
"""Check the actual VBE palette instructions, not a rewritten renderer.

Run with: uv run --with unicorn python scripts/test_ui_palette.py
This checks CPU conversion, bounds, and BIOS payloads; it is not a GPU test.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

from unicorn import UC_HOOK_MEM_READ
from unicorn.x86_const import UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_CX, UC_X86_REG_DX, UC_X86_REG_ES

from test_ui_rendering import Renderer, ROOT


class Palette(Renderer):
    def __init__(self, binary):
        super().__init__(binary)
        self.dac_calls = []
        self.source_reads = []
        self.uc.hook_add(UC_HOOK_MEM_READ, self.read_palette,
                         begin=self.symbols['test_palette'],
                         end=self.symbols['test_palette_end'] + 15)

    def read_palette(self, uc, access, address, size, value, data):
        self.source_reads.append((address, size))

    def interrupt(self, uc, number, data):
        assert number == 0x10
        assert uc.reg_read(UC_X86_REG_AX) == 0x1012
        count = uc.reg_read(UC_X86_REG_CX)
        address = uc.reg_read(UC_X86_REG_ES) * 16 + uc.reg_read(UC_X86_REG_DX)
        self.dac_calls.append({'start': uc.reg_read(UC_X86_REG_BX),
                               'count': count,
                               'payload': bytes(uc.mem_read(address, count * 3))})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path,
                        default=ROOT/'build/full/ciuki-logo-approved-2026-09-25/palette-cpu')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    results = []
    for limit in (16, 80, 144):
        path = args.output/f'palette-{limit}.bin'
        subprocess.run(['nasm', '-f', 'bin', 'scripts/fixtures/ui_palette_cpu.asm',
                        f'-DTEST_PALETTE_LIMIT={limit}', '-o', str(path)], cwd=ROOT, check=True)
        binary = path.read_bytes()
        for depth in (8, 15, 16, 24, 32):
            r = Palette(binary)
            source = r.symbols['test_palette']
            rgb6 = bytes(r.uc.mem_read(source, limit * 3))
            assert len(rgb6) == limit * 3 and max(rgb6) <= 63
            r.set('vc_bytes', (depth + 7) // 8, 1)
            channels = ((5, 10, 5, 5, 5, 0) if depth == 15 else
                        (5, 11, 6, 5, 5, 0) if depth == 16 else
                        (8, 16, 8, 8, 8, 0))
            r.uc.mem_write(r.symbols['vc_info'] + 31, bytes(channels))
            target = r.symbols['vc_colors']
            end = r.symbols['vc_controller']
            assert end - target == limit * 4
            guard = b'PALETTE_GUARD_123'
            r.uc.mem_write(end, guard)
            r.call('vc_set_palette', cx=limit, bx=0, dx=source-0x10000)
            actual = struct.unpack('<' + 'I'*limit, r.uc.mem_read(target, limit*4))
            expected = []
            for i in range(limit):
                native = 0
                if depth == 8:
                    native = i
                else:
                    for c in range(3):
                        rgb8 = rgb6[i*3+c] * 4 + rgb6[i*3+c] // 16
                        native |= (rgb8 // (2 ** (8-channels[c*2]))) * (2 ** channels[c*2+1])
                expected.append(native)
            assert actual == tuple(expected), (limit, depth, actual, expected)
            assert bytes(r.uc.mem_read(end, len(guard))) == guard
            assert all(address + size <= source + limit*3 for address,size in r.source_reads)
            if depth == 8:
                assert r.dac_calls == [{'start': 0, 'count': limit, 'payload': rgb6}]
            else:
                assert not r.dac_calls
            # The initial console palette remains a 16-color call even when
            # the desktop LUT has room for the larger source-image palette.
            if limit > 16:
                sentinel = bytes([0xA5]) * ((limit - 16) * 4)
                r.uc.mem_write(target + 16*4, sentinel)
                r.call('vc_set_palette', cx=16, bx=0, dx=source-0x10000)
                assert bytes(r.uc.mem_read(target+16*4, len(sentinel))) == sentinel
                if depth == 8:
                    assert r.dac_calls[-1] == {'start': 0, 'count': 16, 'payload': rgb6[:48]}
            # Every rejected request must leave all table bytes and DAC alone.
            for count, start in ((0, 0), (limit+1, 0), (65535, 0), (16, 1)):
                before = bytes(r.uc.mem_read(target, limit*4 + len(guard)))
                calls = len(r.dac_calls)
                reads = len(r.source_reads)
                r.call('vc_set_palette', cx=count, bx=start, dx=source-0x10000)
                assert bytes(r.uc.mem_read(target, len(before))) == before
                assert len(r.dac_calls) == calls
                assert len(r.source_reads) == reads
            results.append({'palette_limit': limit, 'depth': depth, 'passed': True,
                            'fixture_sha256': hashlib.sha256(binary).hexdigest()})
            print(f'PASS {limit} colors, {depth} bits: conversion, bounds, DAC, invalid requests', flush=True)
    report = {'passed': True, 'scope': 'assembled CPU instructions and simulated INT 10h',
              'cases': results,
              'source_sha256': {name: hashlib.sha256((ROOT/name).read_bytes()).hexdigest()
                                for name in ('src/com/vbe_modes.inc', 'src/com/vbe_console.inc',
                                             'src/com/ui_theme.inc', 'src/com/ciuki_logo_palette.inc',
                                             'src/com/ui_icons_palette.inc')}}
    (args.output/'results.json').write_text(json.dumps(report, indent=2)+'\n')


if __name__ == '__main__':
    main()
