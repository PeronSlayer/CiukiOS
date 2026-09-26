#!/usr/bin/env python3
"""Run real NASM renderer instructions against a deterministic VBE BIOS.

Both framebuffer backends are exercised: the banked 64 KiB window and the
linear framebuffer reached through short protected-mode limit bursts.

Requires Unicorn: uv run --with unicorn python scripts/test_ui_rendering.py
This is CPU/byte-level validation, not a physical GPU or timing qualification.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess

from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_INTR, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.x86_const import *

ROOT = Path(__file__).resolve().parents[1]
LFB = 0xE0000000
VRAM_SIZE = 32 * 1024 * 1024


class Renderer:
    def __init__(self, binary, linear=False):
        self.linear = linear
        self.uc = Uc(UC_ARCH_X86, UC_MODE_16)
        # Firmware supplies a valid real-mode IVT, unlike Unicorn's zero-limit
        # reset default. The burst guard snapshots and restores this IDTR.
        self.uc.reg_write(UC_X86_REG_IDTR, (0, 0, 0x3FF, 0))
        self.uc.mem_map(0, 0x100000)
        # High memory past the real-mode wrap: A20 is enabled in this machine.
        self.uc.mem_map(0x100000, 0x10000)
        self.uc.mem_write(0x10100, binary)
        self.symbols = {}
        cursor = binary.index(b'UIR1') + 4
        while address := struct.unpack_from('<H', binary, cursor)[0]:
            end = binary.index(0, cursor + 2)
            self.symbols[binary[cursor+2:end].decode()] = 0x10000 + address
            cursor = end + 1
        self.vram = bytearray([0xC7]) * VRAM_SIZE
        self.linear_reads = 0
        self.low_writes = 0
        if linear:
            self.uc.mem_map(LFB, VRAM_SIZE)
            self.uc.mem_write(LFB, bytes(self.vram))
            self.uc.hook_add(UC_HOOK_MEM_READ, self.linear_read, begin=LFB, end=LFB+VRAM_SIZE-1)
        # The interrupt vector table must never be written by the renderer.
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.low_write, begin=0, end=0x3FF)
        self.bank = None
        self.switches = 0
        self.video_reads = 0
        self.modes = {}
        self.page_y = 0
        self.reject_page = False
        self.page_snapshots = []
        self.uc.hook_add(UC_HOOK_INTR, self.interrupt)
        self.uc.hook_add(UC_HOOK_MEM_READ, self.video_read, begin=0xA0000, end=0xAFFFF)

    def video_read(self, uc, access, address, size, value, data):
        self.video_reads += size

    def linear_read(self, uc, access, address, size, value, data):
        self.linear_reads += size

    def low_write(self, uc, access, address, size, value, data):
        self.low_writes += size

    def interrupt(self, uc, number, _):
        ax = uc.reg_read(UC_X86_REG_AX)
        if number == 0x2F:
            assert ax == 0x4300, f'unexpected multiplex call {ax:04x}'
            return  # no XMS driver: AL stays 00
        assert number == 0x10, f'unexpected INT {number:x}'
        if ax == 0x4F05:
            assert not self.linear, "linear framebuffer mode switched a bank"
            self.sync()
            self.bank = uc.reg_read(UC_X86_REG_DX) * self.granularity
            uc.mem_write(0xA0000, bytes(self.vram[self.bank:self.bank+65536]))
            self.switches += 1
            uc.reg_write(UC_X86_REG_AX, 0x004F)
        elif ax == 0x4F00:
            controller = bytearray(512)
            controller[:4] = b'VESA'
            struct.pack_into('<H',controller,4,0x300)
            struct.pack_into('<HH',controller,14,0,0x8000)
            struct.pack_into('<H',controller,18,512)
            uc.mem_write(0x80000,struct.pack('<'+'H'*(len(self.modes)+1),*self.modes,0xFFFF))
            destination = uc.reg_read(UC_X86_REG_ES)*16 + uc.reg_read(UC_X86_REG_DI)
            uc.mem_write(destination,bytes(controller))
            uc.reg_write(UC_X86_REG_AX,0x004F)
        elif ax == 0x4F15:
            uc.reg_write(UC_X86_REG_AX,0x014F)  # a firmware without DDC
        elif ax == 0x4F01:
            mode = self.modes.get(uc.reg_read(UC_X86_REG_CX))
            assert mode is not None
            destination = uc.reg_read(UC_X86_REG_ES)*16 + uc.reg_read(UC_X86_REG_DI)
            uc.mem_write(destination,mode)
            uc.reg_write(UC_X86_REG_AX,0x004F)
        elif ax == 0x4F07:
            if uc.reg_read(UC_X86_REG_BX) == 1:
                uc.reg_write(UC_X86_REG_CX,0)
                uc.reg_write(UC_X86_REG_DX,self.page_y)
            else:
                requested = uc.reg_read(UC_X86_REG_DX)
                if requested and self.reject_page:
                    uc.reg_write(UC_X86_REG_AX,0x014F)
                    return
                self.sync()
                self.page_y = requested
                pitch = self.get('vc_pitch')
                size = self.get('vc_frame_bytes',4)
                self.page_snapshots.append(bytes(self.vram[requested*pitch:requested*pitch+size]))
            uc.reg_write(UC_X86_REG_AX,0x004F)
        elif ax != 3:
            raise AssertionError(f'unexpected VBE operation {ax:04x}')

    def sync(self):
        if self.linear:
            self.vram[:] = self.uc.mem_read(LFB, VRAM_SIZE)
        elif self.bank is not None:
            self.vram[self.bank:self.bank+65536] = self.uc.mem_read(0xA0000, 65536)

    def set(self, name, value, size=2):
        self.uc.mem_write(self.symbols[name], value.to_bytes(size, 'little'))

    def get(self, name, size=2):
        return int.from_bytes(self.uc.mem_read(self.symbols[name], size), 'little')

    def call(self, name, **regs):
        uc = self.uc
        for reg in (UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS):
            uc.reg_write(reg, 0x1000)
        uc.reg_write(UC_X86_REG_SP, 0xEFFC)
        uc.reg_write(UC_X86_REG_EFLAGS, 2)
        uc.mem_write(0x1EFFC, struct.pack('<H', self.symbols['stop'] - 0x10000))
        for name_, value in regs.items():
            uc.reg_write(globals()['UC_X86_REG_' + name_.upper()], value)
        uc.emu_start(self.symbols[name], self.symbols['stop'], count=200000000)
        assert uc.reg_read(UC_X86_REG_IP) == self.symbols['stop'] - 0x10000, 'routine did not return'
        self.sync()
        return bool(uc.reg_read(UC_X86_REG_EFLAGS) & 1)

    def mode(self, width, height, depth, padding=0, memory_mb=32, granularity=64):
        info = bytearray(256)
        def word(offset, value):
            struct.pack_into('<H', info, offset, value)
        word(0, 0x99 if self.linear else 0x19)
        if self.linear:
            struct.pack_into('<I', info, 40, LFB)
        info[2] = 7
        word(4, granularity)
        word(6, 64)
        word(8, 0xA000)
        word(16, width * ((depth + 7) // 8) + padding)
        word(18, width)
        word(20, height)
        info[24:28] = bytes((1, depth, 1, 4 if depth == 8 else 6))
        info[31:37] = bytes((5, 10, 5, 5, 5, 0) if depth == 15 else
                           (5, 11, 6, 5, 5, 0) if depth == 16 else
                           (8, 16, 8, 8, 8, 0))
        self.uc.mem_write(self.symbols['vc_info'], bytes(info))
        self.uc.mem_write(self.symbols['vc_controller']+18, struct.pack('<H', memory_mb*16))
        self.granularity = granularity * 1024
        rejected = self.call('vc_validate')
        self.set('vc_active', 1, 1)
        self.set('vc_bank', 0xFFFF)
        self.set('ui_width', width)
        self.set('ui_height', height)
        if self.linear and not rejected:
            assert self.get('vc_lfb_ok', 1) == 1
            assert not self.call('vc_lfb_open'), 'A20 probe rejected an enabled gate'
            self.set('vc_lfb', 1, 1)
            self.set('vc_lfb_base', LFB, 4)
        return rejected

    def state_call(self, name, **regs):
        """Call with IF set, distinctive ES/GS and GDTR; verify all restored."""
        uc = self.uc
        uc.reg_write(UC_X86_REG_GDTR, (0, 0x123450, 0x2F, 0))
        uc.reg_write(UC_X86_REG_GS, 0x2345)
        result = self.call(name, eflags=0x202, es=0x3456, **regs)
        assert uc.reg_read(UC_X86_REG_GDTR)[1:3] == (0x123450, 0x2F), 'GDTR not restored'
        assert uc.reg_read(UC_X86_REG_IDTR)[1:3] == (0, 0x3FF), 'IDTR not restored'
        assert uc.reg_read(UC_X86_REG_EFLAGS) & 0x200, 'interrupt flag not restored'
        assert uc.reg_read(UC_X86_REG_ES) == 0x3456, 'ES not restored'
        assert uc.reg_read(UC_X86_REG_GS) == 0x2345, 'GS not restored'
        assert self.get('vc_fb_depth', 1) == 0, 'unbalanced framebuffer burst'
        assert self.low_writes == 0, 'renderer wrote the interrupt vector table'
        return result


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--output', type=Path, default=ROOT/'build/full/ui-redesign-2026-09-25/rendering-cpu')
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    binary_path = args.output/'renderer.bin'
    subprocess.run(['nasm', '-f', 'bin', 'scripts/fixtures/ui_rendering_cpu.asm', '-o', str(binary_path)], cwd=ROOT, check=True)
    binary = binary_path.read_bytes()
    results = []
    def passed(name):
        results.append(name)
        print('PASS', name, flush=True)

    for w, h in ((800,600), (1024,768), (1920,1080), (2048,1152), (2048,1536), (2560,1440)):
        for depth in (8, 15, 16, 24, 32):
            r = Renderer(binary)
            assert not r.mode(w, h, depth, padding=16)
            assert r.get('vc_frame_bytes', 4) == (w*((depth+7)//8)+16)*h
    passed('six native geometries through 2560x1440, five pixel depths and padded pitches')
    r = Renderer(binary)
    assert r.mode(2560, 1440, 32, memory_mb=8)
    assert r.mode(8192, 8192, 32)
    passed('reject insufficient VRAM and oversized real-mode console')

    r = Renderer(binary)
    for ident, geometry in ((0x111,(800,600,16)), (0x140,(2560,1440,24)),
                            (0x141,(2560,1440,32)), (0x142,(2560,1440,8)),
                            (0x143,(2048,1152,32))):
        assert not r.mode(*geometry)
        r.modes[ident] = bytes(r.uc.mem_read(r.symbols['vc_info'],256))
    r.call('vc_resolve_mode',ax=2560,dx=1440)
    assert r.get('vc_mode') == 0x141
    r.call('vc_resolve_mode',ax=2048,dx=1152)
    assert r.get('vc_mode') == 0x143
    r.call('vc_resolve_mode',ax=1920,dx=1080)
    assert r.get('vc_mode') == 0
    passed('exact native geometry enumeration chooses highest valid depth and rejects absent modes')

    r = Renderer(binary)
    assert not r.mode(2560, 1440, 32)
    for bx, dx, cx, si in ((2560,0,4,4), (0,1440,4,4), (0,0,0,2), (0,0,2,0), (65535,0,2,2)):
        r.set('ui_comp_dirty', 0, 1)
        r.call('ui_comp_damage', bx=bx, dx=dx, cx=cx, si=si)
        assert r.get('ui_comp_dirty', 1) == 0
    r.call('ui_comp_damage', bx=2550, dx=1430, cx=65535, si=65535)
    assert (r.get('ui_comp_left'), r.get('ui_comp_right'), r.get('ui_comp_limit_top'), r.get('ui_comp_limit_bottom')) == (2550,2560,1430,1440)
    r.call('ui_comp_damage', bx=20, dx=30, cx=10, si=10)
    assert (r.get('ui_comp_left'), r.get('ui_comp_right'), r.get('ui_comp_limit_top'), r.get('ui_comp_limit_bottom')) == (20,2560,30,1440)
    passed('empty/off-screen damage is ignored; wrapped extents clip and union correctly')

    for depth in (8,15,16,24,32):
        r = Renderer(binary)
        assert not r.mode(2560,1440,depth,padding=14)
        bpp = (depth+7)//8
        pitch = r.get('vc_pitch')
        # A non-aligned partial region spanning several physical bank boundaries.
        left, right, top, bottom = 73, 2519, 19, 24
        stride = (right-left)*bpp
        r.set('ui_comp_seg', 0x5000)
        r.set('ui_comp_stride', stride)
        for name, value in (('left',left),('right',right),('top',top),('bottom',bottom)):
            r.set('ui_comp_'+name, value)
        r.call('ui_comp_band_setup')
        assert r.get('ui_comp_stride') == stride
        assert r.get('ui_comp_rows') == min(64,61440//stride)
        source = bytes((i*17+3)&255 for i in range(61440))
        r.uc.mem_write(0x50000, source + b'\xAD'*4096)
        expected = bytearray(r.vram)
        for y in range(top,bottom):
            start = (y-top)*stride
            expected[y*pitch+left*bpp:y*pitch+right*bpp] = source[start:start+(right-left)*bpp]
        r.call('ui_comp_present')
        assert r.vram == expected, f'bad banked copy at {depth}bpp'
        assert r.video_reads == 0, 'compositor should never read destination VRAM'
        assert bytes(r.uc.mem_read(0x5F000,4096)) == b'\xAD'*4096
    passed('partial-band copies preserve every outside byte at 8/15/16/24/32bpp, including bank crossings')

    for depth in (8,15,16,24,32):
        r = Renderer(binary)
        assert not r.mode(2560,1440,depth)
        bpp = (depth+7)//8
        r.set('ui_comp_seg',0x5000)
        for name,value in (('left',100),('right',150),('top',20),('bottom',24)):
            r.set('ui_comp_'+name,value)
        r.call('ui_comp_band_setup')
        r.uc.mem_write(0x50000,b'\xAD'*65536)
        r.uc.mem_write(r.symbols['vc_colors']+12,struct.pack('<I',0x125599))
        r.call('test_comp_rect',ax=3,bx=90,dx=18,cx=80,si=8)
        expected = bytearray(b'\xAD'*65536)
        for row in range(4):
            expected[row*50*bpp:(row+1)*50*bpp] = struct.pack('<I',0x125599)[:bpp]*50
        assert bytes(r.uc.mem_read(0x50000,65536)) == expected
    passed('rectangle fills clip all four band edges and preserve all allocation guards at five depths')

    for depth in (8,15,16,24,32):
        r = Renderer(binary)
        assert not r.mode(2560,1440,depth)
        bpp = (depth+7)//8
        r.set('ui_comp_seg',0x5000)
        for name,value in (('left',100),('right',110),('top',20),('bottom',24)):
            r.set('ui_comp_'+name,value)
        r.call('ui_comp_band_setup')
        r.uc.mem_write(0x50000,b'\xAD'*65536)
        r.uc.mem_write(0x1D000,b'\xFF'*32)
        r.set('ug_text_color',3,1)
        r.uc.mem_write(r.symbols['vc_colors']+12,struct.pack('<I',0x125599))
        r.call('ui_comp_glyph',bx=95,dx=18,si=0xD000)
        expected = bytearray(b'\xAD'*65536)
        expected[:4*10*bpp] = struct.pack('<I',0x125599)[:bpp]*40
        assert bytes(r.uc.mem_read(0x50000,65536)) == expected
    passed('packed glyph origin clips safely across all band edges at five depths')

    r = Renderer(binary)
    assert not r.mode(2560,1440,32)
    r.set('ui_comp_left',0)
    r.set('ui_comp_right',2560)
    r.call('ui_comp_band_setup')
    full_rows = r.get('ui_comp_rows')
    r.set('ui_comp_left',200)
    r.set('ui_comp_right',740)
    r.call('ui_comp_band_setup')
    narrow_rows = r.get('ui_comp_rows')
    assert (full_rows,narrow_rows) == (6,28)
    passed('540-pixel damage on 2560x1440x32 fits 28 scratch rows instead of six')

    r = Renderer(binary)
    assert not r.mode(2560,1440,24)
    expected = bytearray(r.vram)
    native = 0x0055AA33
    r.call('vc_span', edi=65534, ecx=100, eax=native)
    expected[65534:65534+300] = b'\x33\xAA\x55'*100
    assert r.vram == expected
    r.call('vc_span', edi=65534, ecx=0, eax=0)
    assert r.vram == expected
    passed('24-bit pixels crossing bank boundaries and zero-length span')

    r = Renderer(binary)
    assert not r.mode(641,481,8)
    size = 641*481
    r.call('vc_clear_video')
    assert r.vram[:size] == bytes(size)
    assert r.vram[size:] == b'\xC7'*(len(r.vram)-size)
    passed('odd final framebuffer byte clears without touching VRAM beyond the surface')

    # Complete the last character exactly on a bank/frame edge: prefetching a
    # nonexistent next bank must not disable video after the final pixel.
    r = Renderer(binary)
    assert not r.mode(2560,1440,32)
    r.set('vc_cells_seg',0x6000)
    r.set('vc_font_seg',0x7000)
    r.set('vc_font_off',0)
    r.set('vc_batch',0,1)
    r.uc.mem_write(0x60000,b'\x41\x0F'*(320*90))
    r.uc.mem_write(0x70000,b'\xFF'*4096)
    r.uc.mem_write(r.symbols['vc_colors']+60,struct.pack('<I',0x445566))
    r.call('vc_draw_cell',bx=320*90-1)
    assert r.get('vc_active',1) == 1
    assert r.vram[2560*1440*4-32:2560*1440*4] == struct.pack('<I',0x445566)*8
    passed('last console cell at exact framebuffer/bank edge remains active')

    r = Renderer(binary)
    assert not r.mode(2560,1440,32,memory_mb=32)
    r.uc.mem_write(r.symbols['vc_info']+29,b'\x01')
    r.set('vc_scan_lines',3276)
    r.call('ui_pages_begin')
    assert r.get('ui_page_enabled',1) == 1
    assert r.get('vc_access_bytes',4) == 2560*1440*8
    assert r.page_y == 0
    r.call('vc_put_pixel',edi=2560*1440*8-4,eax=0x11223344)
    assert r.get('vc_active',1) == 1
    assert r.vram[2560*1440*8-4:2560*1440*8] == b'\x44\x33\x22\x11'
    r = Renderer(binary)
    assert not r.mode(2560,1440,32,memory_mb=16)
    r.uc.mem_write(r.symbols['vc_info']+29,b'\x01')
    r.set('vc_scan_lines',1638)
    r.call('ui_pages_begin')
    assert r.get('ui_page_enabled',1) == 0 and r.page_y == 0
    passed('double-page allocation requires two complete framebuffers and exposes exactly that bank range')

    for linear in (False, True):
        r = Renderer(binary, linear)
        assert not r.mode(800,600,32,memory_mb=8)
        r.uc.mem_write(r.symbols['vc_info']+29,b'\x00')
        r.set('vc_scan_lines',2621)
        r.call('ui_pages_begin')
        assert r.get('ui_page_enabled',1) == 1 and r.page_y == 0
        assert r.get('vc_access_bytes',4) == 800*600*8
        for failure in ('scanlines','memory','display-start'):
            r = Renderer(binary, linear)
            assert not r.mode(800,600,32,memory_mb=2 if failure=='memory' else 8)
            r.uc.mem_write(r.symbols['vc_info']+29,b'\x00')
            r.set('vc_scan_lines',1199 if failure=='scanlines' else 2621)
            r.reject_page = failure == 'display-start'
            r.call('ui_pages_begin')
            assert r.get('ui_page_enabled',1) == 0 and r.page_y == 0, (linear,failure)
            assert r.get('vc_access_bytes',4) == 800*600*4
    passed('zero static page counts use verified active capability; short scanlines, VRAM and refused display start still reject both backends')

    r = Renderer(binary)
    assert not r.mode(800,600,8)
    r.uc.mem_write(r.symbols['vc_info']+29,b'\x01')
    r.set('vc_scan_lines',4096)
    r.reject_page = True
    r.call('ui_pages_begin')
    assert r.get('ui_page_enabled',1) == 0
    assert r.get('ui_comp_recover',1) == 0 and r.page_y == 0
    passed('rejected display-start capability probe restores page zero before disabling paging')

    r = Renderer(binary)
    assert not r.mode(800,600,8)
    r.uc.mem_write(r.symbols['vc_info']+29,b'\x01')
    r.set('vc_scan_lines',4096)
    r.call('ui_pages_begin')
    r.set('ui_comp_seg',0x5000)
    r.set('test_scene_count',2)
    r.uc.mem_write(r.symbols['vc_colors'],struct.pack('<16I',*range(16)))
    previous = None
    for x,y,width,height,color in ((40,60,180,120,3),(360,240,240,180,7),(610,410,170,130,12),(50,80,210,100,9)):
        rects = struct.pack('<HHHHBB',0,0,800,600,1,0) + struct.pack('<HHHHBB',x,y,width,height,color,0)
        r.uc.mem_write(r.symbols['test_scene_rects'],rects)
        if previous:
            px,py,pw,ph = previous
            r.call('ui_comp_damage',bx=px,dx=py,cx=pw,si=ph)
            r.call('ui_comp_damage',bx=x,dx=y,cx=width,si=height)
        expected = bytearray(b'\x01'*(800*600))
        for row in range(y,y+height):
            expected[row*800+x:row*800+x+width] = bytes([color])*width
        r.call('ui_comp_draw')
        assert r.page_snapshots[-1] == expected, 'page switched before the complete scene was ready'
        assert r.get('ui_front_base',4) == r.page_y*800
        previous = x,y,width,height
    assert r.video_reads == 0
    assert r.vram[960000:] == b'\xC7'*(len(r.vram)-960000)
    # Reject a subsequent page-1 swap. Recovery must request a fresh ordinary
    # video session and prevent another attempt to enable the failed feature.
    r.reject_page = True
    r.call('ui_comp_damage',bx=20,dx=20,cx=20,si=20)
    r.call('ui_comp_draw')
    assert r.get('ui_comp_recover',1) == 1
    assert r.get('ui_page_enabled',1) == 0 and r.get('ui_page_failed',1) == 1
    assert r.get('vc_active',1) == 0
    passed('four moving-window presentations contain complete current scenes, no ghost frames; runtime flip failure requests safe recovery')

    # ---- Linear framebuffer backend (VBE 2.0 PhysBasePtr) ----
    def load_vram(r, data):
        r.vram[:len(data)] = data
        if r.linear:
            r.uc.mem_write(LFB, bytes(r.vram))

    r = Renderer(binary, True)
    info = bytearray(256)
    r.mode(1024, 768, 32)
    info = bytearray(r.uc.mem_read(r.symbols['vc_info'], 256))
    info[0] |= 0x40  # firmware without a banked window in this mode
    r.uc.mem_write(r.symbols['vc_info'], bytes(info))
    assert not r.call('vc_validate')
    assert (r.get('vc_bank_ok', 1), r.get('vc_lfb_ok', 1)) == (0, 1)
    info[0] &= 0x7F
    r.uc.mem_write(r.symbols['vc_info'], bytes(info))
    assert r.call('vc_validate'), 'a mode with neither access method must be rejected'
    info[0] |= 0x80
    struct.pack_into('<I', info, 40, 0xFFFFF000)
    r.uc.mem_write(r.symbols['vc_info'], bytes(info))
    assert r.call('vc_validate'), 'a framebuffer wrapping 4 GiB must be rejected'
    passed('linear-only modes accepted; windowless modes without LFB and wrapping PhysBasePtr rejected')

    r = Renderer(binary, True)
    assert not r.mode(1024, 768, 32)
    mirror = r.uc.hook_add(UC_HOOK_MEM_WRITE,
        lambda uc, access, address, size, value, data:
            uc.mem_write(0x100000 + address, value.to_bytes(size, 'little')),
        begin=0x4F0, end=0x4F1)
    assert r.call('vc_lfb_open'), 'A20 wrap-around must disable linear access'
    r.uc.hook_del(mirror)
    passed('A20 wrap probe refuses linear access while the gate is disabled')

    for depth in (8, 15, 16, 24, 32):
        outputs = []
        for linear in (False, True):
            r = Renderer(binary, linear)
            assert not r.mode(2560, 1440, depth, padding=14)
            bpp = (depth+7)//8
            left, right, top, bottom = 73, 2519, 19, 24
            r.set('ui_comp_seg', 0x5000)
            for name, value in (('left',left),('right',right),('top',top),('bottom',bottom)):
                r.set('ui_comp_'+name, value)
            r.call('ui_comp_band_setup')
            source = bytes((i*17+3)&255 for i in range(61440))
            r.uc.mem_write(0x50000, source + b'\xAD'*4096)
            r.state_call('ui_comp_present')
            if linear:
                assert r.switches == 0 and r.linear_reads == 0
            outputs.append(bytes(r.vram))
        assert outputs[0] == outputs[1], f'linear band differs from banked at {depth}bpp'
    passed('linear band presentation equals banked output at five depths; no bank switch, VRAM read, IVT write; GDTR/IF/ES/GS restored')

    r = Renderer(binary, True)
    assert not r.mode(2560, 1440, 24)
    expected = bytearray(r.vram)
    r.state_call('vc_span', edi=65534, ecx=100, eax=0x0055AA33)
    expected[65534:65534+300] = b'\x33\xAA\x55'*100
    assert r.vram == expected
    r.state_call('vc_span', edi=65534, ecx=0, eax=0)
    frame = r.get('vc_frame_bytes', 4)
    r.state_call('vc_span', edi=frame-3, ecx=2, eax=0x112233)
    r.state_call('vc_put_pixel', edi=frame, eax=0x445566)
    assert r.vram == expected, 'linear writes outside the validated surface'
    rows = bytes(range(256))*64
    r.uc.mem_write(0x50000, rows)
    r.state_call('vc_fb_put_rows', edi=frame-2*r.get('vc_pitch'), esi=0, ecx=300,
                 edx=4, ebx=0x100, fs=0x5000)
    pitch = r.get('vc_pitch')
    for row in range(2):
        expected[frame-(2-row)*pitch:frame-(2-row)*pitch+300] = rows[row*256:row*256+300]
    assert r.vram == expected
    r = Renderer(binary, True)
    assert not r.mode(641, 481, 8)
    size = 641*481
    r.state_call('vc_clear_video')
    assert r.vram[:size] == bytes(size) and r.vram[size:] == b'\xC7'*(len(r.vram)-size)
    passed('linear spans, row copies and clears stay inside the surface, including odd sizes')

    for depth in (8, 16, 24, 32):
        outputs = []
        for linear in (False, True):
            r = Renderer(binary, linear)
            assert not r.mode(1024, 768, depth)
            bpp = (depth+7)//8
            cols, rows = r.get('vc_cols'), r.get('vc_rows')
            r.set('vc_cells_seg', 0x6000)
            r.set('vc_font_seg', 0x7000)
            r.set('vc_font_off', 0)
            cells = bytearray()
            for i in range(cols*rows):
                cells += bytes(((i*7+33) & 0x7F, (i*13+5) & 0xFF))
            r.uc.mem_write(0x60000, bytes(cells))
            font = bytes((i*29+5) & 255 for i in range(4096))
            r.uc.mem_write(0x70000, font)
            colors = [(0x00A1B2C3 + c*0x00131517) & 0xFFFFFFFF for c in range(16)]
            if bpp == 1:
                colors = list(range(16))
            r.uc.mem_write(r.symbols['vc_colors'], struct.pack('<16I', *colors))
            r.set('vc_batch', 1, 1)
            for cell in range(0, cols*rows, 37):
                r.call('vc_queue_cell', bx=cell)
            r.state_call('vc_flush')
            r.state_call('vc_draw_cell', bx=cols*rows-1)
            outputs.append(bytes(r.vram))
        assert outputs[0] == outputs[1], f'linear console differs from banked at {depth}bpp'
        # Independently render the final cell.
        pitch = r.get('vc_pitch')
        cell = cols*rows-1
        ch, attr = cells[cell*2], cells[cell*2+1]
        x0, y0 = (cell % cols)*8, (cell//cols)*16
        for scan in range(16):
            bits = font[ch*16+scan]
            line = b''.join(struct.pack('<I', colors[attr & 15] if bits & (0x80 >> px) else colors[attr >> 4])[:bpp]
                            for px in range(8))
            offset = (y0+scan)*pitch + x0*bpp
            assert outputs[1][offset:offset+8*bpp] == line
    passed('linear batched and direct console output equals banked output; independent glyph check')

    masks = struct.unpack('<32H', bytes(r.uc.mem_read(r.symbols['test_pointer_masks'], 64)))
    for depth in (8, 15, 16, 24, 32):
        outputs = []
        for linear in (False, True):
            r = Renderer(binary, linear)
            assert not r.mode(1280, 1024, depth)
            bpp = (depth+7)//8
            pitch = r.get('vc_pitch')
            background = bytes((i*31+7) & 255 for i in range(1024*pitch))
            load_vram(r, background)
            colors = [(0x00102030 + c*0x00050607) for c in range(16)]
            r.uc.mem_write(r.symbols['vc_colors'], struct.pack('<16I', *colors))
            x, y = 1256, 1008
            edi = y*pitch + x*bpp
            area = 0xD000
            masks_off = r.symbols['test_pointer_masks'] - 0x10000
            r.state_call('vc_pointer_save', edi=edi, esi=area)
            assert bytes(r.uc.mem_read(0x10000+area, 24*bpp*16)) == b''.join(
                background[(y+row)*pitch+x*bpp:(y+row)*pitch+(x+24)*bpp] for row in range(16))
            r.state_call('vc_pointer_draw', edi=edi, esi=area, ebx=masks_off)
            drawn = bytearray(background)
            for row in range(16):
                for bit in range(16):
                    if masks[16+row] & (0x8000 >> bit):
                        color = colors[15]
                    elif masks[row] & (0x8000 >> bit):
                        color = colors[0]
                    else:
                        continue
                    offset = (y+row)*pitch + (x+bit)*bpp
                    drawn[offset:offset+bpp] = struct.pack('<I', color)[:bpp]
            assert r.vram[:len(drawn)] == drawn, f'pointer arrow wrong at {depth}bpp linear={linear}'
            r.state_call('vc_pointer_restore', edi=edi, esi=area)
            assert r.vram[:len(background)] == background, 'pointer restore left pixels behind'
            outputs.append(bytes(r.vram))
        assert outputs[0] == outputs[1]
    passed('software pointer save/draw/restore round trip at five depths on both backends, at the screen corner')

    r = Renderer(binary, True)
    assert not r.mode(800, 600, 8)
    r.uc.mem_write(r.symbols['vc_info']+29, b'\x01')
    r.set('vc_scan_lines', 4096)
    r.call('ui_pages_begin')
    assert r.get('ui_page_enabled', 1) == 1
    r.set('ui_comp_seg', 0x5000)
    r.set('test_scene_count', 2)
    r.uc.mem_write(r.symbols['vc_colors'], struct.pack('<16I', *range(16)))
    previous = None
    for x, y, width, height, color in ((40,60,180,120,3),(360,240,240,180,7),(610,410,170,130,12),(50,80,210,100,9)):
        rects = struct.pack('<HHHHBB',0,0,800,600,1,0) + struct.pack('<HHHHBB',x,y,width,height,color,0)
        r.uc.mem_write(r.symbols['test_scene_rects'], rects)
        if previous:
            px, py, pw, ph = previous
            r.call('ui_comp_damage', bx=px, dx=py, cx=pw, si=ph)
            r.call('ui_comp_damage', bx=x, dx=y, cx=width, si=height)
        expected = bytearray(b'\x01'*(800*600))
        for row in range(y, y+height):
            expected[row*800+x:row*800+x+width] = bytes([color])*width
        r.state_call('ui_comp_draw')
        assert r.page_snapshots[-1] == expected, 'linear page switched before the complete scene was ready'
        previous = x, y, width, height
    assert r.switches == 0 and r.linear_reads == 0
    assert r.vram[960000:] == b'\xC7'*(len(r.vram)-960000)
    passed('linear two-page presentation shows only complete scenes and never reads VRAM')

    (args.output/'results.json').write_text(json.dumps({'scope':'real renderer CPU instructions; emulated VBE bank BIOS, not GPU timing', 'passed':results},indent=2)+'\n')


if __name__ == '__main__':
    main()
