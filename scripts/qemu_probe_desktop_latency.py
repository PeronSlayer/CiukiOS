#!/usr/bin/env python3
"""Bounded single-VM desktop/input probe; run inside a capped systemd scope.

Uses a snapshot overlay, silent AC97 output, small targeted RAM reads and
overwritten screenshots. No builds, full RAM dumps or disk-image copies.
"""
import argparse
import json
import re
import signal
import struct
import tempfile
import time
from pathlib import Path

from PIL import Image
from qemu_test_native_windows import WindowVM
from qemu_test_installed_hdd import listing_address


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--shell', type=Path, required=True)
    p.add_argument('--listing', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--game', default=r'C:\DESKTOP\TestGames\DOOM.COM -warp 1 1')
    p.add_argument('--cursor-frames', type=int, default=0,
                   help='also check the visible cursor in this many gameplay frames (max 16)')
    p.add_argument('--cache-cycle', action='store_true',
                   help='verify framebuffer release on DOS switch and rebind on desktop return')
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    lines = a.listing.read_text().splitlines()
    names = ('ui_mouse_x', 'ui_mouse_y', 'ui_pointer_visible', 'app_busy',
             'vc_session_bound', 'vc_session_row_status', 'vc_session_row_calls')
    offsets = {n: listing_address(lines, n) + 0x100 for n in names}
    report = {'completed': False, 'input': 'QEMU monitor PS/2',
              'timing': 'host time until shell consumes motion; not display latency'}
    vm = WindowVM(a.image.resolve(), a.output.resolve(), 'std', memory=256,
                  palette='platinum', extra_qemu_args=[
                      '-snapshot', '-audiodev', 'none,id=snd',
                      '-device', 'AC97,audiodev=snd'])

    def terminate(_signum, _frame):
        raise TimeoutError('probe deadline')

    signal.signal(signal.SIGALRM, terminate)
    signal.signal(signal.SIGTERM, terminate)
    signal.alarm(240)

    def read_physical(address):
        result = vm.hmp(f'xp /1wx {address:#x}').decode(errors='replace')
        m = re.search(r'[0-9a-f]+: (0x[0-9a-f]+)', result)
        if not m:
            raise AssertionError('physical read failed')
        return int(m[1], 16)

    def snapshot(label):
        path = vm.shot('current')
        with Image.open(path) as im:
            im.save(a.output / (label + '.png'))

    def phase(label):
        before_log = vm.offset()
        started_phase = time.monotonic()
        def copy_stats():
            if not copy_address:
                return None
            return [read_physical(copy_address + 8 + i * 4) for i in range(4)]
        before_copy = copy_stats()
        samples = []
        for i in range(4):
            before = read_physical(base + offsets['ui_mouse_x'])
            started = time.monotonic()
            vm.hmp(f'mouse_move {24 if i % 2 == 0 else -24} 0 0')
            while read_physical(base + offsets['ui_mouse_x']) == before:
                if time.monotonic() - started > 5:
                    raise AssertionError(label + ': motion not consumed within 5s')
                time.sleep(.002)
            samples.append(round(1000 * (time.monotonic() - started), 2))
            time.sleep(.12)
        state = {n: read_physical(base + offsets[n]) &
                 (0xff if n in ('ui_pointer_visible', 'app_busy', 'vc_session_bound')
                  else 0xffff if n != 'vc_session_row_calls' else 0xffffffff)
                 for n in names}
        report[label] = {'motion_consumed_ms': samples, 'shell': state,
                         'serial_bytes': vm.offset() - before_log,
                         'elapsed_seconds': time.monotonic() - started_phase,
                         'copy_before': before_copy, 'copy_after': copy_stats()}
        snapshot(label)
        print(label, json.dumps(report[label]), flush=True)

    try:
        vm.ready()
        with tempfile.TemporaryDirectory(prefix='ciuki-probe-', dir='/dev/shm') as temp:
            low = Path(temp) / 'low.bin'
            vm.hmp('stop')
            try:
                vm.hmp(f'pmemsave 0x10000 0x90000 "{low}"')
                found = low.read_bytes().find(a.shell.read_bytes()[:128])
                if found < 0:
                    raise AssertionError('image shell does not match supplied binary')
                base = 0x10000 + found - 0x100
                # Only the low two MiB above conventional RAM: locate tagged
                # resident diagnostics once, then use four-word reads.
                vm.hmp(f'pmemsave 0x100000 0x200000 "{low}"')
                resident = low.read_bytes()
                copy_address = None
                for match in re.finditer(b'CVFBTIME', resident):
                    if struct.unpack_from('<I', resident, match.start() + 8)[0]:
                        copy_address = 0x100000 + match.start()
                        break
                cache = resident.find(b'CVFBCACH')
                cache_address = None
                if cache >= 0:
                    cache_address = 0x100000 + cache
                    report['cache_snapshot_hex'] = resident[cache:cache + 712].hex()
            finally:
                vm.hmp('cont')
        report['shell_physical'] = hex(base)
        phase('idle')
        off = vm.offset()
        vm.hmp('sendkey f3 200')
        time.sleep(1)
        vm.text(a.game)
        vm.wait('ST_Init: Init status bar.', off, 60)
        time.sleep(8)
        phase('new-game')
        if a.cursor_frames:
            assert 0 < a.cursor_frames <= 16
            expected = (read_physical(base + offsets['ui_mouse_x']) & 0xffff,
                        read_physical(base + offsets['ui_mouse_y']) & 0xffff)
            observed = []
            for _ in range(a.cursor_frames):
                try:
                    observed.append(vm._pointer())
                except AssertionError:
                    observed.append(None)
            report['cursor_frames'] = {'expected': expected, 'observed': observed}
            assert all(x == expected for x in observed), 'cursor absent or stale during gameplay'
        # Full key holds avoid losing a brief monitor-injected key while the
        # broken guest is still slow; require actual teardown, not a timeout.
        vm.hmp('sendkey f10 1000')
        time.sleep(2)
        vm.hmp('sendkey y 1000')
        vm.wait('[DOSVM] ended', off, 40)
        time.sleep(1)
        phase('after-exit')
        if a.cache_cycle:
            assert cache_address, 'cache diagnostic record absent'
            def cache_state():
                return {'live': read_physical(cache_address + 12),
                        'write_combining': read_physical(cache_address + 576)}
            report['cache_cycle'] = {'before_release': cache_state()}
            assert cache_state() == {'live': 1, 'write_combining': 1}
            # The ended DOS window still owns keyboard focus. Give the
            # desktop focus explicitly before its F4 full-screen DOS action.
            vm.position(1100, 80)
            vm.hmp('mouse_button 1')
            time.sleep(.2)
            vm.hmp('mouse_button 0')
            time.sleep(.2)
            off = vm.offset()
            vm.hmp('sendkey f4 200')
            vm.wait('[DESKTOP] DOS', off, 10)
            deadline = time.monotonic() + 10
            while cache_state()['live'] and time.monotonic() < deadline:
                time.sleep(.05)
            report['cache_cycle']['released'] = cache_state()
            assert cache_state() == {'live': 0, 'write_combining': 0}, 'framebuffer release failed'
            time.sleep(1)
            off = vm.offset()
            vm.text('desktop')
            vm.ready(off)
            report['cache_cycle']['rebound'] = cache_state()
            assert cache_state() == {'live': 1, 'write_combining': 1}, 'framebuffer rebind failed'
            phase('desktop-rebound')
        report['completed'] = True
    except Exception as error:
        report['error'] = repr(error)
        raise
    finally:
        signal.alarm(0)
        vm.close()
        (a.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
