#!/usr/bin/env python3
"""Doom frame rate inside a desktop DOS window: `-timedemo demo1`.

Doom prints "timed N gametics in M realtics" (35 realtics per second) when
the demo ends; fps = 35 * N / M. Same-host QEMU/KVM comparison only.
"""
import argparse, json, re, shutil, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qemu_test_native_windows import WindowVM


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--game', default='\\APPS\\DOOMVAN\\PCDMCORE.EXE -timedemo demo1 -nosound')
    p.add_argument('--timeout', type=int, default=900)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    disk = a.output / 'disk.img'
    shutil.copyfile(a.image, disk)
    vm = WindowVM(disk, a.output, 'std', memory=128, palette='platinum', boot_capture=True)
    report = {'passed': False, 'game': a.game}
    try:
        vm.ready()
        before = vm.offset()
        vm.hmp('sendkey meta_l-r 180'); time.sleep(1)
        vm.wait('WINDOW 01 OPEN', before, 20)
        for ch in a.game:
            key = {'\\': 'backslash', '.': 'dot', ' ': 'spc', '-': 'minus'}.get(
                ch, 'shift-' + ch.lower() if ch.isupper() else ch)
            vm.hmp(f'sendkey {key} 90'); time.sleep(.14)
        vm.hmp('sendkey ret 180')
        vm.wait('ST_Init: Init status bar.', before, 120)
        started = time.monotonic()
        vm.wait('realtics', before, a.timeout)
        text = vm.serial.read_bytes()[before:].decode('cp437', 'replace')
        m = re.search(r'timed (\d+) gametics in (\d+) realtics', text)
        assert m, 'no timedemo result'
        gametics, realtics = int(m.group(1)), int(m.group(2))
        report.update(gametics=gametics, realtics=realtics,
                      fps=round(35 * gametics / realtics, 2),
                      host_seconds=round(time.monotonic() - started, 1), passed=True)
        print(f"[timedemo] {report['fps']} fps ({gametics} gametics in {realtics} realtics)")
    except Exception as error:
        report['error'] = repr(error)
        raise
    finally:
        vm.close()
        disk.unlink(missing_ok=True)
        (a.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
