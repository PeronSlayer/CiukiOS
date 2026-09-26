#!/usr/bin/env python3
"""Real QEMU regressions for DOS window resume, narrow focus and empty Run.

The queued Backspace/Enter case writes only the BIOS keyboard ring while the
guest is paused through QEMU's GDB server. All resulting dispatch and painting
execute in the guest; no shell state or screen pixels are patched.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import time

import numpy as np
from PIL import Image
from qemu_test_installed_hdd import FAT16, listing_address
from qemu_test_native_windows import WindowVM, pixels

ROOT = Path(__file__).resolve().parents[1]


def saved(vm, name):
    frame = pixels(vm, name)
    Image.fromarray(frame).save(vm.output/f'{name}.png')
    return frame


def state(vm, name, shell, lines):
    path = vm.output/f'{name}-ram.bin'
    vm.hmp(f'pmemsave 0 1048576 "{path}"')
    ram = path.read_bytes()
    needle = shell[:64]
    base = ram.find(needle)
    assert base >= 0 and ram.count(needle) == 1
    def value(label, length=2):
        address = base + listing_address(lines, label)
        return int.from_bytes(ram[address:address+length], 'little')
    return {'active': value('ui_active_window', 1), 'dialog': value('ui_dialog', 1),
            'focus': value('ui_focus'), 'programs_visible': value('ui_window_visible', 1),
            'width': value('ui_ww'), 'height': value('ui_wh'),
            'run_length': value('ui_run_len', 1), 'run_cursor': value('ui_run_cursor', 1)}


def queue_bios(vm, keys):
    vm.hmp('stop')
    try:
        with socket.socket() as reserved:
            reserved.bind(('127.0.0.1', 0))
            port = reserved.getsockname()[1]
        response = vm.hmp(f'gdbserver tcp:127.0.0.1:{port}')
        assert b'Error' not in response, response
        with socket.create_connection(('127.0.0.1', port), timeout=3) as connection:
            def packet(text):
                data = text.encode()
                connection.sendall(b'$'+data+b'#'+f'{sum(data)%256:02x}'.encode())
                received = b''
                while b'#' not in received or len(received.split(b'#')[-1]) < 2:
                    received += connection.recv(4096)
                connection.sendall(b'+')
                assert b'$OK#' in received, received
            payload = b''.join(struct.pack('<H', value) for value in keys)
            packet(f'M41e,{len(payload):x}:{payload.hex()}')
            pointer = struct.pack('<HH', 0x1E, 0x1E+len(payload))
            packet(f'M41a,4:{pointer.hex()}')
    finally:
        vm.hmp('cont')


def resume_case(vm, shell, lines, baseline):
    x, y, width = vm.active_rect()
    assert width >= 680
    vm.click_at(x+width-64, y+15)  # Programs minimize
    vm.repaint_key('f1')
    about = vm.active_rect()
    assert about[2] == 466, about
    vm.repaint_key('tab')  # retain a non-default focus across DOS
    vm.position(1200, 730)
    time.sleep(.4)
    before = saved(vm, 'about-before-dos')
    before_state = state(vm, 'about-before-dos', shell, lines) if lines else None
    offset = vm.offset()
    vm.key('f4')
    vm.wait('[DESKTOP] DOS', offset, 20)
    vm.wait('CiukiOS SHELL C:\\APPS>', offset, 20)
    saved(vm, 'dos-from-about')
    offset = vm.offset()
    vm.text('exit')
    vm.ready(offset)
    vm.position(1200, 730)
    time.sleep(.4)
    after = saved(vm, 'about-after-dos')
    try:
        observed = vm.active_rect()
    except AssertionError:
        if not baseline:
            raise
        observed = None
    result = {'before_rect': about, 'after_rect': observed,
              'before_state': before_state}
    if baseline:
        assert observed != about, 'baseline did not reproduce wrong active window'
        x, y, width = about
        title_changes = int(np.any(before[y+3:y+25, x+3:x+175] !=
                                   after[y+3:y+25, x+3:x+175], axis=2).sum())
        assert title_changes > 1000, 'baseline did not visibly deactivate About'
        result['about_title_changed_pixels'] = title_changes
        result['baseline_bug_reproduced'] = True
        return result
    after_state = state(vm, 'about-after-dos', shell, lines)
    result['after_state'] = after_state
    assert observed == about, (about, observed)
    assert before_state['active'] == after_state['active'] == 2
    assert before_state['dialog'] == after_state['dialog'] == 2
    assert before_state['focus'] == after_state['focus']
    assert before_state['programs_visible'] == after_state['programs_visible'] == 0
    x, y, width = about
    changed = int(np.any(before[y:y+220, x:x+width] != after[y:y+220, x:x+width], axis=2).sum())
    result['about_content_changed_pixels'] = changed
    assert changed == 0, 'About pixels changed across DOS return'
    vm.repaint_key('esc')
    vm.repaint_key('f2')
    return result


def resize_case(vm, shell, lines):
    for _ in range(20):
        current = state(vm, 'focus-navigation', shell, lines)
        if current['focus'] == 2:
            break
        vm.repaint_key('tab')
    else:
        raise AssertionError('could not focus the visible Files sidebar button')
    x, y, width = vm.active_rect()
    assert width >= 680 and current['active'] == 0, (width, current)
    saved(vm, 'sidebar-focus-before-resize')
    vm.position(x+width-6, y+current['height']-6)
    vm.hmp('mouse_button 1')
    time.sleep(.2)
    offset = vm.offset()
    vm.move(-55, 0)
    vm.wait('[DESKTOP] PAINT', offset, 15)
    vm.hmp('mouse_button 0')
    time.sleep(.6)
    vm.position(1200, 730)
    after = state(vm, 'narrow-programs', shell, lines)
    rendered = vm.active_rect()
    saved(vm, 'sidebar-focus-after-resize')
    assert 510 <= rendered[2] < 680 and after['width'] == rendered[2], (rendered, after)
    assert after['focus'] == 44, after
    vm.repaint_key('tab')
    following = state(vm, 'narrow-next-tab', shell, lines)
    assert following['focus'] not in (2, 3, 8, 9), following
    saved(vm, 'sidebar-next-tab')
    return {'before': current, 'after': after, 'following_tab': following,
            'rendered_width': rendered[2]}


def empty_case(vm, shell, lines):
    vm.repaint_key('f3')
    x, y, width = vm.active_rect()
    assert width == 466
    vm.position(1200, 730)
    time.sleep(.4)
    empty = saved(vm, 'run-empty-reference')
    vm.repaint_key('a')
    time.sleep(.4)
    filled = saved(vm, 'run-character')
    region = (slice(y+76, y+104), slice(x+22, x+442))
    assert np.any(empty[region] != filled[region])
    offset = vm.offset()
    queue_bios(vm, [0x0E08, 0x1C0D])
    vm.wait('[DESKTOP] PAINT', offset, 10)
    time.sleep(.5)
    after = saved(vm, 'run-after-queued-delete-enter')
    final = state(vm, 'run-after-queued-delete-enter', shell, lines)
    assert final['run_length'] == final['run_cursor'] == 0 and final['dialog'] == 1, final
    changed = int(np.any(empty[region] != after[region], axis=2).sum())
    assert changed == 0, f'empty Run field retained {changed} stale pixels'
    return {'state': final, 'field_difference_from_initial_empty': changed,
            'input_method': 'Paused guest; GDB wrote BIOS ring keys 0E08/1C0D and head/tail; resumed guest'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disk', type=Path, default=ROOT/'build/full/ui-smooth-2026-09-25/ciukios.img')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--baseline', action='store_true')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    disk = out/'target.img'
    shutil.copyfile(args.disk, disk)
    lines = None
    if not args.baseline:
        binary, listing = out/'shell.com', out/'shell.lst'
        subprocess.run(['nasm', '-f', 'bin', 'src/com/shell.asm', '-o', str(binary),
                        '-l', str(listing)], cwd=ROOT, check=True)
        subprocess.run(['mcopy', '-o', '-i', f'{disk}@@{FAT16(disk).start}',
                        str(binary), '::SYSTEM/SHELL.COM'], check=True)
        lines = listing.read_text().splitlines()
    profile = out/'DISPLAY.CFG'
    profile.write_bytes(b'AUTO')
    subprocess.run(['mcopy', '-o', '-i', f'{disk}@@{FAT16(disk).start}', str(profile),
                    '::SYSTEM/VIDEO/DISPLAY.CFG'], check=True)
    shell = FAT16(disk).read('SYSTEM/SHELL.COM')
    report = {'shell_sha256': hashlib.sha256(shell).hexdigest(),
              'baseline': args.baseline, 'checks': {}}
    vm = WindowVM(disk, out, 'std', memory=128, video='1280x800', palette='platinum')
    report['qemu_command'] = vm.process.args
    try:
        vm.ready()
        assert Image.open(vm.shot('boot')).size == (1280, 800)
        report['checks']['dos_window_resume'] = resume_case(vm, shell, lines, args.baseline)
        print('PASS dos_window_resume' if not args.baseline else 'REPRODUCED baseline DOS resume defect', flush=True)
        if not args.baseline:
            report['checks']['narrow_sidebar_focus'] = resize_case(vm, shell, lines)
            print('PASS narrow_sidebar_focus', flush=True)
            report['checks']['queued_empty_run'] = empty_case(vm, shell, lines)
            print('PASS queued_empty_run', flush=True)
        report['status'] = 'baseline defect reproduced' if args.baseline else 'passed'
    except Exception as error:
        report['status'] = 'failed'
        report['error'] = str(error)
        saved(vm, 'failure')
        raise
    finally:
        vm.close()
        (out/'results.json').write_text(json.dumps(report, indent=2)+'\n')


if __name__ == '__main__':
    main()
