#!/usr/bin/env python3
"""Save, reboot, read and delete a CiukiOS registry value in QEMU."""
import argparse
import json
import shutil
import struct
import subprocess
import time
from pathlib import Path

from qemu_test_native_windows import WindowVM


def click(vm, x, y):
    vm.position(x, y)
    vm.hmp('mouse_button 1')
    time.sleep(.15)
    vm.hmp('mouse_button 0')
    time.sleep(.55)


def double_click(vm, x, y):
    vm.position(x, y)
    for _ in range(2):
        vm.hmp('mouse_button 1')
        time.sleep(.05)
        vm.hmp('mouse_button 0')
        time.sleep(.05)
    time.sleep(.8)


def open_registry(vm):
    vm.key('alt-f4')  # Close the library so the desktop icon receives input.
    time.sleep(.6)
    double_click(vm, 50, 300)
    vm.wait('[DESKTOP] WINDOW 13 OPEN', 0, 20)
    time.sleep(.5)
    double_click(vm, 580, 620)
    vm.wait('[CONTROL] applet Settings Registry', 0, 20)
    time.sleep(1)


def records(disk):
    data = subprocess.check_output(['mcopy', '-i', str(disk),
                                    '::SYSTEM/CONFIG/REGISTRY.DAT', '-'])
    assert data[:8] == b'CIREG01\n', data[:8]
    out = []
    pos = 8
    while pos < len(data):
        magic, op, kind, nk, nn, nd, check = struct.unpack_from('<HBBHHHH', data, pos)
        assert magic == 0x5243 and op in (1, 2)
        pos += 12
        key, name, value = data[pos:pos+nk], data[pos+nk:pos+nk+nn], data[pos+nk+nn:pos+nk+nn+nd]
        pos += nk + nn + nd
        digest = op + kind + nk + nn + nd
        for byte in key + name + value:
            digest = (digest * 33 + byte) & 0xffff
        assert check == digest, (check, digest)
        out.append((op, kind, key, name, value))
    assert pos == len(data)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    disk = out / 'registry.img'
    shutil.copyfile(args.image, disk)
    report = {'image': str(args.image.resolve()), 'checks': {}}
    for phase in ('save', 'read-delete'):
        phase_dir = out / phase
        phase_dir.mkdir(exist_ok=True)
        vm = WindowVM(disk, phase_dir, 'std', memory=128, palette='platinum')
        try:
            vm.ready()
            open_registry(vm)
            if phase == 'save':
                click(vm, 500, 435)
                for char in 'registry test':
                    vm.key('spc' if char == ' ' else char)
                click(vm, 580, 527)
                vm.wait('[REGISTRY] saved HKCU\\Software\\CiukiOS', 0, 20)
                vm.shot('saved')
                report['checks']['save'] = True
            else:
                click(vm, 480, 527)
                vm.wait('[REGISTRY] read HKCU\\Software\\CiukiOS', 0, 20)
                vm.shot('persisted-after-reboot')
                report['checks']['read_after_reboot'] = True
                click(vm, 680, 527)
                vm.wait('[REGISTRY] delete HKCU\\Software\\CiukiOS', 0, 20)
                vm.shot('deleted')
                report['checks']['delete'] = True
        finally:
            vm.close()
        recs = records(disk)
        if phase == 'save':
            assert recs[-1] == (1, 1, b'HKCU\\Software\\CiukiOS', b'Example', b'registry test\0')
        else:
            assert recs[-1] == (2, 0, b'HKCU\\Software\\CiukiOS', b'Example', b'')
        report['checks']['valid_disk_record_' + phase] = True
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
