#!/usr/bin/env python3
"""Exercise the production Mode 12h band presenter at the CPU/plane level."""
from pathlib import Path
import subprocess

from unicorn import UC_HOOK_INSN, UC_HOOK_MEM_WRITE
from unicorn.x86_const import UC_X86_INS_OUT, UC_X86_REG_FS

from test_ui_rendering import ROOT, Renderer


def main():
    target = ROOT / 'build/full/ui-vga-rendering/renderer.bin'
    target.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['nasm', '-f', 'bin', 'scripts/fixtures/ui_rendering_cpu.asm',
                    '-o', str(target)], cwd=ROOT, check=True)
    r = Renderer(target.read_bytes())
    r.set('ui_vbe', 0, 1)
    r.set('ui_width', 640)
    r.set('ui_height', 480)
    r.set('ui_comp_seg', 0x5000)
    planes = [bytearray([0xA5] * 38400) for _ in range(4)]
    mask = [0x0F]
    gc = {}

    def out(_uc, port, size, value, _user):
        if port == 0x3C4 and size == 2:
            assert value & 255 == 2
            mask[0] = value >> 8
        elif port == 0x3CE and size == 2:
            gc[value & 255] = value >> 8
        else:
            raise AssertionError((port, size, value))

    def write(_uc, _access, address, size, value, _user):
        assert size == 1
        assert gc.get(1) == 0 and gc.get(3) == 0
        assert gc.get(5) == 0 and gc.get(8) == 255
        offset = address - 0xA0000
        for p in range(4):
            if mask[0] & (1 << p):
                planes[p][offset] = value & 255

    r.uc.hook_add(UC_HOOK_INSN, out, None, 1, 0, UC_X86_INS_OUT)
    r.uc.hook_add(UC_HOOK_MEM_WRITE, write, begin=0xA0000, end=0xAFFFF)

    # The painter must clip both axes and leave scratch guards untouched.
    r.set('ui_comp_left', 8)
    r.set('ui_comp_right', 24)
    r.set('ui_comp_top', 2)
    r.set('ui_comp_bottom', 4)
    r.call('ui_comp_band_setup')
    assert r.get('ui_comp_stride') == 16
    r.uc.mem_write(0x50000, b'\xE7' * 65536)
    r.call('test_comp_rect', ax=13, bx=12, dx=1, cx=8, si=4)
    band = r.uc.mem_read(0x50000, 65536)
    assert band[:32] == bytes([0xE7] * 4 + [13] * 8 + [0xE7] * 4) * 2
    assert band[32:] == b'\xE7' * (65536 - 32)
    r.uc.mem_write(0x50000, b'\xE7' * 65536)
    r.uc.mem_write(0x1D000, b'\xFF' * 32)
    r.uc.reg_write(UC_X86_REG_FS, 0x1000)
    r.set('ug_text_color', 2, 1)
    r.call('ui_comp_glyph', bx=4, dx=1, si=0xD000)
    band = r.uc.mem_read(0x50000, 65536)
    assert band[:32] == bytes([2] * 12 + [0xE7] * 4) * 2
    assert band[32:] == b'\xE7' * (65536 - 32)

    # Convert a completed two-row indexed band and verify every plane byte,
    # including neighbouring bytes that must survive an isolated repaint.
    pixels = bytes([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15] * 2)
    r.uc.mem_write(0x50000, pixels)
    r.call('ui_comp_present')
    assert mask[0] == 15
    touched = {2 * 80 + 1, 2 * 80 + 2, 3 * 80 + 1, 3 * 80 + 2}
    for p in range(4):
        assert all(v == 0xA5 for i, v in enumerate(planes[p]) if i not in touched)
    for y in range(2):
        for x in range(16):
            address = (y + 2) * 80 + 1 + x // 8
            bit = 7 - x % 8
            color = sum(((planes[p][address] >> bit) & 1) << p for p in range(4))
            assert color == pixels[y * 16 + x], (x, y, color)
    r.set('ui_comp_capacity', 4096)
    r.set('ui_comp_left', 0)
    r.set('ui_comp_right', 640)
    r.call('ui_comp_band_setup')
    assert r.get('ui_comp_stride') == 640 and r.get('ui_comp_rows') == 6
    print('PASS indexed paint clipping and exact four-plane VGA presentation')
    print('PASS reduced scratch capacity limits a full-width VGA band to six rows')


if __name__ == '__main__':
    main()
