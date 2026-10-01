#!/usr/bin/env python3
"""Measure the desktop pointer while an M4 DOOM window updates video."""
import argparse
import json
import shutil
import time
from pathlib import Path

from qemu_test_live_cursor import moved
from qemu_test_native_windows import WindowVM


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
        vm.position(220, 150)
        report['desktop_seconds'] = moved(vm)
        before = vm.offset()
        vm.key('meta_l-r')
        vm.wait('WINDOW 01 OPEN', before, 20)
        vm.text('\\APPS\\DOOM\\DOOMCORE.EXE -warp 1 1 -nomusic')
        vm.key('ret')
        vm.wait('ST_Init: Init status bar.', before, 120)
        vm.wait('startmap: 1', before, 120)
        time.sleep(5)  # sample while the first level is being rendered
        vm.position(220, 150)  # outside the DOS client, pointer still desktop-owned
        report['doom_window_seconds'] = moved(vm)
        vm.shot('cursor-with-doom')
        report['passed'] = report['doom_window_seconds'] < 0.5
        if not report['passed']:
            raise AssertionError('pointer response while DOOM runs exceeded 0.5s')
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
