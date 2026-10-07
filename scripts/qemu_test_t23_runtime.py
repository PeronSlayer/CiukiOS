#!/usr/bin/env python3
"""Apply a real Display preview, then exercise the installed DOOM.COM wrapper.

Use a capped user scope, alone. QEMU covers the BIOS bank path, not S3 silicon.
All guest writes go to a private copy. Verify Run text, game input/frame changes,
menu quit, desktop return and a second opening. Failure retains bounded RAM.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

import numpy as np
from PIL import Image
from doom_screen import menu_skulls, selected_row
from qemu_test_installed_hdd import FAT16
from qemu_test_native_utilities import Utilities
from qemu_test_native_windows import WindowVM

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def assemble_shell(output):
    shell = output / 'assembled-SHELL.COM'
    listing = output / 'SHELL.lst'
    subprocess.run([
        'nasm', '-f', 'bin', '-l', str(listing), '-o', str(shell),
        'src/com/shell.asm',
    ], cwd=ROOT, check=True)
    return shell, listing


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--vga', choices=('std', 'cirrus'), default='cirrus')
    p.add_argument('--memory', type=int, default=512)
    p.add_argument('--game', default=r'C:\DESKTOP\TestGames\DOOM.COM')
    p.add_argument('--game-args', default='-warp 1 1 -nomusic')
    p.add_argument('--skip-display', action='store_true')
    p.add_argument('--display-only', action='store_true')
    p.add_argument('--game-timeout', type=int, default=60)
    a = p.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    image = a.image.resolve()
    before = digest(image)
    disk = out / 'disk.img'
    shutil.copyfile(image, disk)
    shell_binary, listing = assemble_shell(out)
    fat = FAT16(disk)
    shipped_shell = fat.read('SYSTEM/SHELL.COM')
    assert shell_binary.read_bytes() == shipped_shell, (
        'freshly assembled SHELL.COM differs from the source image; refusing stale RAM offsets')
    vm = None
    report = dict(passed=False, vga=a.vga, memory_mib=a.memory,
                  game=a.game, game_args=a.game_args, physical_tested=False,
                  shell_sha256=hashlib.sha256(shipped_shell).hexdigest(),
                  shell_listing=listing.name,
                  shell_listing_sha256=hashlib.sha256(listing.read_bytes()).hexdigest(),
                  requested_commands=[], game_commands=[], events=[])
    ui = None

    def shot(name):
        path = vm.shot(name)
        with Image.open(path) as im:
            im.save(path.with_suffix('.png'))
            return im.size

    def type_run(command):
        assert len(command) <= 96, 'Run field command exceeds its capacity'
        start = vm.offset()
        vm.key('f3')
        ui.until(lambda: ui.b('ui_active_window') == 1, 'Run window did not open', 20)
        accepted = False
        for attempt in range(3):
            ui.refresh()
            length = ui.b('ui_run_len')
            vm.key('home')
            for _ in range(length):
                vm.key('delete')
                time.sleep(.08)
            for char in command:
                key = ('shift-' + char.lower()) if char.isupper() else {
                    ' ': 'spc', '.': 'dot', '\\': 'backslash', '-': 'minus',
                    '/': 'slash', ':': 'shift-semicolon'}.get(char, char)
                vm.key(key)
                time.sleep(.15)
            deadline = time.monotonic() + 5
            expected = command.encode('ascii')
            while time.monotonic() < deadline:
                ui.refresh()
                length = ui.b('ui_run_len')
                actual = ui.ram[ui.offset('ui_run_text'):ui.offset('ui_run_text') + length]
                if length == len(expected) and actual == expected:
                    accepted = True
                    break
                time.sleep(.1)
            if accepted:
                break
        assert accepted, f'Run field lost characters: requested={command!r}, actual={actual!r}'
        report['requested_commands'].append(command)
        report['events'].append(dict(name='Run command verified', command=command,
                                     attempts=attempt + 1))
        ui.click(40, 1)
        return start

    def window_geometry(owner):
        ui.refresh()
        return (ui.w('ui_window_x', owner), ui.w('ui_window_y', owner),
                ui.w('ui_window_width', owner), ui.w('ui_window_height', owner))

    def game_frame(name):
        path = vm.shot(name)
        with Image.open(path) as frame:
            x, y, width, height = window_geometry(11)
            # DOSVM paints precisely the client from TITLE_H to its 3px frame.
            client = frame.convert('RGB').crop((x + 3, y + 30,
                                                 x + width - 3, y + height - 3))
        assert client.width > 0 and client.height > 0, f'{name}: empty DOOM client {client.size}'
        client = client.resize((640, 400), Image.Resampling.NEAREST)
        client.save(out / (name + '-client.png'))
        return client

    def wait_menu_row(expected, name, timeout=8):
        deadline = time.monotonic() + timeout
        current = None
        while time.monotonic() < deadline:
            current = selected_row(game_frame(name), sprites, (97, 64), 6)
            if current == expected:
                report['events'].append(dict(name='DOOM menu row observed',
                                             screenshot=name, row=expected))
                return
            time.sleep(.25)
        raise AssertionError(f'{name}: DOOM menu row {current}, expected {expected}')

    def game_key(key, hold_ms=150):
        vm.hmp(f'sendkey {key} {hold_ms}')
        time.sleep(hold_ms / 1000 + .25)

    def start_game(label):
        command = a.game + (' ' + a.game_args if a.game_args else '')
        report['game_commands'].append(command)
        start = type_run(command)
        vm.wait('[DOSVM] open', start, 30)
        vm.wait('[DOSVM] fork', start, 30)
        ui.until(lambda: ui.b('ui_window_flags', 11) == 1
                 and ui.b('ui_active_window') == 11 and ui.w('app_segs', 8) != 0,
                 'DOOM native DOS window did not become active', 30)
        vm.wait('[DOOM] Using the active DOS session audio devices', start, 30)
        vm.wait('ST_Init: Init status bar.', start, a.game_timeout)
        time.sleep(2)
        first = game_frame(label + '-running')
        assert selected_row(first, sprites, (97, 64), 6) is None, \
            'DOOM remained in its title/menu screen instead of starting the requested level'
        vm.hmp('sendkey right 900')
        time.sleep(.6)
        moved = game_frame(label + '-after-input')
        a_pixels = np.asarray(first)[:300]
        b_pixels = np.asarray(moved)[:300]
        changed = int(np.any(a_pixels != b_pixels, axis=2).sum())
        report['events'].append(dict(name='DOOM gameplay input changed frame',
                                     screenshot=label, changed_pixels=changed))
        assert changed > 640 * 300 * .01, \
            f'DOOM gameplay did not respond to input: changed {changed} client pixels'
        return start

    def quit_game(start, label):
        game_key('esc')
        wait_menu_row(0, label + '-menu-new-game')
        for row in range(1, 6):
            game_key('down')
            wait_menu_row(row, label + f'-menu-row-{row}')
        game_key('ret')
        time.sleep(.5)
        confirmation = game_frame(label + '-quit-confirmation')
        assert selected_row(confirmation, sprites, (97, 64), 6) is None, \
            'DOOM did not open its quit confirmation after selecting Quit Game'
        game_key('y')
        vm.wait('[DOOM] DOS session game returned', start, 60)
        vm.wait('[DOSVM] ended', start, 60)
        ui.until(lambda: ui.b('ui_window_flags', 11) == 1
                 and ui.w('app_segs', 8) != 0 and ui.b('ui_active_window') == 11,
                 'finished DOOM window disappeared before the user closed it', 15)
        shot(label + '-finished-window')
        vm.key('alt-f4')
        vm.wait('[DESKTOP] WINDOW 11 CLOSE', start, 20)
        ui.until(lambda: ui.b('ui_window_flags', 11) == 0
                 and ui.w('app_segs', 8) == 0 and ui.b('ui_active_window') != 11,
                 'Alt+F4 did not close/unload the finished DOSVM window', 20)
        report['events'].append(dict(name='DOOM exited, then DOS window closed with Alt+F4',
                                     screenshot=label, app_slot=8, window=11))
        shot(label + '-desktop-return')

    def clean_hmp(command):
        raw = vm.hmp(command).decode(errors='replace')
        return '\n'.join(line for line in raw.splitlines()
                         if '\x1b' not in line and line.strip() != '(qemu)')

    try:
        vm = WindowVM(disk, out, a.vga, boot_capture=True, memory=a.memory,
                      palette='platinum')
        report['qemu'] = vm.process.args
        vm.ready()
        shell = shipped_shell
        sprites = menu_skulls(fat.read('APPS/DOOM/DOOM.WAD'))
        ui = Utilities(vm, shell, listing)
        report['initial_size'] = shot('initial')
        # Dismiss the initial About window using its public keyboard action.
        vm.key('esc')
        if not a.skip_display:
            start = type_run('DISPLAY')
            vm.wait('[DISPLAY] adapter', start, 30)
            vm.wait('[DESKTOP] PAINT', start, 20)
            vm.key('1')
            vm.key('home')
            vm.key('ret')
            vm.wait('[DISPLAY] preview', start, 20)
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                size = shot('preview')
                if size == (640, 480):
                    break
                time.sleep(.2)
            assert size == (640, 480), f'preview remained {size}'
            report['preview_size'] = size
            # Mode switching is deferred; the first 640 frame can precede
            # Display's poll and its confirmation dialog.
            time.sleep(1.5)
            vm.key('ret')
            vm.wait('[DISPLAY] kept', start, 20)
            report['confirmed_size'] = shot('confirmed')
            assert report['confirmed_size'] == (640, 480)
            vm.key('esc')
        if not a.display_only:
            assert a.game.upper().replace('/', '\\') == r'C:\DESKTOP\TESTGAMES\DOOM.COM', \
                'This gate exercises the shipped TestGames DOOM.COM launcher'
            wrapper = fat.read('APPS/DOOM/DOOM.COM')
            core = fat.read('APPS/DOOM/DOOMCORE.EXE')
            # The independent FAT reader resolves DOS names; TestGames is a
            # VFAT name. mtools resolves its stored alias without assuming it.
            launcher = subprocess.check_output([
                'mtype', '-i', f'{disk}@@{fat.start}',
                '::DESKTOP/TestGames/DOOM.COM'])
            assert wrapper != core, 'APPS/DOOM/DOOM.COM unexpectedly aliases the DOOM core'
            assert b'\\APPS\\DOOM\\DOOM.COM\0' in launcher.upper(), \
                'TestGames DOOM.COM does not EXEC the installed wrapper'
            report['doom_wrapper_sha256'] = hashlib.sha256(wrapper).hexdigest()
            report['testgames_launcher_sha256'] = hashlib.sha256(launcher).hexdigest()
            start = start_game('doom-first')
            report['game_size'] = shot('doom-running')
            quit_game(start, 'doom-first')
            start = start_game('doom-second')
            quit_game(start, 'doom-second')
            report['doom_com_launches'] = 2
        report['passed'] = True
    except Exception as error:
        report['error'] = repr(error)
        if vm:
            try:
                shot('failure')
                report['registers'] = clean_hmp('info registers')
                report['pic'] = clean_hmp('info pic')
                report['pit'] = clean_hmp('info qtree')
                vm.hmp('stop')
                vm.hmp(f'pmemsave 0 0x800000 "{out / "failure-8m.bin"}"')
                vm.hmp('cont')
            except Exception as capture_error:
                report['capture_error'] = repr(capture_error)
    finally:
        if vm:
            vm.close()
        fs = FAT16(disk)
        for name in ('DISPLAY.LOG', 'DOSVM.LOG'):
            try:
                (out / name).write_bytes(fs.read('SYSTEM/' + name))
            except KeyError:
                pass
        if not a.display_only:
            try:
                dosvm_log = fs.read('SYSTEM/DOSVM.LOG').decode('cp437', 'replace')
                normalized = dosvm_log.upper()
                expected_log_text = a.game_args.strip().upper() or 'DOOM.COM'
                logged = normalized.count(expected_log_text)
                report['requested_command_log'] = dosvm_log
                report['logged_game_commands'] = logged
                if logged < len(report['game_commands']):
                    report['passed'] = False
                    report.setdefault('error', 'DOSVM.LOG did not record every requested DOOM command')
            except (KeyError, OSError) as log_error:
                report['passed'] = False
                report.setdefault('error', f'DOSVM.LOG unavailable for command verification: {log_error!r}')
        disk.unlink(missing_ok=True)
        report['source_unchanged'] = digest(image) == before
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ('passed', 'source_unchanged', 'error')
                      if k in report}), flush=True)
    return 0 if report['passed'] and report['source_unchanged'] else 1


if __name__ == '__main__':
    sys.exit(main())
