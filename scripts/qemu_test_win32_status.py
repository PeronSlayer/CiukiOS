#!/usr/bin/env python3
"""Check that free Win32 probes receive a clear, bounded QEMU response."""
import argparse
import json
import shutil
import subprocess
import time
from pathlib import Path

from qemu_test_native_windows import WindowVM


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    probes = out / 'probes'
    subprocess.run(['bash', 'scripts/build_win32_probes.sh', str(probes)], check=True)
    disk = out / 'win32-status.img'
    shutil.copyfile(args.image, disk)
    subprocess.run(['mmd', '-i', str(disk), '::SYSTEM/TEST/WIN32'], check=True)
    for name in ('HELLO', 'SETUP'):
        subprocess.run(['mcopy', '-i', str(disk), str(probes / (name + '.EXE')),
                        '::SYSTEM/TEST/WIN32/'], check=True)
    vm = WindowVM(disk, out, 'std', memory=128, palette='platinum')
    report = {'image': str(args.image.resolve()), 'checks': {}}
    try:
        vm.ready()
        vm.key('alt-f4')
        for name in ('HELLO', 'SETUP'):
            offset = vm.offset()
            vm.key('meta_l-r')
            vm.text('C:\\SYSTEM\\TEST\\WIN32\\' + name + '.EXE')
            vm.wait('[DOSVM] Windows executable', offset, 20)
            time.sleep(.8)
            vm.shot(name.lower() + '-unsupported')
            # The shell owns this close; no DOS guest was forked.
            vm.position(1050, 273)
            vm.hmp('mouse_button 1')
            time.sleep(.15)
            vm.hmp('mouse_button 0')
            vm.wait('[DESKTOP] WINDOW 11 CLOSE', offset, 20)
            report['checks'][name.lower() + '_bounded_response'] = True
        serial = vm.serial.read_text(errors='replace')
        assert '[DOSVM] fork' not in serial, 'A PE executable was incorrectly passed to a DOS VM'
        report['checks']['no_guest_fork'] = True
    finally:
        vm.close()
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
