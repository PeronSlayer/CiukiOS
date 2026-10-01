#!/usr/bin/env python3
"""QEMU integration gate for native x86 DOS VMs shown in desktop windows."""
import argparse
import json
import re
import struct
import shutil
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qemu_test_native_windows import WindowVM


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--game', default='\\APPS\\DOOM\\DOOMCORE.EXE',
                   help='DOS/4GW game to run in the first VM')
    p.add_argument('--game-args', default='', help='arguments passed to the game')
    p.add_argument('--verify-game-audio', action='store_true',
                   help='require non-silent QEMU PCM during gameplay')
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    disk = a.output / 'disk.img'
    shutil.copyfile(a.image, disk)
    vm = WindowVM(disk, a.output, 'std', memory=128, palette='platinum',
                  boot_capture=a.verify_game_audio)
    report = {'passed': False, 'host': 'QEMU', 'checks': {}}

    def log():
        return subprocess.check_output(
            [str(Path(__file__).parent / 'serial_log_normalize.py'),
             '--offset', '0', str(vm.serial)]).decode('cp437', 'replace')

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

    def click(x, y):
        # Wait for each injected packet to reach the software cursor before
        # sending the next one. Otherwise a delayed repaint makes a feedback
        # controller inject the same packet repeatedly and overshoot.
        # The cursor can briefly blend into a moving game's palette. QEMU's
        # absolute PS/2 placement is a fallback; the following serial action
        # checks still require the intended control to respond.
        try:
            vm.pointer()
        except AssertionError:
            vm.position(x, y)
            time.sleep(.5)
            pressed_at = time.monotonic()
            vm.hmp('mouse_button 1')
            time.sleep(.15)
            vm.hmp('mouse_button 0')
            time.sleep(.55)
            return pressed_at
        for axis, wanted in ((0, x), (1, y)):
            for _ in range(35):
                px, py = vm.pointer()
                current = (px, py)[axis]
                if abs(current - wanted) <= 3:
                    break
                delta = max(-60, min(60, round((wanted - current) / 2)))
                vm.hmp(f'mouse_move {delta if axis == 0 else 0} {delta if axis == 1 else 0} 0')
                deadline = time.monotonic() + 4
                while time.monotonic() < deadline:
                    moved = vm.pointer()[axis]
                    if moved != current:
                        break
                    time.sleep(.2)
                else:
                    raise AssertionError(f'pointer did not consume {delta}: {vm.pointer()}')
            else:
                raise AssertionError(f'pointer did not reach {(x, y)}: {vm.pointer()}')
        pressed_at = time.monotonic()
        vm.hmp('mouse_button 1')
        time.sleep(.15)
        vm.hmp('mouse_button 0')
        time.sleep(.55)
        return pressed_at

    def focus():
        dump = a.output / 'focus.bin'
        vm.hmp(f'pmemsave 0 0x200000 "{dump}"')
        data = dump.read_bytes()
        dump.unlink()
        marker = data.find(b'CVMMSTAT')
        assert marker >= 0, 'VM manager status absent'
        return struct.unpack_from('<I', data, marker + 0x28)[0]

    def wait_focus(number, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if focus() == number:
                return
            time.sleep(.2)
        raise AssertionError(f'VM focus did not become {number}')

    def run(command):
        before = vm.offset()
        keys('meta_l-r')
        vm.wait('WINDOW 01 OPEN', before, 20)
        type_text(command)
        keys('ret')
        vm.wait('[DOSVM] fork', before, 60)
        return before

    try:
        vm.ready()
        before = vm.offset()
        keys('meta_l-e')
        vm.wait('WINDOW 08 OPEN', before, 30)
        report['checks']['files_open'] = True

        audio_path = a.output / 'audio.wav'
        before = run(a.game + (' ' + a.game_args if a.game_args else ''))
        vm.wait('ST_Init: Init status bar.', before, 120)
        time.sleep(10)
        frame = np.asarray(Image.open(vm.shot('doom-with-files'))).astype(np.int16)
        area = frame[290:710, 395:1060, :3]
        red = int(((area[:, :, 0] > 110) &
                   (area[:, :, 0] > area[:, :, 1] * 1.35) &
                   (area[:, :, 0] > area[:, :, 2] * 1.15)).sum())
        assert red > 2000, f'DOOM game graphics absent (red pixels: {red})'
        report['checks']['doom_with_files'] = {'program': a.game, 'red_pixels': red, 'ready': True}
        if a.verify_game_audio:
            audio_before = audio_path.stat().st_size if audio_path.exists() else 44
            for _ in range(5):
                keys('ctrl')          # fire: effects must play with -nomusic
            time.sleep(2)
            pcm = audio_path.read_bytes()[max(44, audio_before):]
            energy = sum(abs(int.from_bytes(pcm[i:i+2], 'little', signed=True))
                         for i in range(0, len(pcm)-1, 128))
            assert energy > 10000, f'gameplay PCM is silent ({energy})'
            report['checks']['game_audio'] = {'sampled_energy': energy,
                                               'sampled_bytes': len(pcm)}
        keys('ctrl-esc')              # release keyboard/mouse from the DOS VM
        wait_focus(0)
        time.sleep(1)
        before = vm.offset()
        click(1050, 272)             # close the running DOS window by its box
        vm.wait('WINDOW 11 CLOSE', before, 20)
        vm.wait('[DOSVM] closed', before, 20)
        assert 'WINDOW 08 CLOSE' not in log()[before:], 'closing DOOM closed Files'
        report['checks']['bounded_close'] = True
        time.sleep(3)                  # allow the killed VM's session to drain

        first = run('\\COMMAND.COM')
        vm.wait('[DOSVM] video ready', first, 45)
        time.sleep(4)
        vm.shot('first-text-vm')
        keys('ctrl-esc')                # return keyboard to the desktop
        wait_focus(0)
        time.sleep(2)
        second = vm.offset()
        pressed_at = click(52, 14)    # CiukiOS logo opens its menu by real PS/2 input
        vm.wait('WINDOW 07 OPEN', second, 10)
        menu_latency = time.monotonic() - pressed_at
        assert menu_latency < 5, f'desktop system menu response took {menu_latency:.2f}s'
        pressed_at = click(580, 438)  # Run in that menu; top bar now shows system indicators
        vm.wait('WINDOW 01 OPEN', second, 10)
        latency = time.monotonic() - pressed_at
        assert latency < 5, f'desktop Run response took {latency:.2f}s'
        report['checks']['desktop_mouse_after_release'] = {
            'menu_open_seconds': round(menu_latency, 2),
            'run_open_seconds': round(latency, 2),
        }
        type_text('\\COMMAND.COM')
        keys('ret')
        # Serial writes from the system VM and the forked VM can interleave
        # inside the common "[DOSVM] fork" prefix.  The executable path is
        # emitted after that prefix and identifies this second launch.
        vm.wait('fork \\COMMAND.COM', second, 60)
        vm.wait('[DOSVM] video ready', second, 45)
        opened = [int(value) for value in re.findall(r'WINDOW (\d\d) OPEN', log()[second:])
                  if int(value) >= 18]
        assert len(opened) == 1, f'expected one new dynamic DOS window: {opened}'
        second_window = opened[0]
        time.sleep(2)
        vm.shot('two-text-vms')
        report['checks']['two_vms'] = True

        click(1040, 400)               # exposed right edge of the older DOS VM
        wait_focus(1)
        click(900, 216)                # title of the newer DOS VM
        wait_focus(2)
        report['checks']['focus_by_click'] = True

        before = vm.offset()
        type_text('echo M4 SECOND VM')
        keys('ret')
        vm.wait('M4 SECOND VM', before, 20)
        report['checks']['keyboard_to_focused_vm'] = True
        before = vm.offset()
        click(994, 216)                # the new DOS window's close box
        vm.wait(f'WINDOW {second_window:02d} CLOSE', before, 20)
        vm.wait('[DOSVM] closed', before, 20)
        vm.shot('first-vm-survives')
        assert 'WINDOW 11 CLOSE' not in log()[before:], 'closing VM2 closed VM1'
        report['checks']['independent_close'] = True
        report['passed'] = True
    except Exception as exc:
        report['error'] = repr(exc)
        try:
            vm.shot('failure')
        except Exception:
            pass
        raise
    finally:
        (a.output / 'serial.log').write_text(log())
        vm.close()
        (a.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('[m4] PASS', a.output)


if __name__ == '__main__':
    main()
