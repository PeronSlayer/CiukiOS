#!/usr/bin/env python3
"""QEMU gate for DPMIPORT in a forked desktop DOS VM."""
import argparse
import json
import shutil
import subprocess
import time
from pathlib import Path

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
    subprocess.run(['bash', str(ROOT / 'scripts/build_dpmi_lifetime_probes.sh'),
                    str(probes)], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['mcopy', '-o', '-i', f'{disk}@@{FAT16(disk).start}',
                    str(probes / 'DPMIPORT.EXE'), '::APPS/DPMIPORT.EXE'], check=True)
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
        before = vm.offset()
        send('meta_l-r')
        vm.wait('WINDOW 01 OPEN', before, 20)
        vm.text('\\APPS\\DPMIPORT.EXE')
        send('ret')
        vm.wait('[DOSVM] fork', before, 45)
        vm.wait('[DPMIPORT] START', before, 60)
        vm.wait('[DPMIPORT] PASS', before, 60)
        vm.wait('[DOSVM] ended', before, 30)
        log = serial(before)
        assert '[DPMIPORT] FAIL' not in log, log
        assert '[DESKTOP] READY' in serial(), 'desktop did not remain active'
        report['checks'] = {'protected_mode_vga_ports': True,
                            'desktop_remained_active': True,
                            'clean_guest_exit': True}
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
    print('[m4-dpmi] PASS', a.output)


if __name__ == '__main__':
    main()
