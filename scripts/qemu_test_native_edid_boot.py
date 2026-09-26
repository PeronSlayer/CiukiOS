#!/usr/bin/env python3
"""Observe AUTO on the shipped CD with EDID configured before the first boot.

No guest binary is substituted. The current SHELL listing is used to decode RAM
only after verifying that the assembled binary matches the CD payload exactly.
This exercises monitor selection in QEMU, not physical LCD/BIOS compatibility.
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import time
from unittest.mock import patch

from qemu_test_cd_sessions import Session, digest
from qemu_test_installed_hdd import FAT16


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--edid', choices=('off', '1024x768', '2560x1440'), required=True)
    parser.add_argument('--expect-mode', required=True)
    parser.add_argument('--explicit-2k', action='store_true',
                        help='Preview and apply 2560x1440 through the actual VGASETUP command after AUTO boot')
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='kvm')
    args = parser.parse_args()
    out, iso = args.output.resolve(), args.iso.resolve()
    out.mkdir(parents=True, exist_ok=False)
    packed, raw = out/'cd.img.gz', out/'cd.img'
    subprocess.run(['xorriso', '-osirrox', 'on', '-indev', str(iso),
                    '-extract', '/ciukios-full-cd-disk.img.gz', str(packed)],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    raw.write_bytes(gzip.decompress(packed.read_bytes()))
    shipped = FAT16(raw).read('SYSTEM/SHELL.COM')
    subprocess.run(['nasm', '-f', 'bin', 'src/com/shell.asm',
                    '-o', str(out/'current.com'), '-l', str(out/'current.lst')], check=True)
    assert (out/'current.com').read_bytes() == shipped, 'Current listing does not match the ISO shell'
    lines = (out/'current.lst').read_text().splitlines()
    offsets = {}
    for line in lines:
        match = re.match(r'\s*\d+\s+([0-9A-F]{8})\s.*\b(vc_\w+)\s+(?:d[bwd]|times)\b', line)
        if match:
            offsets[match[2]] = int(match[1], 16)
    report = dict(iso_sha256=digest(iso), shell_sha256=hashlib.sha256(shipped).hexdigest(),
                  edid_device=args.edid, scope='Actual immutable CD first boot; no guest payload overrides')
    launch = subprocess.Popen
    def configured(command, *positional, **keywords):
        device = 'VGA,vgamem_mb=32,'
        if args.edid == 'off':
            device += 'edid=off'
        else:
            width, height = args.edid.split('x')
            device += f'edid=on,xres={width},yres={height}'
        index = command.index('-vga')
        command[index:index+2] = ['-vga', 'none', '-device', device]
        return launch(command, *positional, **keywords)
    with patch('subprocess.Popen', configured):
        vm = Session(iso, out/'vm', 'live', args.accel)
    report['qemu_command'] = vm.command
    try:
        vm.connect()
        vm.wait(b'[DESKTOP] READY', timeout=90)
        vm.settled_desktop('first-desktop')
        expected = list(map(int, args.expect_mode.split('x')))
        assert vm.report['first-desktop']['size'] == expected, vm.report
        vm.hmp('stop')
        vm.hmp(f'pmemsave 0 0x100000 "{out}/ram.bin"')
        ram = (out/'ram.bin').read_bytes()
        signature = shipped[:48]
        start = ram.find(signature)
        assert start >= 0 and ram.find(signature, start+1) == -1, 'Ambiguous resident shell'
        def blob(name, length):
            address = start+offsets[name]
            return ram[address:address+length]
        state = {name: int.from_bytes(blob(name, size), 'little') for name, size in (
            ('vc_auto_done', 1), ('vc_mode', 2), ('vc_best_mode', 2),
            ('vc_max_width', 2), ('vc_max_height', 2), ('vc_lfb', 1),
            ('vc_active', 1), ('vc_mode_count', 2))}
        info = blob('vc_info', 256)
        state['descriptor_geometry'] = list(struct.unpack_from('<HH', info, 18))
        state['bpp'] = info[25]
        edid = blob('vc_edid', 128)
        valid_header = edid[:8] == bytes.fromhex('00ffffffffffff00')
        report['edid'] = dict(hex=edid.hex(), valid_header=valid_header, checksum=sum(edid) & 255)
        report['first_boot_state'] = state
        assert state['vc_active'] == state['vc_auto_done'] == 1
        assert state['descriptor_geometry'] == expected
        if args.edid == 'off':
            assert not valid_header or sum(edid) & 255
        else:
            assert valid_header and not (sum(edid) & 255)
            assert [state['vc_max_width'], state['vc_max_height']] == expected
        vm.hmp('cont')
        mark = vm.offset(); vm.key('f4'); vm.wait(b'CiukiOS SHELL ', mark)
        mark = vm.offset(); vm.text('comdemo')
        vm.wait(b'COM demo via INT21h', mark); vm.wait(b'CiukiOS SHELL ', mark)
        report['comdemo_executed'] = True
        if args.explicit_2k:
            mark = vm.offset(); vm.text('vgasetup desktop 2560')
            vm.wait(b'[VGASETUP] DESKTOP PREVIEW ', mark)
            time.sleep(.6)
            assert vm.shot('explicit-2k-preview').size == (2560, 1440)
            vm.key('ret')
            vm.wait(b'Desktop resolution saved; Windows profile preserved.', mark)
            vm.wait(b'CiukiOS SHELL ', mark)
            expected = [2560, 1440]
            report['explicit_2k_preview_confirmed'] = True
        mark = vm.offset(); vm.text('exit'); vm.wait(b'[DESKTOP] READY', mark)
        vm.settled_desktop('desktop-after-comdemo')
        assert vm.report['desktop-after-comdemo']['size'] == expected
        report['passed'] = True
        vm.report['status'] = 'passed'
    except Exception as error:
        report['passed'] = False
        vm.report['status'] = 'failed'
        report['error'] = repr(error)
        raise
    finally:
        vm.close()
        report['guest'] = vm.report
        (out/'result.json').write_text(json.dumps(report, indent=2)+'\n')
        print(json.dumps(report, indent=2), flush=True)


if __name__ == '__main__':
    main()
