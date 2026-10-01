#!/usr/bin/env python3
"""QEMU gate for COM/MZ BIOS text programs in independent M4 DOS VMs."""
import argparse
import json
import re
import shutil
import subprocess
import time
from pathlib import Path

import numpy as np
from PIL import Image

from qemu_test_installed_hdd import FAT16
from qemu_test_native_windows import WindowVM

ROOT = Path(__file__).resolve().parent.parent


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    disk = a.output / 'disk.img'
    shutil.copyfile(a.image, disk)
    probes = a.output / 'probes'
    subprocess.run(['bash', str(ROOT / 'src/probes/doswindow/build.sh'),
                    str(probes)], check=True, stdout=subprocess.DEVNULL)
    volume = f'{disk}@@{FAT16(disk).start}'
    for name in ('DWBIOST.COM', 'DWBIOST.EXE', 'DWBIOSCH.COM'):
        subprocess.run(['mcopy', '-o', '-i', volume, str(probes / name),
                        f'::APPS/{name}'], check=True)
    vm = WindowVM(disk, a.output, 'std', boot_capture=True,
                  memory=128, palette='platinum')
    report = {'passed': False, 'host': 'QEMU', 'checks': {}}

    def serial(offset=0):
        return subprocess.check_output([
            str(ROOT / 'scripts/serial_log_normalize.py'), '--offset',
            str(offset), str(vm.serial)]).decode('cp437', 'replace')

    def send(key):
        vm.hmp(f'sendkey {key} 180')
        time.sleep(.25)

    try:
        vm.ready()
        for name, kind in (('DWBIOST.COM', 'BIOS-COM'),
                           ('DWBIOST.EXE', 'BIOS-MZ')):
            before = vm.offset()
            send('meta_l-r')
            vm.wait('WINDOW 01 OPEN', before, 20)
            vm.text('\\APPS\\' + name)
            send('ret')
            marker = '[DOSWIN:' + kind + '] '
            vm.wait(marker + 'START psp=', before, 45)
            vm.wait(marker + 'FILE OK', before, 30)
            vm.wait(marker + 'EXEC RETURN', before, 40)
            vm.wait(marker + 'FILE MIDRUN OK', before, 45)
            frame = np.asarray(Image.open(vm.shot(kind.lower() + '-text')).convert('RGB'))
            blue = int(((frame[:, :, 0] == 0) & (frame[:, :, 1] == 0) &
                        (frame[:, :, 2] == 170)).sum())
            white = int((frame[:, :, :3] == 255).all(axis=2).sum())
            assert blue > 200000 and white > 1000, (blue, white)
            send('esc')
            vm.wait(marker + 'END', before, 20)
            vm.wait('[DOSVM] ended', before, 25)
            log = serial(before)
            assert marker + 'FAIL' not in log
            assert re.search(re.escape(marker) + r'END ticks=([0-9A-F]{4})', log)
            report['checks'][kind] = {
                'file_roundtrip': True, 'nested_exec': True,
                'midrun_file_io': True, 'bios_text_pixels': blue,
                'bounded_exit': True,
            }
            # The first finished VM stays in its window until the next Run.
        report['passed'] = True
    except Exception as exc:
        report['error'] = repr(exc)
        try:
            vm.shot('failure')
        except Exception:
            pass
        raise
    finally:
        (a.output / 'serial.log').write_text(serial())
        vm.close()
        (a.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('[m4-text] PASS', a.output)


if __name__ == '__main__':
    main()
