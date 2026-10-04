"""Bounded 0.8.3 UI checks, called inside the single native-GPU QEMU scope."""
import re
import time

import numpy as np
from PIL import Image
from qemu_test_native_desktop import match_pointer


def validate_release_ui(vm, shot, gpu_state, report, mouse_state):
    def run(command):
        vm.key('f3')
        time.sleep(.15)
        vm.text(command)

    def settle():
        time.sleep(.5)

    def gpu_size(width, height):
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            state = gpu_state()
            if state['enabled'] == 1 and state['error_stage'] == 0 and (state['width'], state['height']) == (width, height):
                return state
            time.sleep(.2)
        raise AssertionError(f'GPU did not reach {width}x{height}: {state}')

    vm._pointer = lambda: match_pointer(np.asarray(Image.open(shot('pointer')).convert('RGB')), *vm.cursor_colors)
    ui_start = vm.offset()
    vm.key('alt-f4')  # the finished DOS window
    settle()
    start = vm.offset()
    run('CIUKWEB http://example.com/')
    vm.wait('[CIUKWEB] rendered http://example.com/', start, 25)
    vm.wait('[CIUKWEB] HTTP status=200 bytes=', start, 1)
    shot('web')
    report['internet_http_dns'] = True
    start = vm.offset()
    vm.key('ctrl-r')
    vm.wait('[CIUKWEB] rendered http://example.com/', start, 25)
    vm.wait('[CIUKWEB] HTTP status=200 bytes=', start, 1)
    report['http_reload_reclaims_network'] = True
    vm.key('alt-f4')
    settle()

    start = vm.offset()
    run(r'FILES C:\SYSTEM')
    vm.wait('[FILES] geometry', start, 15)
    settle()
    raw = vm.serial.read_bytes()[start:]
    geometry = re.findall(rb'\[FILES\] geometry (\d+) (\d+) (\d+) (\d+)', raw)
    assert geometry, 'Files did not report its list position'
    x, y, _, _ = map(int, geometry[-1])
    vm.position(x + 140, y + 60)
    settle()
    before = np.asarray(Image.open(shot('files-wheel-before')).convert('RGB'))[y + 3:y + 180, x + 5:x + 110].copy()
    report['mouse_before_wheel'] = mouse_state()
    # HMP positive dz is wheel-up; QEMU encodes that as negative PS/2 Z.
    vm.hmp('mouse_move 0 0 -3')
    settle()
    down = np.asarray(Image.open(shot('files-wheel-down')).convert('RGB'))[y + 3:y + 180, x + 5:x + 110].copy()
    report['mouse_after_wheel_down'] = mouse_state()
    changed = np.count_nonzero(before != down) > 100
    vm.hmp('mouse_move 0 0 3')
    settle()
    up = np.asarray(Image.open(shot('files-wheel-up')).convert('RGB'))[y + 3:y + 180, x + 5:x + 110].copy()
    report['ps2_wheel_files'] = bool(changed and np.array_equal(before, up))
    vm.key('alt-f4')
    settle()

    start = vm.offset()
    run('DISPLAY')
    vm.wait('[DISPLAY] monitor', start, 15)
    settle()
    shot('display')
    original = gpu_state()
    vm.key('home')
    for _ in range(4): vm.key('down')  # 8, 15, 16, 24, 32-bit modes
    vm.key('ret')
    gpu_size(640, 480)
    settle()
    shot('display-preview')
    # Exercise the unattended rollback, not just Escape.
    time.sleep(12.5)
    gpu_size(original['width'], original['height'])
    vm.wait('[DISPLAY] reverted graphics', start, 5)
    report['display_timeout_reverted'] = True
    # Banked 16-bit rendering used to overrun the compositor scratch segment.
    start16 = vm.offset()
    vm.key('home')
    vm.key('down'); vm.key('down')
    vm.key('ret')
    time.sleep(2)
    shot('display-16bit-preview')
    vm.key('esc')
    vm.wait('[DISPLAY] reverted graphics', start16, 5)
    gpu_size(original['width'], original['height'])
    assert b'Exception' not in vm.serial.read_bytes()[start16:], 'banked VBE exception'
    report['display_16bit_banked_reverted'] = True
    vm.key('home')
    for _ in range(4): vm.key('down')
    vm.key('ret')
    gpu_size(640, 480)
    settle()
    vm.key('ret')
    vm.wait('[DISPLAY] kept graphics', start, 5)
    vm.key('alt-f4')
    settle()

    start = vm.offset()
    run('ABOUT')
    vm.wait('[ABOUT] open', start, 10)
    vm.key('tab'); vm.key('spc')
    vm.wait('[ABOUT] show at startup 0', start, 5)
    vm.key('esc')
    settle()
    end = vm.offset()
    ui_log = vm.serial.read_bytes()[ui_start:end]
    assert b'[DESKTOP] DOS' not in ui_log and b'[DESKTOP] RUN ' not in ui_log, 'native UI invoked the full-screen DOS command path'
    report['native_operations_stay_graphical'] = True
    vm.hmp('system_reset')
    vm.ready(end)
    gpu_size(640, 480)
    boot_log = vm.serial.read_bytes()[end:]
    assert b'[ABOUT] open' not in boot_log, 'About startup preference was not retained'
    shot('desktop-preference-saved')
    report['display_profile_persisted'] = True
    report['about_preference_persisted'] = True
    start = vm.offset()
    vm.key('f1')
    vm.wait('[ABOUT] open', start, 5)
    shot('about-640')
    vm.key('esc')
    assert report['ps2_wheel_files'], 'mouse wheel did not scroll Files down and back up'
