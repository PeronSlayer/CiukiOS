#!/usr/bin/env python3
"""Check production AUTO selection with absent, invalid and valid monitor data."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

from unicorn.x86_const import UC_X86_REG_AX, UC_X86_REG_CX, UC_X86_REG_ES, UC_X86_REG_DI
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
        select(label+' bounds AUTO to 800x600', data, 0x141, (800, 600))
    for offset, value, label, repair_checksum in (
        (127, 1, 'Invalid checksum', False), (18, 2, 'Unknown EDID version', True),
        (24, 0, 'No preferred timing flag', True), (54, 0, 'Zero preferred pixel clock', True)):
        data = preferred_edid()
        data[offset] = value
        if offset == 54:
            data[55] = 0
        if repair_checksum:
            data[127] = 0; data[127] = (-sum(data)) & 255
        select(label+' bounds AUTO to 800x600', data, 0x141, (800, 600))
    select('Valid 1024x768 EDID retains native bound', preferred_edid(), 0x142, (1024, 768))
    select('Valid 2K EDID retains high resolution', preferred_edid(2560, 1440), 0x143, (2560, 1440))
    for data in (None, preferred_edid()):
        video = SelectionVideo(code, data)
        video.call('vc_resolve_mode', ax=2560, dx=1440)
        assert video.get('vc_mode') == 0x143
    checks.append('Explicit 2K profile resolves above absent or smaller EDID preference')
    select('Missing 800x600 mode retains an available safe lower mode', None, 0x140, (800, 600),
           {0x140: (640, 480), 0x143: (2560, 1440)})
    select('No mode within unknown-monitor bound returns zero for VGA fallback', None, 0, (800, 600),
           {0x143: (2560, 1440)})
    report = dict(passed=True, checks=checks,
                  scope='Production CPU selection; BIOS descriptors and EDID supplied by the fixture',
                  source_sha256=digest(ROOT/'src/com/vbe_modes.inc'),
                  fixture_sha256=digest(binary))
    (out/'result.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


if __name__ == '__main__':
    main()
