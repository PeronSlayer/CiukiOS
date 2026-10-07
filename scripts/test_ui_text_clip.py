#!/usr/bin/env python3
"""Run production ui_text/ui_glyph/compositor glyph code in Unicorn.

The test extracts real NASM routine ranges from the shell sources. It checks
right-edge scene text, band clipping, and the retained direct-planar guard.
This is a CPU-level regression, not a complete shell or GPU test.
Run: uv run --with unicorn python scripts/test_ui_text_clip.py --output DIR
"""
import argparse
import hashlib
import json
import re
import struct
import subprocess
from pathlib import Path

from unicorn import Uc, UC_ARCH_X86, UC_MODE_16
from unicorn.x86_const import *

ROOT = Path(__file__).resolve().parents[1]


def routine(path, label, next_label):
    source = path.read_text()
    start = re.search(rf'^{re.escape(label)}:\s*$', source, re.M)
    if not start:
        raise RuntimeError(f'could not find {label} in {path}')
    end = re.search(rf'^{re.escape(next_label)}:\s*$', source[start.end():], re.M)
    if not end:
        raise RuntimeError(f'could not extract {label}..{next_label} from {path}')
    return source[start.start():start.end() + end.start()]


def fixture_source():
    draw = ROOT / 'src/com/shell_gui_draw.inc'
    compositor = ROOT / 'src/com/shell_gui_compositor.inc'
    code = routine(draw, 'ui_text', 'ui_glyph') + '\n' + routine(draw, 'ui_glyph', 'ui_bevel')
    code += '\n' + routine(compositor, 'ui_comp_glyph', 'ui_comp_present')
    exports = [
        'stop', 'ui_text', 'ui_composing', 'ui_vbe', 'ui_width', 'ui_height',
        'ui_comp_seg', 'ui_comp_top', 'ui_comp_bottom', 'ui_comp_left',
        'ui_comp_right', 'ui_comp_stride', 'ui_assets_seg', 'ug_text_color',
        'ui_font_ptr', 'ui_advance', 'vc_bytes', 'vc_colors', 'planar_calls',
    ]
    asm = ['bits 16', 'org 0x100', 'jmp stop', "db 'UTC1'",
           '%define UI_FONT_BYTES (95+95*32)', '%define UI_ASSET_FONT 16',
           '%macro export 1', 'dw %1', 'db %str(%1),0', '%endmacro']
    asm += [f'export {name}' for name in exports]
    asm += ['dw 0', 'stop: hlt', 'ug_glyph: inc word [planar_calls]', 'ret',
            'vc_put_pixel: ret', 'vc_put_index: ret', code,
            'ui_composing: db 1', 'ui_vbe: db 1',
            'ui_width: dw 1280', 'ui_height: dw 800',
            'ui_comp_seg: dw 0x5000', 'ui_comp_top: dw 0', 'ui_comp_bottom: dw 0',
            'ui_comp_left: dw 0', 'ui_comp_right: dw 0', 'ui_comp_stride: dw 0',
            'ui_assets_seg: dw 0x3000', 'ug_text_color: db 0', 'ui_font_ptr: dw 0',
            'ui_advance: db 0', 'vc_bytes: db 4', 'vc_pitch: dw 0', 'vc_colors: times 64 dd 0',
            'ui_comp_glyph_x: dw 0', 'planar_calls: dw 0']
    return '\n'.join(asm) + '\n'


class TextCPU:
    def __init__(self, binary):
        self.uc = Uc(UC_ARCH_X86, UC_MODE_16)
        self.uc.mem_map(0, 0x100000)
        self.uc.mem_write(0x10100, binary)
        self.symbols = {}
        cursor = binary.index(b'UTC1') + 4
        while address := struct.unpack_from('<H', binary, cursor)[0]:
            end = binary.index(b'\0', cursor + 2)
            self.symbols[binary[cursor + 2:end].decode()] = 0x10000 + address
            cursor = end + 1
        self.set('ui_assets_seg', 0x3000)
        self.set('ui_width', 1280)
        self.set('ui_height', 800)
        self.uc.mem_write(0x1D000, b'A\0')
        font = bytearray(95 + 95 * 32)
        font[ord('A') - 32] = 13
        glyph = 95 + (ord('A') - 32) * 32
        for row in range(16):
            struct.pack_into('<H', font, glyph + row * 2, 0x8001)
        self.uc.mem_write(0x30000 + 16, bytes(font))

    def set(self, name, value, size=2):
        self.uc.mem_write(self.symbols[name], int(value).to_bytes(size, 'little'))

    def get(self, name, size=2):
        return int.from_bytes(self.uc.mem_read(self.symbols[name], size), 'little')

    def call(self, x, y, composing, vbe=1):
        uc = self.uc
        for reg in (UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS):
            uc.reg_write(reg, 0x1000)
        uc.reg_write(UC_X86_REG_FS, 0x1111)
        uc.reg_write(UC_X86_REG_SP, 0xEFFC)
        uc.reg_write(UC_X86_REG_EFLAGS, 2)
        uc.mem_write(0x1EFFC, struct.pack('<H', self.symbols['stop'] - 0x10000))
        uc.reg_write(UC_X86_REG_BX, x)
        uc.reg_write(UC_X86_REG_DX, y)
        uc.reg_write(UC_X86_REG_SI, 0xD000)
        uc.reg_write(UC_X86_REG_AX, 5)  # colour 5, regular font
        self.set('ui_composing', composing, 1)
        self.set('ui_vbe', vbe, 1)
        uc.emu_start(self.symbols['ui_text'], self.symbols['stop'], count=2000000)
        assert uc.reg_read(UC_X86_REG_IP) == self.symbols['stop'] - 0x10000, 'ui_text did not return'


def packed_color(depth):
    color = 0x00D4A15B
    return color.to_bytes(4, 'little')[:(depth + 7) // 8]


def check_band(cpu, depth, x, band_left, band_right, band_top, band_bottom):
    bytes_per_pixel = (depth + 7) // 8
    stride = (band_right - band_left) * bytes_per_pixel
    cpu.set('ui_comp_left', band_left)
    cpu.set('ui_comp_right', band_right)
    cpu.set('ui_comp_top', band_top)
    cpu.set('ui_comp_bottom', band_bottom)
    cpu.set('ui_comp_stride', stride)
    cpu.set('ui_comp_seg', 0x5000)
    cpu.set('vc_bytes', bytes_per_pixel, 1)
    cpu.set('ug_text_color', 5, 1)
    color_table = bytearray(64 * 4)
    struct.pack_into('<I', color_table, 5 * 4, 0x00D4A15B)
    cpu.uc.mem_write(cpu.symbols['vc_colors'], bytes(color_table))
    cpu.uc.mem_write(0x50000, b'\xA5' * 65536)
    cpu.call(x, 100, composing=1)

    expected = bytearray(b'\xA5' * 65536)
    color = packed_color(depth)
    for row in range(16):
        y = 100 + row
        if not band_top <= y < band_bottom:
            continue
        for col in (0, 15):
            px = x + col
            if band_left <= px < band_right and px < 1280:
                offset = (y - band_top) * stride + (px - band_left) * bytes_per_pixel
                expected[offset:offset + bytes_per_pixel] = color
    actual = bytes(cpu.uc.mem_read(0x50000, 65536))
    assert actual == expected, (
        f'{depth}-bit x={x} band={band_left}:{band_right} y={band_top}:{band_bottom}: '
        f'first mismatch {next((i for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),None)}'
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    asm_path = args.output / 'ui_text_clip.asm'
    binary_path = args.output / 'ui_text_clip.bin'
    asm_path.write_text(fixture_source())
    subprocess.run(['nasm', '-f', 'bin', str(asm_path), '-o', str(binary_path)], cwd=ROOT, check=True)
    results = []

    for depth in (15, 16, 24, 32):
        cpu = TextCPU(binary_path.read_bytes())
        for x, left, right, top, bottom in (
            (1258, 1258, 1280, 100, 116),  # right-edge glyph is visible
            (1258, 1265, 1280, 100, 116),  # left side clipped by this band
            (1268, 1268, 1280, 100, 116),  # glyph's right half clipped at screen edge
            (1258, 1258, 1280, 107, 112),  # vertical band clips five rows
        ):
            check_band(cpu, depth, x, left, right, top, bottom)
        results.append(f'{depth}-bit screen/band clipping')

    # The non-composited planar path still needs its wider three-byte safety
    # margin. The near-edge character must be skipped; one pixel inward calls
    # the planar writer. The writer is stubbed and counted, not mirrored.
    cpu = TextCPU(binary_path.read_bytes())
    cpu.set('planar_calls', 0)
    cpu.call(1258, 100, composing=0, vbe=0)
    assert cpu.get('planar_calls') == 0, 'edge planar glyph bypassed the 24-pixel guard'
    cpu.call(1256, 100, composing=0, vbe=0)
    assert cpu.get('planar_calls') == 1, 'planar guard rejected a glyph whose footprint fits'
    results.append('planar 24-pixel footprint guard retained')

    report = {
        'scope': 'Actual NASM routine ranges; DOS/VBE hardware not exercised',
        'sources': {
            'shell_gui_draw.inc': hashlib.sha256(
                (ROOT / 'src/com/shell_gui_draw.inc').read_bytes()).hexdigest(),
            'shell_gui_compositor.inc': hashlib.sha256(
                (ROOT / 'src/com/shell_gui_compositor.inc').read_bytes()).hexdigest(),
        },
        'checks': results,
    }
    (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS ui_text composition clipping:', '; '.join(results))


if __name__ == '__main__':
    main()
