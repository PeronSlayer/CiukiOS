#!/usr/bin/env python3
"""Execute the production external icon loader and reader with Unicorn.

Run: uv run --with unicorn python scripts/test_ui_icons.py
DOS file/allocation interrupts and the rectangle callback are simulated.
The real pack and unchanged production assembly are used. This checks CPU
instructions and resource lifetime; QEMU separately checks final screen output.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_CODE, UC_HOOK_INTR, UC_HOOK_MEM_READ
from unicorn.x86_const import *
from build_ui_icons import ICONS

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x10000
ALLOC_SEG = 0x3000
ALLOC = ALLOC_SEG * 16
ICON_COUNT = len(ICONS)
PACK_SIZE = 16 + ICON_COUNT * 32 * 32
PARAGRAPHS = (PACK_SIZE + 15) // 16
GUARD = b'ICON_GUARD_01234'
SYMBOLS = ('begin', 'end', 'draw', 'rect', 'segment', 'handle', 'vbe',
           'composing', 'top', 'bottom', 'fallback', 'extra', 'path', 'bytes')


def signed(value):
    return value if value < 32768 else value - 65536


class Icons:
    def __init__(self, binary, payload, failure=None):
        self.symbols = dict(zip(SYMBOLS, struct.unpack_from('<14H', binary)))
        assert self.symbols['bytes'] == PACK_SIZE
        self.uc = Uc(UC_ARCH_X86, UC_MODE_16)
        self.uc.mem_map(0, 0x100000)
        self.uc.mem_write(BASE, binary)
        self.uc.mem_write(ALLOC-16, GUARD)
        self.uc.mem_write(ALLOC, bytes([0xAA]) * (PARAGRAPHS * 16))
        self.uc.mem_write(ALLOC + PARAGRAPHS*16, GUARD)
        self.payload = payload
        self.failure = failure
        self.position = 0
        self.events = []
        self.open = False
        self.allocated = False
        self.read_calls = 0
        self.seen = {}
        self.runs = []
        self.source_reads = []
        self.draw_spec = None
        self.uc.hook_add(UC_HOOK_INTR, self.interrupt)
        self.uc.hook_add(UC_HOOK_CODE, self.rectangle)
        # Include a guard on either side so an over-read fails too.
        self.uc.hook_add(UC_HOOK_MEM_READ, self.read_source,
                         begin=ALLOC-16, end=ALLOC+PARAGRAPHS*16+15)
        self.binary = binary

    def word(self, name):
        return struct.unpack('<H', self.uc.mem_read(BASE+self.symbols[name], 2))[0]

    def set_word(self, name, value):
        self.uc.mem_write(BASE+self.symbols[name], struct.pack('<H', value & 65535))

    def set_byte(self, name, value):
        self.uc.mem_write(BASE+self.symbols[name], bytes([value]))

    def carry(self, value):
        flags = self.uc.reg_read(UC_X86_REG_EFLAGS)
        self.uc.reg_write(UC_X86_REG_EFLAGS, flags | 1 if value else flags & ~1)

    def error(self, code):
        self.uc.reg_write(UC_X86_REG_AX, code)
        self.carry(True)

    def interrupt(self, cpu, number, data):
        assert number == 0x21, ('unexpected interrupt', number)
        ah = cpu.reg_read(UC_X86_REG_AH)
        self.events.append(ah)
        if ah == 0x3D:
            assert not self.open
            assert cpu.reg_read(UC_X86_REG_AL) == 0, 'pack must open read-only'
            address = cpu.reg_read(UC_X86_REG_DS)*16 + cpu.reg_read(UC_X86_REG_DX)
            path = bytes(cpu.mem_read(address, 64)).split(b'\0', 1)[0]
            assert path == b'\\SYSTEM\\UI\\ICONS.DAT', path
            if self.failure == 'open':
                self.error(2)
                return
            self.open = True
            self.position = 0
            cpu.reg_write(UC_X86_REG_AX, 7)
        elif ah == 0x48:
            assert not self.allocated
            assert cpu.reg_read(UC_X86_REG_BX) == PARAGRAPHS
            if self.failure == 'allocate':
                self.error(8)
                return
            self.allocated = True
            cpu.reg_write(UC_X86_REG_AX, ALLOC_SEG)
        elif ah == 0x3F:
            assert self.open and cpu.reg_read(UC_X86_REG_BX) == 7
            self.read_calls += 1
            length = cpu.reg_read(UC_X86_REG_CX)
            address = cpu.reg_read(UC_X86_REG_DS)*16 + cpu.reg_read(UC_X86_REG_DX)
            if self.read_calls % 2 == 1:
                assert length == PACK_SIZE and address == ALLOC
                if self.failure == 'read':
                    self.error(5)
                    return
            else:
                assert length == 1 and address == BASE+self.symbols['extra']
                if self.failure == 'eof':
                    self.error(5)
                    return
            chunk = self.payload[self.position:self.position+length]
            if chunk:
                cpu.mem_write(address, chunk)
            self.position += len(chunk)
            cpu.reg_write(UC_X86_REG_AX, len(chunk))
        elif ah == 0x3E:
            assert self.open and cpu.reg_read(UC_X86_REG_BX) == 7
            self.open = False
            cpu.reg_write(UC_X86_REG_AX, 0)
        elif ah == 0x49:
            assert self.allocated and cpu.reg_read(UC_X86_REG_ES) == ALLOC_SEG
            self.allocated = False
            cpu.reg_write(UC_X86_REG_AX, 0)
        else:
            raise AssertionError(('unexpected DOS function', hex(ah)))
        self.carry(False)

    def rectangle(self, cpu, address, size, data):
        if address != BASE+self.symbols['rect']:
            return
        assert self.draw_spec is not None
        ox, oy, rows, packed, fallback = self.draw_spec
        x = signed(cpu.reg_read(UC_X86_REG_BX))
        y = signed(cpu.reg_read(UC_X86_REG_DX))
        width = cpu.reg_read(UC_X86_REG_CX)
        height = cpu.reg_read(UC_X86_REG_SI)
        color = cpu.reg_read(UC_X86_REG_AL)
        assert height == 1 and 0 < width <= 32
        assert 0 <= x-ox < 32 and x-ox+width <= 32
        assert y-oy in rows
        assert 0 <= color < (144 if packed else 16)
        for px in range(x, x+width):
            assert (px-ox, y-oy) not in self.seen, 'overlapping pixel runs'
            self.seen[px-ox, y-oy] = color
        self.runs.append((x, y, width, color))
        # Existing rectangle primitives preserve registers, not flags.
        cpu.reg_write(UC_X86_REG_EFLAGS, 0x202)

    def read_source(self, cpu, access, address, size, value, data):
        assert ALLOC <= address and address+size <= ALLOC+PACK_SIZE, 'asset over-read'
        if self.draw_spec is None:
            return
        relative = address - self.draw_source
        assert 0 <= relative and relative+size <= 1024, 'read escaped selected icon'
        assert all((relative+i)//32 in self.draw_spec[2] for i in range(size)), 'read outside compositor band'
        self.source_reads.append(relative)

    def call(self, name, *, icon=0, x=0, y=0, different_ds=False):
        regs = {UC_X86_REG_EAX: 0x12345600 | icon,
                UC_X86_REG_EBX: 0xABCD0000 | (x & 65535),
                UC_X86_REG_ECX: 0x88889999,
                UC_X86_REG_EDX: 0xABEF0000 | (y & 65535),
                UC_X86_REG_ESI: 0x77778888, UC_X86_REG_EDI: 0x55556666,
                UC_X86_REG_EBP: 0x33334444, UC_X86_REG_ESP: 0xFFF0,
                UC_X86_REG_DS: 0x2000 if different_ds else 0x1000,
                UC_X86_REG_ES: 0x4000, UC_X86_REG_CS: 0x1000,
                UC_X86_REG_SS: 0x7000, UC_X86_REG_FS: 0x5000,
                UC_X86_REG_GS: 0x6000, UC_X86_REG_EFLAGS: 0xE43}
        for reg, value in regs.items():
            self.uc.reg_write(reg, value)
        entry = self.symbols[name]
        self.uc.emu_start(BASE+entry, BASE+len(self.binary), count=1000000)
        assert self.uc.reg_read(UC_X86_REG_IP) == entry+4, 'routine did not return'
        for reg, value in regs.items():
            assert self.uc.reg_read(reg) == value, (name, reg, self.uc.reg_read(reg), value)
        assert bytes(self.uc.mem_read(ALLOC-16, 16)) == GUARD
        assert bytes(self.uc.mem_read(ALLOC+PARAGRAPHS*16, 16)) == GUARD


def loader_cases(binary, pack):
    cases = [('valid', pack, None, True),
             ('missing-file', pack, 'open', False),
             ('allocation-failed', pack, 'allocate', False),
             ('read-error', pack, 'read', False),
             ('eof-probe-error', pack, 'eof', False),
             ('empty-file', b'', None, False),
             ('short-header', pack[:15], None, False),
             ('short-pixels', pack[:-1], None, False),
             ('trailing-data', pack+b'X', None, False)]
    for name, offset, value in (('bad-magic', 0, b'NOPE'),
                               ('bad-version', 4, b'ICO2'),
                               ('bad-count', 8, struct.pack('<H', 19)),
                               ('bad-width', 10, struct.pack('<H', 31)),
                               ('bad-height', 12, struct.pack('<H', 33)),
                               ('reserved-field', 14, b'\1\0'),
                               ('first-pixel-invalid', 16, b'\x90'),
                               ('last-pixel-invalid', PACK_SIZE-1, b'\xfe')):
        bad = bytearray(pack)
        bad[offset:offset+len(value)] = value
        cases.append((name, bytes(bad), None, False))
    results = []
    for name, payload, failure, success in cases:
        test = Icons(binary, payload, failure)
        test.call('begin', different_ds=True)
        assert not test.open and test.word('handle') == 65535, name
        assert test.allocated == success, name
        assert test.word('segment') == (ALLOC_SEG if success else 0), name
        if success:
            assert bytes(test.uc.mem_read(ALLOC, PACK_SIZE)) == pack
            before = list(test.events)
            test.call('begin', different_ds=True)
            assert test.events == before, 'cached session was loaded twice'
        test.call('end')
        assert not test.allocated and test.word('segment') == 0
        before = list(test.events)
        test.call('end')
        assert test.events == before, 'free of an already-freed pack'
        results.append({'case': name, 'accepted': success, 'dos_calls': test.events,
                        'handles_closed': True, 'allocation_released': True,
                        'guards_untouched': True, 'preserves_full_abi': True})
    # Repeated desktop/DOS transitions must acquire and release each time.
    test = Icons(binary, pack)
    for _ in range(16):
        test.call('begin')
        assert test.allocated and test.word('segment') == ALLOC_SEG
        test.call('end')
        assert not test.allocated and not test.open and test.word('segment') == 0
    assert test.events.count(0x48) == test.events.count(0x49) == 16
    results.append({'case': '16-session-lifetime', 'allocations': 16, 'frees': 16,
                    'handles_closed': True, 'preserves_full_abi': True})
    return results


def reader_cases(binary, pack, fallback):
    results = []
    for icon in range(ICON_COUNT):
        source = pack[16+icon*1024:16+(icon+1)*1024]
        for packed in (False, True):
            for x, y in ((5, 2), (-18, -12), (2510, 1408)):
                ox, oy = x+6, y+4
                for composing, band in ((False, (0, 600)), (True, (oy, oy+32)),
                                        (True, (oy+3, oy+9)), (True, (oy-16, oy)),
                                        (True, (oy+32, oy+48))):
                    test = Icons(binary, pack)
                    test.call('begin')
                    test.set_byte('vbe', int(packed))
                    test.set_byte('composing', int(composing))
                    test.set_word('top', band[0])
                    test.set_word('bottom', band[1])
                    rows = {row for row in range(32)
                            if not composing or band[0] <= oy+row < band[1]}
                    expected = {}
                    for row in rows:
                        for column in range(32):
                            color = source[row*32+column]
                            if color != 255:
                                expected[column, row] = (color if packed or color < 16
                                                         else fallback[color-16])
                    test.draw_spec = ox, oy, rows, packed, fallback
                    test.draw_source = ALLOC+16+icon*1024
                    before = list(test.events)
                    test.call('draw', icon=icon, x=x, y=y)
                    assert test.events == before, 'paint accessed DOS'
                    assert test.seen == expected, (icon, packed, x, y, composing, band)
                    if not rows:
                        assert not test.runs and not test.source_reads
                    assert bytes(test.uc.mem_read(ALLOC, PACK_SIZE)) == pack, 'paint modified assets'
                    test.draw_spec = None
                    test.call('end')
                    results.append({'icon': icon, 'packed': packed, 'origin': [x, y],
                                    'composing': composing, 'band': list(band),
                                    'pixels': len(test.seen), 'runs': len(test.runs),
                                    'source_reads': len(test.source_reads),
                                    'preserves_full_abi': True, 'matches_pack': True,
                                    'no_paint_io': True})
        print(f'PASS icon {icon}: native pixels, transparency, bands, planar map, ABI', flush=True)
    return results


def reader_edges(binary, pack, fallback):
    results = []
    for loaded, icon in ((False, 0), (False, ICON_COUNT - 1), (False, ICON_COUNT), (False, 255),
                         (True, ICON_COUNT), (True, 255)):
        test = Icons(binary, pack)
        if loaded:
            test.call('begin')
        test.draw_spec = 6, 4, set(), True, fallback
        test.draw_source = ALLOC+16+icon*1024
        before = list(test.events)
        test.call('draw', icon=icon)
        assert not test.seen and not test.source_reads and test.events == before
        test.draw_spec = None
        test.call('end')
        results.append({'case': 'no-pack-or-invalid-id', 'loaded': loaded,
                        'icon': icon, 'no_reads_or_rectangles': True,
                        'preserves_full_abi': True})
    # Exercise every palette boundary plus long transparent/opaque runs,
    # independently of the particular colour usage in today's artwork.
    swatches = bytes([0, 15, 16, 79, 80, 143, 255, 255])*4
    source = swatches*32
    pattern = pack[:16]+source+pack[16+1024:]
    for packed in (False, True):
        test = Icons(binary, pattern)
        test.call('begin')
        assert test.allocated
        test.set_byte('vbe', int(packed))
        test.draw_spec = 6, 4, set(range(32)), packed, fallback
        test.draw_source = ALLOC+16
        test.call('draw')
        expected = {(x, y): (color if packed or color < 16 else fallback[color-16])
                    for y in range(32) for x, color in enumerate(swatches) if color != 255}
        assert test.seen == expected
        test.draw_spec = None
        test.call('end')
        results.append({'case': 'palette-boundary-swatches', 'packed': packed,
                        'tested_indices': [0, 15, 16, 79, 80, 143, 255],
                        'pixels': len(expected), 'matches_swatches': True})
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path,
                        default=ROOT/'build/full/retro-icons-2026-09-25/icon-cpu')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    target = args.output/'icons.bin'
    subprocess.run(['nasm', '-f', 'bin', 'scripts/fixtures/ui_icons_cpu.asm',
                    '-o', str(target)], cwd=ROOT, check=True)
    binary = target.read_bytes()
    pack = (ROOT/'assets/icons/native/ICONS.DAT').read_bytes()
    assert len(pack) == PACK_SIZE
    assert struct.unpack_from('<8s4H', pack) == (b'CIUKICO1', ICON_COUNT, 32, 32, 0)
    assert all(color < 144 or color == 255 for color in pack[16:])
    fallback = (ROOT/'assets/icons/native/fallback.bin').read_bytes()
    assert len(fallback) == 128 and max(fallback) < 16
    symbols = dict(zip(SYMBOLS, struct.unpack_from('<14H', binary)))
    assert binary[symbols['fallback']:symbols['fallback']+128] == fallback
    loaders = loader_cases(binary, pack)
    print(f'PASS {len(loaders)} loader/lifetime cases: format, I/O errors, handles, free, ABI', flush=True)
    edges = reader_edges(binary, pack, fallback)
    print(f'PASS {len(edges)} reader edge cases: missing pack, invalid IDs, palette boundaries', flush=True)
    readers = reader_cases(binary, pack, fallback)
    report = {'passed': True,
              'scope': 'unchanged production assembly with simulated DOS and rectangle callback',
              'fixture_sha256': hashlib.sha256(binary).hexdigest(),
              'source_sha256': hashlib.sha256((ROOT/'src/com/ui_icons.inc').read_bytes()).hexdigest(),
              'pack_sha256': hashlib.sha256(pack).hexdigest(),
              'loader_cases': loaders, 'reader_edge_cases': edges, 'reader_cases': readers}
    (args.output/'results.json').write_text(json.dumps(report, indent=2)+'\n')


if __name__ == '__main__':
    main()
