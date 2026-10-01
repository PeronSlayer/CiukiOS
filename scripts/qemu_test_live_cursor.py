#!/usr/bin/env python3
"""Measure software pointer response with an idle M4 DOS program open."""
import argparse
import json
import shutil
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qemu_test_native_windows import WindowVM  # noqa: E402


def moved(vm):
    x0, y0 = vm.pointer()
    start = time.monotonic()
    vm.hmp('mouse_move 18 0 0')
    deadline = start + 2.0
    while time.monotonic() < deadline:
        x, y = vm.pointer()
        if x >= x0 + 12 and abs(y - y0) <= 3:
            return round(time.monotonic() - start, 3)
    raise AssertionError('desktop pointer did not visibly consume the PS/2 packet within 2s')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    disk = out / 'disk.img'
    shutil.copyfile(a.image, disk)
    report = {'passed': False, 'host': 'QEMU', 'physical_hardware_qualified': False}
    vm = WindowVM(disk, out, 'std', memory=128, palette='platinum')
    try:
        vm.ready()
        report['desktop_seconds'] = moved(vm)
        before = vm.offset()
        vm.key('meta_l-r')
        vm.text('\\COMMAND.COM')
        vm.key('ret')
        vm.wait('[DOSVM] video ready', before, 45)
        time.sleep(0.5)
        report['dos_window_seconds'] = moved(vm)
        vm.shot('cursor-with-dos')
        report['passed'] = report['dos_window_seconds'] < 1.25
        if not report['passed']:
            raise AssertionError('DOS window pointer response exceeded 1.25s')
    except Exception as error:
        report['error'] = repr(error)
        try:
            vm.shot('failure')
        except Exception:
            pass
    finally:
        vm.close()
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
