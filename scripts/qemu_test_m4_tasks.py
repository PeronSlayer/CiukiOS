#!/usr/bin/env python3
"""QEMU gate for Task Manager focus and End VM on an M4 DOS window."""
import argparse
import json
import shutil
import struct
import subprocess
import time
from pathlib import Path

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
    vm = WindowVM(disk, a.output, 'std', memory=128, palette='platinum')
    report = {'passed': False, 'host': 'QEMU', 'checks': {}}

    def serial(offset=0):
        return subprocess.check_output([
            str(ROOT / 'scripts/serial_log_normalize.py'), '--offset',
            str(offset), str(vm.serial)]).decode('cp437', 'replace')

    def send(key):
        vm.hmp(f'sendkey {key} 180')
        time.sleep(.3)

    def focus():
        dump = a.output / 'vm-status.bin'
        vm.hmp(f'pmemsave 0 0x200000 "{dump}"')
        data = dump.read_bytes()
        dump.unlink()
        marker = data.find(b'CVMMSTAT')
        assert marker >= 0
        return struct.unpack_from('<I', data, marker + 0x28)[0]

    def wait_focus(wanted, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if focus() == wanted:
                return
            time.sleep(.2)
        raise AssertionError(f'focus remained {focus()}, wanted {wanted}')

    try:
        vm.ready()
        before = vm.offset()
        send('meta_l-r')
        vm.wait('WINDOW 01 OPEN', before, 20)
        vm.text('\\COMMAND.COM')
        send('ret')
        vm.wait('[DOSVM] video ready', before, 60)
        send('ctrl-esc')
        wait_focus(0)
        before = vm.offset()
        send('ctrl-shift-esc')
        vm.wait('WINDOW 09 OPEN', before, 20)
        send('right')
        send('right')
        vm.wait('[TASKS] tab Virtual Machines', before, 15)
        send('down')          # VM 1 (row zero is the system VM)
        time.sleep(1)
        vm.shot('task-manager-vms')
        send('ret')           # Give Focus
        wait_focus(1)
        time.sleep(1)
        assert focus() == 1, 'Task Manager focus did not persist'
        report['checks']['give_focus'] = True
        send('ctrl-esc')
        wait_focus(0)
        send('delete')        # End VM for selected row
        vm.wait('[DOSVM] ended', before, 20)
        report['checks']['end_vm'] = True
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
    print('[m4-tasks] PASS', a.output)


if __name__ == '__main__':
    main()
