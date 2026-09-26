#!/usr/bin/env python3
"""Check real make/break actions in a Doom-recorded demo, not inferred pixels.

QEMU receives ordinary PS/2 key events. Doom writes the .LMP itself through DOS;
its actual ticcmd attack/use bits and movement prove the engine consumed them.
This covers the existing cooperative source port, not arbitrary DOS programs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import time

from qemu_test_graphics_window import Graphics
from qemu_test_installed_hdd import FAT16
from qemu_test_native_windows import WindowVM


def decode_demo(data):
    assert len(data) >= 18 and data[0] in (109, 111), 'unexpected demo header'
    assert data[9:13] == bytes((1, 0, 0, 0)), 'expected single player demo'
    size = 5 if data[0] == 111 else 4
    assert data[-1] == 0x80 and (len(data)-14) % size == 0, 'unfinished demo'
    frames = [data[p:p+size] for p in range(13, len(data)-1, size)]
    runs = []
    for frame in frames:
        action = frame[-1] & 3
        if runs and runs[-1]['buttons'] == action:
            runs[-1]['tics'] += 1
        else:
            runs.append(dict(buttons=action, tics=1))
    return dict(tics=len(frames), action_runs=runs,
                fire_tics=sum(bool(f[-1] & 1) for f in frames),
                use_tics=sum(bool(f[-1] & 2) for f in frames),
                simultaneous_tics=sum(f[-1] & 3 == 3 for f in frames),
                moving_fire_tics=sum(bool(f[0]) and bool(f[-1] & 1) for f in frames))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--listing', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--expect-broken', action='store_true')
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    assert not (a.output/'report.json').exists(), 'preserve previous evidence'
    original = hashlib.sha256(a.image.read_bytes()).hexdigest()
    disk = a.output/'private.img'
    shutil.copyfile(a.image, disk)
    report = dict(passed=False, events=[], source_image_sha256=original,
                  guest_memory_writes=False, guest_pauses=False,
                  expected_broken=a.expect_broken, scope='cooperative Doom backend only')
    vm = WindowVM(disk, a.output, 'std', memory=128, palette='platinum')
    report['qemu'] = vm.process.args
    try:
        vm.ready()
        ui = Graphics(vm, FAT16(disk).read('SYSTEM/SHELL.COM'), a.listing, report)
        # The name is deliberately the final argument. Its exact output name
        # verifies the whole Run -> COMMAND /W /C -> launcher -> DOS4GW tail.
        # The production launcher supplies -nogui for DOS error/record exits.
        inline_nogui = ' -nogui' if a.expect_broken else ''
        off = ui.launch('DWIN.COM'+inline_nogui+' -warp 1 1 -nomonsters -record KEYTEST')
        ui.await_graphics('Doom started actual demo recording', 90)
        vm.wait('[CGFX] Doom source port 320x200', off, 10)
        time.sleep(1)
        ui.crop('before-input')
        # Release gaps must produce zero button tics: key-up matters as much
        # as key-down, including the E0-prefixed right Ctrl and multi-key use.
        for keys in ('ctrl', 'spc', 'ctrl-spc', 'ctrl_r', 'ctrl-up'):
            vm.hmp('sendkey '+keys+' 550')
            time.sleep(.9)
            ui.event('physical PS/2 chord', keys=keys, hold_ms=550)
        time.sleep(.4)
        ui.crop('after-input')
        vm.key('q')  # Doom's normal end-recording key flushes the real .LMP.
        ui.finish('Doom keyboard demo')
        vm.key('f3')
        ui.until(lambda: ui.b('ui_active_window') == 1, 'native keyboard after exit')
        ui.click(18, 1)
        report['native_input_restored'] = True
        vm.close()
        data = FAT16(disk).read('APPS/KEYTEST.LMP')
        (a.output/'KEYTEST.LMP').write_bytes(data)
        result = decode_demo(data)
        report['demo'] = result
        buttons = [r['buttons'] for r in result['action_runs']]
        if a.expect_broken:
            assert buttons == [0], ('baseline bug no longer reproduced', result)
        else:
            # Chord events are individual PS/2 bytes. A game tic may sample
            # Ctrl before Space's make, or after Space's break; do not demand
            # atomic simultaneous delivery from real keyboard hardware.
            groups = []
            for run in result['action_runs']:
                if not run['buttons']:
                    if groups and groups[-1]:
                        groups.append([])
                else:
                    if not groups:
                        groups.append([])
                    groups[-1].append(run['buttons'])
            groups = [group for group in groups if group]
            assert len(groups) == 5, result
            assert groups[:2] == [[1], [2]] and groups[3:] == [[1], [1]], result
            assert 3 in groups[2] and set(groups[2]) <= {1, 2, 3}, result
            assert result['simultaneous_tics'] >= 8, result
            assert result['action_runs'][-1]['buttons'] == 0, result
            assert result['action_runs'][-1]['tics'] >= 8, result
            assert result['moving_fire_tics'] >= 8, result
        report['passed'] = True
    except Exception as exc:
        report['error'] = repr(exc)
        raise
    finally:
        vm.close()
        assert hashlib.sha256(a.image.read_bytes()).hexdigest() == original
        (a.output/'report.json').write_text(json.dumps(report, indent=2)+'\n')


if __name__ == '__main__':
    main()
