#!/usr/bin/env python3
"""Bounded VirtIO GL display-settings and DOS-session guard check.

Run alone inside a capped user scope. This harness boots the full image once
with a snapshot overlay; it never changes the source disk.
"""
import argparse
import json
import os
from pathlib import Path
import re
import signal
import struct
import subprocess
import tempfile
import time

import numpy as np
from PIL import Image

from qemu_test_native_windows import WindowVM
from qemu_test_native_desktop import match_pointer


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--guard-only', action='store_true', help='check DOS guard without repeating mode previews')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    vm = None
    gpu_address = None
    report = {'passed': False, 'device': 'virtio-gl', 'memory_mib': 256,
              'snapshot': True, 'guest_disk_written': False, 'screenshots': [],
              'guard_only': args.guard_only, 'physical_input_tested': False}

    def expired(*_):
        raise TimeoutError('180-second display-settings validation limit')

    signal.signal(signal.SIGALRM, expired)
    signal.signal(signal.SIGTERM, expired)
    signal.alarm(180)

    def wait_paint(offset, timeout=30):
        vm.wait('[DESKTOP] PAINT', offset, timeout)

    def words(address, count=1):
        raw = vm.hmp(f'xp /{count}wx {address:#x}').decode(errors='replace')
        result = []
        for line in raw.splitlines():
            match = re.search(r'[0-9a-fA-F]+:\s+(0x[0-9a-fA-F]+.*)', line)
            if match:
                result.extend(int(value, 16) for value in re.findall(r'0x([0-9a-fA-F]+)', match[1]))
        assert len(result) == count, raw
        return result

    def gpu_state():
        values = words(gpu_address + 8, 11)
        return dict(zip(('size', 'version', 'enabled', 'width', 'height', 'pitch',
                         'device_status', 'submits', 'completions', 'skipped', 'error_stage'), values))

    def wait_gpu(width, height, timeout=15):
        deadline = time.monotonic() + timeout
        state = gpu_state()
        while time.monotonic() < deadline:
            if state['enabled'] == 1 and state['error_stage'] == 0 and (state['width'], state['height']) == (width, height):
                return state
            time.sleep(.25)
            state = gpu_state()
        raise AssertionError(f'GPU did not reach {width}x{height}: {state}')

    def shot(name):
        # KWin's capture sees QEMU's direct GL buffer; screendump is blank here.
        windows = subprocess.check_output(['xdotool', 'search', '--onlyvisible',
                                           '--pid', str(vm.process.pid)],
                                          stderr=subprocess.DEVNULL, timeout=5).split()
        assert len(windows) == 1, windows
        path = output / (name + '.png')
        subprocess.run(['xdotool', 'windowactivate', '--sync', windows[0].decode()],
                       check=True, stderr=subprocess.DEVNULL, timeout=5)
        time.sleep(.15)
        subprocess.run(['spectacle', '--activewindow', '--background', '--nonotify',
                        '--no-decoration', '--no-shadow', '--output', str(path)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
        report['screenshots'].append(path.name)
        return path

    def open_run(command):
        vm.key('f3')
        vm.text(command)

    try:
        os.environ['SDL_VIDEODRIVER'] = 'x11'
        os.environ['SDL_MOUSE_RELATIVE_MODE_WARP'] = '0'
        extra = ['-snapshot', '-device', 'virtio-vga-gl,xres=1024,yres=768',
                 '-audiodev', 'none,id=snd', '-device', 'AC97,audiodev=snd',
                 '-display', 'sdl,gl=on']
        vm = WindowVM(args.image.resolve(), output, 'none', memory=256,
                      palette='platinum', extra_qemu_args=extra)
        vm._pointer = lambda: match_pointer(
            np.asarray(Image.open(shot('pointer')).convert('RGB')), *vm.cursor_colors)
        report['qemu'] = vm.process.args
        vm.ready()

        # Find the single resident CVGPU001 ABI within one bounded 4-MiB read.
        with tempfile.TemporaryDirectory(prefix='ciuki-display-', dir='/dev/shm') as temp:
            dump = Path(temp) / 'resident.bin'
            vm.hmp('stop')
            try:
                vm.hmp(f'pmemsave 0x100000 0x400000 "{dump}"')
                data = dump.read_bytes()
                candidates = [m.start() for m in re.finditer(b'CVGPU001', data)
                              if struct.unpack_from('<II', data, m.start() + 8) == (52, 1)]
                assert len(candidates) == 1, candidates
                gpu_address = 0x100000 + candidates[0]
            finally:
                vm.hmp('cont')
        report['gpu_physical'] = hex(gpu_address)
        report['gpu_initial'] = wait_gpu(800, 600)

        start = vm.offset()
        open_run('CONTROL')
        vm.wait('[DESKTOP] PAINT', start, 30)
        start = vm.offset()
        vm.key('home')
        vm.key('ret')
        vm.wait('[CONTROL] open Display', start, 30)
        for marker in ('[DISPLAY] adapter', '[DISPLAY] driver', '[DISPLAY] monitor'):
            vm.wait(marker, start, 15)
        report['display_probe_logged'] = True
        wait_paint(start, 30)
        for number, name in enumerate(() if args.guard_only else ('screen', 'adapter', 'monitor', 'advanced'), 1):
            start = vm.offset()
            vm.key(str(number))
            wait_paint(start, 20)
            shot('display-' + name)

        # The first five firmware modes are 640x480 at 8/15/16/24/32 bpp.
        # Preview the fifth, let its 12-second timer restore the original mode,
        # and then repeat and explicitly keep it.
        for _ in range(0 if args.guard_only else 2):
            start = vm.offset()
            vm.key('1')
            vm.key('home')
            for _ in range(4):
                vm.key('down')
            vm.key('ret')
            vm.wait('[DISPLAY] preview', start, 20)
            vm.wait('[VGASETUP] DESKTOP PREVIEW', start, 20)
            report.setdefault('mode_previews', []).append(
                subprocess.check_output(['scripts/serial_log_normalize.py', '--offset', str(start),
                                         str(vm.serial)]).decode(errors='replace')[-1800:])
            if len(report['mode_previews']) == 1:
                vm.wait('[VGASETUP] Preview cancelled; settings preserved.', start, 22)
                wait_paint(start, 25)
                report['after_timeout_restore'] = wait_gpu(800, 600)
                shot('display-timeout-restored')
            else:
                time.sleep(1.5)
                vm.key('ret')
                vm.wait('[VGASETUP] Desktop resolution saved; Windows profile preserved.', start, 20)
                wait_paint(start, 30)
                report['after_confirmed_mode'] = wait_gpu(640, 480)
                vm.wait('[DISPLAY] monitor QEMU Monitor', start, 10)
                time.sleep(.2)
                shot('display-640x480-32')

        # Close Display Properties before starting the game session.
        start = vm.offset()
        vm.key('esc')
        wait_paint(start, 20)
        start = vm.offset()
        open_run(r'C:\DESKTOP\TestGames\DOOM.COM')
        vm.wait('ST_Init: Init status bar.', start, 60)
        for key in ('esc', 'ret', 'ret', 'ret'):
            vm.hmp(f'sendkey {key} 100')
            time.sleep(.4)
        time.sleep(2)
        shot('doom-new-game')

        start = vm.offset()
        # The physical mouse belongs to the desktop. Raise Control via its
        # task button before opening properties, while Doom keeps running.
        # The keyboard belongs to Doom until a desktop window is focused.
        vm.click_at(189, gpu_state()['height'] - 18)
        open_run('DISPLAY')
        vm.wait('[DISPLAY] adapter', start, 30)
        vm.wait('[DISPLAY] driver', start, 15)
        vm.wait('[DISPLAY] monitor', start, 15)
        vm.key('ret')
        vm.wait('[DISPLAY] blocked', start, 20)
        wait_paint(start, 20)
        shot('display-blocked-by-dos')
        report['dos_session_guard'] = 'DISPLAY refused mode change while Doom VM was open'
        vm.key('ret')  # dismiss the explanatory message
        time.sleep(.4)
        vm.key('esc')  # close Display Properties
        time.sleep(.5)
        vm.click_at(268, gpu_state()['height'] - 18)  # raise the still-running DOS window
        vm.hmp('sendkey f10 300')
        time.sleep(.4)
        vm.hmp('sendkey y 300')
        vm.wait('[DOSVM] ended', start, 30)
        time.sleep(.5)
        report['gpu_after_doom_exit'] = gpu_state()
        assert report['gpu_after_doom_exit']['enabled'] == 1, report['gpu_after_doom_exit']
        assert report['gpu_after_doom_exit']['error_stage'] == 0, report['gpu_after_doom_exit']
        report['passed'] = True
    except Exception as exc:
        report['error'] = repr(exc)
        if vm:
            try:
                shot('failure')
            except Exception:
                pass
        raise
    finally:
        signal.alarm(0)
        if vm:
            try:
                vm.close()
            except Exception as exc:
                report.setdefault('cleanup_error', repr(exc))
            finally:
                if vm.process.poll() is None:
                    vm.process.kill()
                    vm.process.wait(timeout=5)
        (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
