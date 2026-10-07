#!/usr/bin/env python3
"""Check production AUTO selection with absent, invalid and valid monitor data."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

from unicorn.x86_const import (
    UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_BP, UC_X86_REG_CX, UC_X86_REG_DX,
    UC_X86_REG_ES, UC_X86_REG_DI,
)
from test_vbe3_metadata import Video, descriptor, ROOT


MODES = {0x140: (640, 480), 0x141: (800, 600),
         0x142: (1024, 768), 0x143: (2560, 1440)}


def preferred_edid(width=1024, height=768):
    data = bytearray(128)
    data[:8] = bytes.fromhex('00ffffffffffff00')
    data[18:20] = bytes((1, 3))
    data[24] = 2
    struct.pack_into('<H', data, 54, 6500)
    data[56], data[58] = width & 255, (width >> 8) << 4
    data[59], data[61] = height & 255, (height >> 8) << 4
    data[127] = (-sum(data)) & 255
    return data


class SelectionVideo(Video):
    def __init__(self, binary, edid=None, modes=None):
        super().__init__(binary)
        self.edid = edid
        self.modes = MODES if modes is None else modes
        self.queries = []
        struct.pack_into('<HH', self.controller, 14, 0, 0x5000)
        self.cpu.mem_write(0x50000, struct.pack('<'+'H'*(len(self.modes)+1), *self.modes, 0xFFFF))

    def interrupt(self, cpu, number, data):
        ax = cpu.reg_read(UC_X86_REG_AX)
        buffer = cpu.reg_read(UC_X86_REG_ES)*16+cpu.reg_read(UC_X86_REG_DI)
        if number == 0x10 and ax == 0x4F15:
            if self.edid is not None:
                cpu.mem_write(buffer, bytes(self.edid))
            cpu.reg_write(UC_X86_REG_AX, 0x014F if self.edid is None else 0x004F)
            return
        if number == 0x10 and ax == 0x4F01:
            mode = cpu.reg_read(UC_X86_REG_CX)
            self.queries.append(mode)
            width, height = self.modes[mode]
            info = descriptor()
            for offset, value in ((16, width*4), (18, width), (20, height), (50, width*4)):
                struct.pack_into('<H', info, offset, value)
            cpu.mem_write(buffer, bytes(info))
            cpu.reg_write(UC_X86_REG_AX, 0x004F)
            return
        super().interrupt(cpu, number, data)


STARTUP_MODES = {
    0x111: (800, 600, 32),
    0x118: (1024, 768, 8),
    0x119: (1024, 768, 15),
    0x11A: (1024, 768, 16),
    0x11B: (1024, 768, 24),
    0x11C: (1024, 768, 32),
    0x120: (1280, 800, 32),
}


def mode_descriptor(width, height, depth):
    info = bytearray(descriptor())
    bpp = depth
    if depth == 8:
        info[27] = 4  # packed-pixel indexed mode
        info[29] = 0
        info[31:39] = bytes(8)
        struct.pack_into('<H', info, 16, width)
        struct.pack_into('<H', info, 50, width)
    else:
        info[27] = 6  # direct color
        if depth == 15:
            masks = (5, 10, 5, 5, 5, 0, 0, 0)
        elif depth == 16:
            masks = (5, 11, 6, 5, 5, 0, 0, 0)
        else:
            masks = (8, 16, 8, 8, 8, 0, 8, 24) if depth == 32 else (8, 16, 8, 8, 8, 0, 0, 0)
        info[31:39] = bytes(masks)
        info[54:62] = bytes(masks)
        pitch = width * ((bpp + 7) // 8)
        struct.pack_into('<H', info, 16, pitch)
        struct.pack_into('<H', info, 50, pitch)
    struct.pack_into('<HH', info, 18, width, height)
    info[24:28] = bytes((1, depth, 1, info[27]))
    return bytes(info)


class StartupVideo(Video):
    """Run the production startup path against a scripted VBE BIOS."""
    def __init__(self, binary, modes=None, edid=None, fail_set=(),
                 fail_readback=(), fail_allocations=0, raw_mode_ids=None):
        super().__init__(binary, msw_pe=1)
        self.modes = STARTUP_MODES if modes is None else modes
        self.descriptors = {
            mode: mode_descriptor(*details) for mode, details in self.modes.items()
        }
        self.edid = edid
        self.fail_set = set(fail_set)
        self.fail_readback = set(fail_readback)
        self.fail_allocations = fail_allocations
        self.current_mode = 3
        self.current_linear = False
        self.mode_attempts = []
        self.mode_queries = []
        self.readbacks = []
        self.text_restores = 0
        struct.pack_into('<HH', self.controller, 14, 0, 0x5000)
        raw_mode_ids = list(self.modes) if raw_mode_ids is None else raw_mode_ids
        self.cpu.mem_write(0x50000, struct.pack(
            '<'+'H'*(len(raw_mode_ids)+1), *raw_mode_ids, 0xFFFF))

    def interrupt(self, cpu, number, data):
        ax = cpu.reg_read(UC_X86_REG_AX)
        if number == 0x21:
            ah = (ax >> 8) & 0xFF
            if ah == 0x48:
                if self.fail_allocations:
                    self.fail_allocations -= 1
                    cpu.reg_write(UC_X86_REG_AX, 0x0008)
                    self.carry(True)
                else:
                    cpu.reg_write(UC_X86_REG_AX, 0x3000)
                    self.carry(False)
                return
            if ah == 0x49:
                self.carry(False)
                return
        if number == 0x10:
            buffer = cpu.reg_read(UC_X86_REG_ES)*16+cpu.reg_read(UC_X86_REG_DI)
            if ax == 0x4F00:
                cpu.mem_write(buffer, bytes(self.controller))
                cpu.reg_write(UC_X86_REG_AX, 0x004F)
                return
            if ax == 0x4F15:
                if self.edid is None:
                    cpu.reg_write(UC_X86_REG_AX, 0x014F)
                else:
                    cpu.mem_write(buffer, bytes(self.edid))
                    cpu.reg_write(UC_X86_REG_AX, 0x004F)
                return
            if ax == 0x4F01:
                mode = cpu.reg_read(UC_X86_REG_CX)
                self.mode_queries.append(mode)
                info = self.descriptors.get(mode)
                if info is None:
                    cpu.reg_write(UC_X86_REG_AX, 0x014F)
                else:
                    cpu.mem_write(buffer, info)
                    cpu.reg_write(UC_X86_REG_AX, 0x004F)
                return
            if ax == 0x4F02:
                bx = cpu.reg_read(UC_X86_REG_BX)
                mode = bx & 0x3FFF
                self.mode_attempts.append((mode, bool(bx & 0x4000)))
                if mode in self.fail_set:
                    cpu.reg_write(UC_X86_REG_AX, 0x014F)
                    return
                self.current_mode = mode
                self.current_linear = bool(bx & 0x4000)
                cpu.reg_write(UC_X86_REG_AX, 0x004F)
                return
            if ax == 0x4F03:
                self.readbacks.append(self.current_mode)
                if self.current_mode in self.fail_readback:
                    cpu.reg_write(UC_X86_REG_AX, 0x014F)
                    return
                flags = 0x4000 if self.current_linear else 0
                cpu.reg_write(UC_X86_REG_BX, self.current_mode | flags)
                cpu.reg_write(UC_X86_REG_AX, 0x004F)
                return
            if ax == 0x4F06:
                info = self.descriptors[self.current_mode]
                pitch = struct.unpack_from('<H', info, 16)[0]
                cpu.reg_write(UC_X86_REG_BX, pitch)
                cpu.reg_write(UC_X86_REG_CX, struct.unpack_from('<H', info, 18)[0])
                cpu.reg_write(UC_X86_REG_DX, struct.unpack_from('<H', info, 20)[0])
                cpu.reg_write(UC_X86_REG_AX, 0x004F)
                return
            if ax in (0x4F05, 0x4F07, 0x4F09):
                cpu.reg_write(UC_X86_REG_AX, 0x004F)
                return
            if ax == 3:
                self.text_restores += 1
                self.current_mode = 3
                self.current_linear = False
                return
        if number == 0x33:
            return
        if number == 0x10 and ax == 0x1130:
            cpu.reg_write(UC_X86_REG_ES, 0xF000)
            cpu.reg_write(UC_X86_REG_BP, 0)
            return
        super().interrupt(cpu, number, data)


def run_startup(video):
    video.set('vc_min_depth', 15, 1)
    video.call('vc_auto')
    error = video.call('vc_desktop_begin')
    return error


def mode_list(video):
    count = video.get('vc_mode_count')
    return [int.from_bytes(video.cpu.mem_read(
        video.symbols['vc_mode_list'] + i*2, 2), 'little') for i in range(count)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    binary = out/'fixture.com'
    subprocess.run(['nasm', '-f', 'bin', 'scripts/fixtures/vbe_auto_cpu.asm',
                    '-o', str(binary)], cwd=ROOT, check=True)
    code = binary.read_bytes()
    checks = []

    def select(label, edid, expected_mode, expected_cap, modes=None):
        video = SelectionVideo(code, edid, modes)
        video.call('vc_auto')
        assert video.get('vc_mode') == expected_mode, (label, video.get('vc_mode'))
        assert (video.get('vc_max_width'), video.get('vc_max_height')) == expected_cap
        assert video.get('vc_mode_count') == len(video.modes)
        assert set(video.queries) == set(video.modes), 'The complete firmware mode list must remain available'
        checks.append(label)
        return video

    for label, data in (('DDC unsupported', None), ('Invalid EDID header', bytes([255])*128)):
        select(label+' bounds AUTO to 1024x768', data, 0x142, (1024, 768))
    for offset, value, label, repair_checksum in (
        (127, 1, 'Invalid checksum', False), (18, 2, 'Unknown EDID version', True),
        (24, 0, 'No preferred timing flag', True), (54, 0, 'Zero preferred pixel clock', True)):
        data = preferred_edid()
        data[offset] = value
        if offset == 54:
            data[55] = 0
        if repair_checksum:
            data[127] = 0; data[127] = (-sum(data)) & 255
        select(label+' bounds AUTO to 1024x768', data, 0x142, (1024, 768))
    select('Valid 1024x768 EDID retains native bound', preferred_edid(), 0x142, (1024, 768))
    select('Valid 2K EDID retains high resolution', preferred_edid(2560, 1440), 0x143, (2560, 1440))
    for data in (None, preferred_edid()):
        video = SelectionVideo(code, data)
        video.call('vc_resolve_mode', ax=2560, dx=1440)
        assert video.get('vc_mode') == 0x143
    checks.append('Explicit 2K profile resolves above absent or smaller EDID preference')
    select('Missing 800x600 mode retains an available safe lower mode under XGA fallback', None, 0x140, (1024, 768),
           {0x140: (640, 480), 0x143: (2560, 1440)})
    select('No mode within XGA fallback bound returns zero for VGA fallback', None, 0, (1024, 768),
           {0x143: (2560, 1440)})

    startup = StartupVideo(code)
    startup.set('vc_min_depth', 15, 1)
    startup.call('vc_auto')
    assert startup.get('vc_mode') == 0x11C, (
        'AUTO chooses largest in-bound area, then greatest direct-color depth',
        hex(startup.get('vc_mode')))
    assert startup.call('vc_desktop_begin') is False
    assert startup.get('vc_active', 1) == 1
    assert startup.get('vc_mode') == 0x11C
    assert startup.get('vc_start_attempts', 1) == 1
    assert startup.mode_attempts == [(0x11C, False)]
    assert mode_list(startup) == list(STARTUP_MODES)
    checks.append('Startup AUTO prefers largest allowed direct-color geometry/depth; 8bpp is excluded')

    for failure_kind, options, expected_reads in (
        ('4F02 selected-mode refusal', {'fail_set': {0x11C}}, [0x11C, 0x11B]),
        ('4F03 selected-mode readback refusal', {'fail_readback': {0x11C}}, [0x11C, 0x11B]),
        ('first DOS allocation refusal', {'fail_allocations': 1}, [0x11B]),
    ):
        startup = StartupVideo(code, **options)
        assert run_startup(startup) is False, failure_kind
        assert startup.get('vc_mode') == 0x11B, (failure_kind, hex(startup.get('vc_mode')))
        assert startup.get('vc_active', 1) == 1
        assert startup.get('vc_start_attempts', 1) == 2
        assert [mode for mode, _linear in startup.mode_attempts] == expected_reads
        assert mode_list(startup) == list(STARTUP_MODES), 'failed-mode exclusion markers leaked'
        assert len(set(startup.mode_attempts)) == len(startup.mode_attempts), (
            failure_kind, startup.mode_attempts)
        checks.append(f'Failed highest candidate retries once and restores list markers: {failure_kind}')

    startup = StartupVideo(code, modes={0x119: (1024, 768, 15),
                                        0x11A: (1024, 768, 16)},
                           fail_set={0x119, 0x11A})
    assert run_startup(startup) is True, 'all-fail startup must return carry set'
    assert startup.get('vc_mode') == 0
    assert startup.get('vc_active', 1) == 0
    assert startup.get('vc_start_attempts', 1) == 2
    assert [mode for mode, _linear in startup.mode_attempts] == [0x11A, 0x119]
    assert mode_list(startup) == [0x119, 0x11A], 'all-fail left list exclusions set'
    assert startup.text_restores == 2, startup.text_restores
    checks.append('All direct-color startup candidates fail boundedly and restore BIOS VGA text')

    explicit = StartupVideo(code, fail_set={0x11C})
    explicit.set('vc_mode', 0x11C)
    assert explicit.call('vc_begin') is True
    assert [mode for mode, _linear in explicit.mode_attempts] == [0x11C]
    assert explicit.get('vc_mode') == 0x11C
    assert explicit.get('vc_start_attempts', 1) == 0
    checks.append('Explicit vc_begin failure preserves exact requested mode without substitution')

    bounded = StartupVideo(code, edid=None)
    bounded.set('vc_min_depth', 15, 1)
    bounded.call('vc_auto')
    assert (bounded.get('vc_max_width'), bounded.get('vc_max_height')) == (1024, 768)
    assert bounded.get('vc_mode') == 0x11C
    checks.append('No-EDID startup stays at or below 1024x768 while retaining direct color')

    flagged_ids = [0x8142, 0x9142, 0x4142]
    valid_modes = {0x119: (1024, 768, 15), 0x11A: (1024, 768, 16)}
    startup = StartupVideo(
        code, modes=valid_modes, fail_set={0x11A},
        raw_mode_ids=flagged_ids + list(valid_modes))
    original_rom_list = startup.cpu.mem_read(
        0x50000, 2*(len(flagged_ids)+len(valid_modes)+1))
    startup.set('vc_min_depth', 15, 1)
    startup.call('vc_auto')
    assert startup.get('vc_mode_count') == len(valid_modes)
    assert mode_list(startup) == list(valid_modes)
    assert not set(flagged_ids).intersection(startup.mode_queries), startup.mode_queries
    assert startup.get('vc_mode') == 0x11A
    assert startup.call('vc_desktop_begin') is False
    assert startup.mode_attempts == [(0x11A, False), (0x119, False)]
    assert mode_list(startup) == list(valid_modes), 'retry did not restore filtered list markers'
    assert startup.cpu.mem_read(
        0x50000, len(original_rom_list)) == original_rom_list, 'firmware mode list was modified'
    assert not set(flagged_ids).intersection(startup.mode_queries), startup.mode_queries
    checks.append('Flagged ROM mode IDs are filtered, never queried, and remain untouched across retry')

    report = dict(passed=True, checks=checks,
                  scope='Production NASM selection/startup instructions; VBE descriptors, BIOS failures, and EDID supplied by the Unicorn fixture',
                  source_sha256=digest(ROOT/'src/com/vbe_modes.inc'),
                  fixture_sha256=digest(binary))
    (out/'result.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


if __name__ == '__main__':
    main()
