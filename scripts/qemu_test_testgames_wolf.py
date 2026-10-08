#!/usr/bin/env python3
"""Qualify the original Wolf3D TestGames shortcut through real Files input.

Run alone in a capped scope. Uses a private unchanged-payload HDD copy,
512MiB/std VGA, real mouse/keys and read-only RAM. No game or fixture patches.
Screens are matched to the installed WL6 assets, including the gameplay HUD.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

import numpy as np
from PIL import Image

from inspect_dosvm_snapshot import parse_snapshot
from qemu_files_game_launch import launch_testgames_from_files
from qemu_test_installed_hdd import FAT16
from qemu_test_native_utilities import Utilities
from qemu_test_native_windows import WindowVM
from wolf_intro import wait_for_menu
from wolf_screen import (WolfPalette, WolfPictures, WolfQuitMessages, WolfSignon,
                         complete_picture_score, cursor_score, episode_score, gameplay_score,
                         pattern_score, quit_confirmation_score, signon_score)

ROOT = Path(__file__).resolve().parents[1]
GAME = r'C:\DESKTOP\TestGames\WOLF3D.COM'


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=ROOT / 'build/full/ciukios-full.img')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--game-timeout', type=int, default=90)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    image = args.image.resolve()
    source_hash = digest(image)
    disk = out / 'disk.img'
    shutil.copyfile(image, disk)
    vm = ui = None
    report = dict(passed=False, game=GAME, game_args='', launch_method='Files double-click',
                  memory_mib=512, vga='std', physical_hardware_qualified=False,
                  guest_memory_writes=False, fixture_payloads=False, game_patches=False,
                  source_image_sha256=source_hash, events=[], snapshots=[])
    previous_snapshots = {}

    def event(name, **data):
        report['events'].append(dict(name=name, **data))
        print('[testgames-wolf] ' + name, flush=True)

    def capture(name):
        path = vm.shot(name)
        with Image.open(path) as image_frame:
            frame = image_frame.convert('RGB')
            frame.save(out / (name + '.png'))
            if ui is None:
                return frame
            ui.refresh()
            x, y, w, h = (ui.w(label, 11) for label in
                           ('ui_window_x', 'ui_window_y', 'ui_window_width', 'ui_window_height'))
            bounds = (x + 3, y + 30, x + w - 3, y + h - 3)
            assert 0 <= bounds[0] < bounds[2] <= frame.width
            assert 0 <= bounds[1] < bounds[3] <= frame.height
            client = frame.crop(bounds).resize((320, 200), Image.Resampling.NEAREST)
        client.save(out / (name + '-client.png'))
        report['client_rectangle'] = list(bounds)
        return np.array(client)

    def key(value, milliseconds=150):
        vm.hmp(f'sendkey {value} {milliseconds}')
        time.sleep(milliseconds / 1000 + .25)

    def wait_pixels(name, predicate, timeout=None):
        deadline = time.monotonic() + (timeout or args.game_timeout)
        last = None
        while time.monotonic() < deadline:
            frame = capture(name)
            last = predicate(frame)
            if last.get('matched'):
                event(name, screenshot=name + '-client.png', evidence=last)
                return frame
            assert vm.process.poll() is None, 'QEMU exited during screen readiness'
            time.sleep(.25)
        raise AssertionError(f'{name}: expected screen never became ready: {last}')

    def main_menu(frame):
        options = complete_picture_score(frame, pictures.picture(10), 80, 0, palette)
        rows = [row for row in range(10) if cursor_score(frame, pictures, (76, 55), row)['matched']]
        return dict(matched=options['matched'] and len(rows) == 1, options=options, selected_rows=rows)

    def serial():
        return subprocess.check_output(['scripts/serial_log_normalize.py', str(vm.serial)]).decode('cp437', 'replace')

    try:
        fat = FAT16(disk)
        fresh = out / 'fresh-SHELL.COM'
        listing = out / 'SHELL.lst'
        subprocess.run(['nasm', '-f', 'bin', '-l', str(listing), '-o', str(fresh),
                        'src/com/shell.asm'], cwd=ROOT, check=True)
        shell = fat.read('SYSTEM/SHELL.COM')
        assert fresh.read_bytes() == shell, 'Fresh SHELL differs from image; refusing stale RAM offsets'
        report['shell_sha256'] = digest(fresh)
        report['listing_sha256'] = digest(listing)
        launcher = subprocess.check_output(['mtype', '-i', f'{disk}@@{fat.start}',
                                            '::DESKTOP/TestGames/WOLF3D.COM'])
        assert b'\\APPS\\WOLF3D\\WOLF3D.EXE\0' in launcher.upper(), 'TestGames does not EXEC original Wolf'
        report['launcher_sha256'] = hashlib.sha256(launcher).hexdigest()
        engine = fat.read('APPS/WOLF3D/WOLF3D.EXE')
        report['engine_sha256'] = hashlib.sha256(engine).hexdigest()
        signon = WolfSignon(engine)
        report['signon_asset'] = signon.evidence
        palette = WolfPalette(engine)
        report['palette_asset'] = palette.evidence
        quit_messages = WolfQuitMessages(engine)
        report['quit_message_asset'] = quit_messages.evidence
        assets = {name: fat.read('APPS/WOLF3D/' + name + '.WL6')
                  for name in ('VGAHEAD', 'VGADICT', 'VGAGRAPH')}
        report['asset_sha256'] = {name: hashlib.sha256(data).hexdigest() for name, data in assets.items()}
        pictures = WolfPictures(assets['VGAHEAD'], assets['VGADICT'], assets['VGAGRAPH'])
        for slot in range(1, 4):
            try:
                previous_snapshots[slot] = hashlib.sha256(fat.read(f'SYSTEM/DOSVM{slot}.BIN')).hexdigest()
            except KeyError:
                pass
        vm = WindowVM(disk, out, 'std', boot_capture=True, memory=512, palette='platinum')
        report['qemu_command'] = vm.process.args
        vm.ready()
        vm.repaint_key('esc')
        ui = Utilities(vm, shell, listing)
        launch, evidence = launch_testgames_from_files(vm, ui, disk, 'WOLF3D.COM')
        event('No-argument Files shortcut verified', **evidence)
        vm.wait('[DOSVM] fork', launch, 30)
        ui.until(lambda: ui.b('ui_active_window') == 11 and ui.b('ui_window_flags', 11) == 1,
                 'Native Wolf DOS window did not become active', 30)
        vm.position(8, ui.w('ui_height') - 20)

        def intro(frame):
            candidates = [signon_score(frame, pictures, signon)]
            candidates.extend(dict(complete_picture_score(frame, pictures.picture(number), x, y, palette), picture=number)
                              for number, x, y in ((87, 0, 0), (88, 216, 110), (89, 0, 0)))
            result = max(candidates,
                         key=lambda item: (item['matched'], item['score']))
            return result

        def intro_acknowledged(frame, evidence):
            name = f'wolf-intro-ack-{evidence["number"]}'
            Image.fromarray(frame).save(out / (name + '-client.png'))
            event('Wolf acknowledged complete intro', screenshot=name + '-client.png', evidence=evidence)

        _, ready = wait_for_menu(lambda: capture('wolf-main-menu'), main_menu, intro,
                                 lambda: key('spc'), timeout=args.game_timeout,
                                 on_acknowledge=intro_acknowledged)
        event('Wolf main menu ready', evidence=ready)
        key('n')
        wait_pixels('wolf-new-game-selected',
                    lambda f: dict(cursor_score(f, pictures, (76, 55), 0), screen='main-menu-row0'), 15)
        key('ret')
        wait_pixels('wolf-episode-menu', lambda f: episode_score(f, pictures), 20)
        key('ret')
        wait_pixels('wolf-normal-difficulty',
                    lambda f: dict(pattern_score(f, pictures.picture(21), 232, 107),
                                   matched=pattern_score(f, pictures.picture(21), 232, 107)['matched']
                                   and cursor_score(f, pictures, (50, 100), 2)['matched']), 20)
        key('ret')
        settled = lambda f: gameplay_score(f, pictures, palette)
        first = wait_pixels('wolf-gameplay-hud', settled)
        assert not main_menu(first)['matched'], 'Menu pixels mistaken for gameplay'

        # Same-sized timed comparisons distinguish a commanded rotation from
        # passive animation. Ignore HUD, weapon and the outer view border.
        view = np.s_[12:128, 16:304, :]
        passive = []
        previous = first
        for number in range(3):
            time.sleep(1.25)
            frame = wait_pixels(f'wolf-passive-{number}', settled)
            passive.append(int(np.any(previous[view] != frame[view], axis=2).sum()))
            previous = frame
        vm.hmp('sendkey right 1000')
        time.sleep(1.25)
        moved = wait_pixels('wolf-after-right', settled)
        changed = int(np.any(previous[view] != moved[view], axis=2).sum())
        minimum = max(1000, max(passive) * 2 + 256)
        event('Wolf commanded rotation response', passive_changed_pixels=passive,
              input_changed_pixels=changed, required_input_change=minimum)
        assert changed > minimum, 'Wolf input response did not exceed passive frame changes'

        key('f10')
        wait_pixels('wolf-quit-confirmation',
                    lambda frame: quit_confirmation_score(frame, pictures, quit_messages))
        event('Wolf F10 confirmation displayed')
        key('y')
        vm.wait('[DOSVM] ended', launch, 60)
        ui.until(lambda: ui.b('ui_active_window') == 11 and ui.b('ui_window_flags', 11) == 1,
                 'Finished Wolf window disappeared', 15)
        capture('wolf-finished')
        event('Wolf exited through its own confirmation')
        key('alt-f4')
        vm.wait('[DESKTOP] WINDOW 11 CLOSE', launch, 20)
        ui.until(lambda: ui.b('ui_window_flags', 11) == 0 and ui.w('app_segs', 8) == 0,
                 'Finished Wolf module did not close/unload', 20)
        path = vm.shot('desktop-return')
        with Image.open(path) as screen:
            screen.save(out / 'desktop-return.png')
        report['passed'] = True
    except Exception as error:
        report['error'] = repr(error)
        if vm:
            try:
                path = vm.shot('failure')
                with Image.open(path) as screen:
                    screen.save(out / 'failure.png')
                report['registers'] = vm.hmp('info registers').decode('utf-8', 'replace')
                report['pic'] = vm.hmp('info pic').decode('utf-8', 'replace')
                vm.hmp('stop')
                try:
                    dump = out / 'failure-16m.bin'
                    vm.hmp(f'pmemsave 0 0x1000000 "{dump}"')
                    assert dump.stat().st_size == 16 * 1024 * 1024
                    report['failure_ram_bytes'] = dump.stat().st_size
                finally:
                    vm.hmp('cont')
            except Exception as capture_error:
                report['capture_error'] = repr(capture_error)
    finally:
        if vm:
            try:
                report['serial_tail'] = serial()[-16000:]
            except Exception as error:
                report['serial_capture_error'] = repr(error)
            vm.close()
        try:
            fs = FAT16(disk)
            for name in ('DOSVM.LOG', 'DISPLAY.LOG'):
                try:
                    (out / name).write_bytes(fs.read('SYSTEM/' + name))
                except KeyError:
                    pass
            for slot in range(1, 4):
                name = f'DOSVM{slot}.BIN'
                try:
                    raw = fs.read('SYSTEM/' + name)
                except KeyError:
                    continue
                (out / name).write_bytes(raw)
                snapshot = parse_snapshot(raw)  # Requires exactly448bytes/CDVS1.0.
                snapshot['file'] = name
                snapshot['fresh'] = hashlib.sha256(raw).hexdigest() != previous_snapshots.get(slot)
                report['snapshots'].append(snapshot)
            if report['passed']:
                exits = [s for s in report['snapshots'] if s['fresh'] and 'WOLF3D.COM' in s['title'].upper()
                         and s['reason'] in ('observed_exit', 'user_close') and s['exit_code'] == 0]
                assert exits, 'No fresh448B Wolf snapshot proves exit status0'
                log = fs.read('SYSTEM/DOSVM.LOG').decode('cp437', 'replace')
                assert GAME.upper() in log.upper(), 'DOSVM.LOG lacks the requested bare shortcut'
                report['exit_snapshot_files'] = [s['file'] for s in exits]
        except Exception as error:
            report['passed'] = False
            report.setdefault('error', repr(error))
        report['source_unchanged'] = digest(image) == source_hash
        report['passed'] &= report['source_unchanged']
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: report[key] for key in ('passed', 'source_unchanged', 'error') if key in report}))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
