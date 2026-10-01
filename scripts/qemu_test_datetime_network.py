#!/usr/bin/env python3
"""Focused QEMU check for the taskbar clock and network time-zone lookup."""
import argparse
import json
import shutil
import subprocess
import time
from pathlib import Path

from qemu_test_full_display_profile import VM
from qemu_test_native_windows import WindowVM


def click(vm, x, y, button=1):
    vm.position(x, y)
    vm.hmp(f'mouse_button {button}')
    time.sleep(.18)
    vm.hmp('mouse_button 0')
    time.sleep(.5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    disk = output / 'disk.img'
    shutil.copyfile(args.image, disk)
    result = {'passed': False, 'memory_mb': 128, 'qemu_instances': 1}
    vm = WindowVM.__new__(WindowVM)
    vm.palette = 'platinum'
    vm.cursor_colors = ((36, 40, 48), (246, 246, 242))
    vm.control_latencies = []
    VM.__init__(vm, disk, output, qemu_args=[
        '-vga', 'std', '-netdev', 'user,id=ciuknet0',
        '-device', 'ne2k_pci,netdev=ciuknet0,mac=52:54:00:12:34:56',
    ], memory=128)
    try:
        vm.ready()
        vm.shot('clock-with-date')
        offset = vm.offset()
        click(vm, 850, 16)
        vm.wait('[CONTROL] open Network', offset, 20)
        offset = vm.offset()
        click(vm, 868, 582)
        vm.wait('NETCFG: DHCP lease saved', offset, 100)
        vm.wait('[DESKTOP] READY', offset, 40)

        offset = vm.offset()
        click(vm, 1170, 783, 2)
        vm.wait('[CONTROL] open Date and Time', offset, 25)
        vm.shot('date-time-settings')
        offset = vm.offset()
        click(vm, 540, 594)
        vm.wait('[CONTROL] time zone lookup worldtime.timezone.io', offset, 30)
        vm.wait('[CONTROL] time zone detected', offset, 90)
        vm.shot('time-zone-detected')
        reply = subprocess.check_output(['mtype', '-i', str(disk), '::NET/TIME.TXT'],
                                        text=True, errors='replace')
        zone = next(line[10:].strip() for line in reply.splitlines()
                    if line.startswith('timezone: '))
        assert zone and 'datetime: ' in reply
        result['detected_zone'] = zone

        click(vm, 475, 537)
        click(vm, 843, 594)
        saved = subprocess.check_output(['mtype', '-i', str(disk),
                                         '::SYSTEM/UI/TIMEZONE.CFG'])
        assert saved == b'1', f'automatic setting not saved: {saved!r}'
        result['automatic_setting'] = 'saved'
        click(vm, 666, 594)
        offset = vm.offset()
        click(vm, 1170, 783, 2)
        vm.wait('[CONTROL] time zone lookup worldtime.timezone.io', offset, 30)
        vm.wait('[CONTROL] time zone detected', offset, 90)
        result['automatic_on_reopen'] = 'passed'
        result['passed'] = True
    except Exception as exc:
        result['error'] = repr(exc)
        try:
            vm.shot('failure')
        except Exception:
            pass
    finally:
        vm.close()
    (output / 'report.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
