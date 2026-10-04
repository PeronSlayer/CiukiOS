#!/usr/bin/env python3
"""Desktop responsiveness while a DOS program runs in a window.

Measures the host time from a mouse-button release on a desktop icon to the
desktop's completed paint, first with an idle desktop and then while a busy
DOS/4GW game runs in a DOS window. The click alternates between two icons so
every release changes the selection and must produce a paint.
"""
import argparse
import json
import shutil
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qemu_test_native_windows import WindowVM

ICONS = ((66, 74), (66, 156))          # *COMPUTR and *PROGRAM icon centres


def summary(values):
    ms = sorted(round(v * 1000, 1) for v in values)
    return {'samples_ms': ms, 'median_ms': statistics.median(ms), 'max_ms': ms[-1]}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--game', default='\\APPS\\DOOMVAN\\PCDMCORE.EXE')
    p.add_argument('--game-args', default='-warp 1 1 -nomusic')
    p.add_argument('--clicks', type=int, default=10)
    p.add_argument('--memory', type=int, default=128)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    disk = a.output / 'disk.img'
    shutil.copyfile(a.image, disk)
    vm = WindowVM(disk, a.output, 'std', memory=a.memory, palette='platinum',
                  boot_capture=True)
    report = {'passed': False, 'game': a.game, 'game_args': a.game_args}

    def keys(*names):
        for name in names:
            vm.hmp(f'sendkey {name} 180')
            time.sleep(.24)
        time.sleep(.4)

    def type_text(s):
        for ch in s:
            key = {'\\': 'backslash', '.': 'dot', ' ': 'spc', '-': 'minus'}.get(
                ch, 'shift-' + ch.lower() if ch.isupper() else ch)
            vm.hmp(f'sendkey {key} 90')
            time.sleep(.14)

    def pointer_moves(label):
        # Host time from a relative PS/2 move to the moved software cursor on
        # screen (screendump polling: a few tens of ms of resolution).
        values = []
        for n in range(a.clicks):
            before = vm.pointer()
            started = time.monotonic()
            vm.hmp(f'mouse_move {30 if n % 2 == 0 else -30} 0 0')
            while True:
                try:
                    now = vm._pointer()
                except AssertionError:
                    now = before
                if now != before:
                    break
                if time.monotonic() - started > 5:
                    raise AssertionError(f'pointer did not move ({label})')
            values.append(time.monotonic() - started)
            time.sleep(.2)
        report[label] = summary(values)
        print(f'[dos-lag] {label}: median {report[label]["median_ms"]} ms, '
              f'max {report[label]["max_ms"]} ms', flush=True)

    def clicks(label):
        values = []
        for n in range(a.clicks):
            values.append(vm.completed_control_click(*ICONS[n % 2]))
        report[label] = summary(values)
        print(f'[dos-lag] {label}: median {report[label]["median_ms"]} ms, '
              f'max {report[label]["max_ms"]} ms', flush=True)

    try:
        vm.ready()
        clicks('idle_desktop')
        pointer_moves('idle_pointer')
        before = vm.offset()
        keys('meta_l-r')
        vm.wait('WINDOW 01 OPEN', before, 20)
        type_text(a.game + (' ' + a.game_args if a.game_args else ''))
        keys('ret')
        vm.wait('[DOSVM] fork', before, 60)
        vm.wait('ST_Init: Init status bar.', before, 120)
        time.sleep(8)
        vm.shot('game-running')
        clicks('with_dos_game')
        pointer_moves('pointer_with_dos_game')
        vm.shot('after-clicks')
        vm.hmp(f'pmemsave 0 {a.memory * 1048576} "{a.output / "state.bin"}"')
        report['passed'] = True
    except Exception as error:
        report['error'] = repr(error)
        try:
            vm.hmp(f'pmemsave 0 {a.memory * 1048576} "{a.output / "failure.bin"}"')
            vm.shot('failure')
            samples = []
            for _ in range(20):
                text = vm.hmp('info registers').decode('utf-8', 'replace')
                samples.append(' '.join(line.strip() for line in text.splitlines()
                                        if line.startswith(('EIP=', 'CS =', 'EAX=', 'ESI='))))
                time.sleep(.05)
            report['failure_registers'] = samples
        except Exception:
            pass
        raise
    finally:
        vm.close()
        disk.unlink(missing_ok=True)
        (a.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
