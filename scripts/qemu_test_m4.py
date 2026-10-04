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
    p.add_argument('--audio-only', action='store_true',
                   help='finish after the DOS-window PCM assertion for focused diagnostics')
    p.add_argument('--diagnostic-vm-state', action='store_true',
                   help='capture VM scheduler and keyboard counters around VM2 input')
    a = p.parse_args()
    if a.audio_only and not a.verify_game_audio:
        p.error('--audio-only requires --verify-game-audio')
    if a.verify_game_audio and any(name in a.game.upper()
                                   for name in ('DOOM', 'PCDMCORE')):
        words = a.game_args.lower().split()
        if not any(i + 1 < len(words) and words[i + 1][0] != '-'
                   for i, word in enumerate(words)
                   if word in ('-warp', '-skill', '-episode')):
            p.error('--verify-game-audio for Doom needs -warp, -skill, or '
                    '-episode with a value to start gameplay')
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

    def vm_state(label):
        if not a.diagnostic_vm_state:
            return
        dump = a.output / 'vm-state.bin'
        vm.hmp(f'pmemsave 0 0x8000000 "{dump}"')
        blob = dump.read_bytes()
        if label in ('failure', 'audio_pass'):
            dump.rename(a.output / f'vm-state-{label.replace("_", "-")}.bin')
        else:
            dump.unlink()
        at = blob.find(b'CVMMSTAT')
        assert at >= 32, 'VM scheduler state absent'
        words = struct.unpack_from('<' + 'I' * 13, blob, at + 8)
        names = ('ticks', 'switches', 'gated', 'yields', 'yield_calls',
                 'yield_seen', 'last_exit', 'idle', 'focus', 'audio_urgent',
                 'mouse_home', 'seen', 'tick_owners')
        state = dict(zip(names, words))
        state['status_physical'] = at
        state['cpu_regs'] = vm.hmp('info registers').decode('utf-8', 'replace')[-5000:]
        tr_match = re.search(r'TR =[0-9a-fA-F]{4} ([0-9a-fA-F]{8})', state['cpu_regs'])
        cr3_match = re.search(r'CR3=([0-9a-fA-F]{8})', state['cpu_regs'])
        if tr_match and cr3_match:
            tr_linear, cr3 = int(tr_match.group(1), 16), int(cr3_match.group(1), 16)
            pde = struct.unpack_from('<I', blob,
                                     (cr3 & 0xfffff000) + ((tr_linear >> 22) & 1023)*4)[0]
            pte = struct.unpack_from('<I', blob,
                                     (pde & 0xfffff000) + ((tr_linear >> 12) & 1023)*4)[0]
            tr_physical = (pte & 0xfffff000) + (tr_linear & 4095)
            io_offset = struct.unpack_from('<H', blob, tr_physical + 0x66)[0]
            io20 = blob[tr_physical + io_offset + 0x20//8]
            state['tss_io'] = {'linear': tr_linear, 'physical': tr_physical,
                               'pde': pde, 'pte': pte, 'io_offset': io_offset,
                               'byte20': io20, 'port20_trapped': bool(io20 & 1),
                               'port21_trapped': bool(io20 & 2)}
        state['current'] = struct.unpack_from('<I', blob, at - 16)[0]
        state['live'] = struct.unpack_from('<I', blob, at - 12)[0]
        state['count'] = struct.unpack_from('<I', blob, at - 20)[0]
        base = struct.unpack_from('<I', blob, at - 28)[0]
        state['kernel_base'] = base
        if base + 0xA000 < len(blob):
            state['kernel'] = {key: blob[base + offset] for key, offset in
                               (('fat_dirty', 0x97DF), ('indos', 0x9A51),
                                ('last_ah', 0x94C5))}
        records = []
        table = at + 0x47C
        for number in range(min(state['count'], 4)):
            rec = table + number * 0xC7C
            status, valid, missed, _, session, _, dev_instance = struct.unpack_from(
                '<7I', blob, rec)
            cv = rec + 0x87C
            guest_if, pending, in_service = struct.unpack_from('<III', blob, cv)
            mask = struct.unpack_from('<H', blob, cv + 16)[0]
            frame = struct.unpack_from('<19I', blob, rec + 0x830)
            ip, cs, flags, sp, ss = frame[10:15]
            linear = ((cs & 0xffff) << 4) + (ip & 0xffff)
            pte = (struct.unpack_from('<I', blob, rec + 0x8BC + (linear >> 12) * 4)[0]
                   if linear < 0xC0000 else 0)
            physical = (pte & 0xfffff000) + (linear & 0xfff) if pte & 1 else 0
            code = blob[physical:physical+16].hex() if pte & 1 else ''
            code_before = blob[max(0, physical-96):physical].hex() if pte & 1 else ''
            stack_linear = ((ss & 0xffff) << 4) + (sp & 0xffff)
            stack_pte = (struct.unpack_from('<I', blob,
                                           rec + 0x8BC + (stack_linear >> 12) * 4)[0]
                         if stack_linear < 0xC0000 else 0)
            stack_physical = ((stack_pte & 0xfffff000) + (stack_linear & 0xfff)
                              if stack_pte & 1 else 0)
            stack = blob[stack_physical:stack_physical+32].hex() if stack_pte & 1 else ''
            ivt_pte = struct.unpack_from('<I', blob, rec + 0x8BC)[0]
            ivt_physical = ivt_pte & 0xfffff000 if ivt_pte & 1 else 0
            bda = ivt_physical + 0x400
            keyboard_head, keyboard_tail = struct.unpack_from('<HH', blob, bda + 0x1a)
            keyboard_ring = list(struct.unpack_from('<16H', blob, bda + 0x1e))
            int16_off, int16_seg = struct.unpack_from('<HH', blob, ivt_physical + 0x16*4)
            irq9_off, irq9_seg = struct.unpack_from('<HH', blob, ivt_physical + 9*4)
            irq9_linear = (irq9_seg << 4) + irq9_off
            irq9_pte = (struct.unpack_from('<I', blob,
                        rec + 0x8BC + (irq9_linear >> 12)*4)[0]
                        if irq9_linear < 0xC0000 else 0)
            irq9_physical = ((irq9_pte & 0xfffff000) + (irq9_linear & 0xfff)
                             if irq9_pte & 1 else irq9_linear)
            irq9_code = blob[irq9_physical:irq9_physical+512]
            private_linear = (irq9_seg << 4) + 0x9619
            private_pte = struct.unpack_from('<I', blob,
                          rec + 0x8BC + (private_linear >> 12)*4)[0]
            private_physical = (private_pte & 0xfffff000) + (private_linear & 0xfff)
            old_off, old_seg = struct.unpack_from('<HH', blob, private_physical)
            irq9_flag = blob[private_physical + 4]
            old_linear = (old_seg << 4) + old_off
            old_pte = (struct.unpack_from('<I', blob,
                       rec + 0x8BC + (old_linear >> 12)*4)[0]
                       if old_linear < 0xC0000 else 0)
            old_physical = ((old_pte & 0xfffff000) + (old_linear & 0xfff)
                            if old_pte & 1 else old_linear)
            old_code = blob[old_physical:old_physical+512]
            if number == 2:
                (a.output / f'vm2-irq9-{label}.bin').write_bytes(irq9_code)
                (a.output / f'vm2-irq9-old-{label}.bin').write_bytes(old_code)
            records.append({'vm': number, 'status': status, 'valid': valid,
                            'missed': missed, 'session': session,
                            'dev_instance': dev_instance, 'guest_if': guest_if,
                            'pending': pending, 'in_service': in_service,
                            'mask': mask, 'frame': {'ip': ip, 'cs': cs,
                                                   'flags': flags, 'sp': sp, 'ss': ss,
                                                   'linear': linear, 'physical': physical,
                                                   'code': code,
                                                   'code_before': code_before,
                                                   'stack_linear': stack_linear,
                                                   'stack_physical': stack_physical,
                                                   'stack': stack}})
            records[-1]['bios_keyboard'] = {
                'head': keyboard_head, 'tail': keyboard_tail,
                'ring': keyboard_ring, 'int16': f'{int16_seg:04X}:{int16_off:04X}'}
            records[-1]['irq9'] = {'segment': irq9_seg, 'offset': irq9_off,
                                   'linear': irq9_linear, 'physical': irq9_physical,
                                   'code_prefix': irq9_code[:64].hex(),
                                   'old_segment': old_seg, 'old_offset': old_off,
                                   'old_physical': old_physical, 'flag': irq9_flag,
                                   'old_code_prefix': old_code[:64].hex()}
        state['records'] = records
        devices = []
        at = blob.find(b'CVDV')
        while at >= 0:
            if at + 8 + 22 * 4 <= len(blob) and blob[at+4:at+6] == b'\x00\x01':
                fields = struct.unpack_from('<' + 'I' * 22, blob, at + 8)
                devices.append({'address': at, 'active': fields[0],
                                'focused': fields[2], 'keys_forwarded': fields[4],
                                'keys_dropped': fields[5], 'irqs_raised': fields[8],
                                'polls': fields[18], 'sb_blocks': fields[19],
                                'dsp_commands': fields[21],
                                'model_active': blob[at+128+23],
                                'model_focused': blob[at+128+24],
                                'kbc_count': struct.unpack_from('<H', blob, at+128+2420)[0],
                                'kbc_command': blob[at+128+2422],
                                'pic1': list(blob[at+128+2435:at+128+2438])})
            at = blob.find(b'CVDV', at + 1)
        state['devices'] = sorted(devices, key=lambda item: item['polls'], reverse=True)[:4]
        trace_at = blob.find(b'CVATRC01')
        if trace_at >= 0:
            count, tsc_per_us = struct.unpack_from('<II', blob, trace_at + 8)
            events = []
            for seq in range(max(0, count - 64), count):
                low, high, pm, completed, pumped, raised, dma, kind, value = \
                    struct.unpack_from('<IIIIIIIHH', blob,
                                       trace_at + 16 + (seq & 63) * 32)
                events.append({'seq': seq, 'tsc': (high << 32) | low,
                               'kind': kind, 'value': value, 'pm': pm,
                               'completed': completed & 0xffff,
                               'pumped': pumped, 'raised': raised & 0xffff,
                               'gate': (completed >> 16) & 0xffff,
                               'gated_switches': raised >> 16,
                               'current_vm': (dma >> 18) & 7,
                               'urgent_vm': (dma >> 21) & 7})
            if events and tsc_per_us:
                start = events[0]['tsc']
                for event in events:
                    event['ms'] = round((event.pop('tsc') - start) / tsc_per_us / 1000, 2)
            state['audio_trace'] = events
        report['checks'].setdefault('vm_state', {})[label] = state

    def capture_live_vm2():
        if not a.diagnostic_vm_state:
            return
        state = report['checks'].get('vm_state', {}).get('failure')
        if not state or state['count'] < 3:
            return
        current_at = state['status_physical'] - 16
        current_file = a.output / 'live-current.bin'
        for attempt in range(300):
            vm.hmp(f'pmemsave {current_at} 4 "{current_file}"')
            if struct.unpack('<I', current_file.read_bytes())[0] != 2:
                continue
            vm.hmp('stop')
            vm.hmp(f'pmemsave {current_at} 4 "{current_file}"')
            if struct.unpack('<I', current_file.read_bytes())[0] == 2:
                report['checks']['live_vm2'] = {
                    'attempt': attempt,
                    'registers': vm.hmp('info registers').decode('utf-8', 'replace')[-5000:]}
                vm.hmp(f'pmemsave 0 0x8000000 "{a.output / "vm2-live.bin"}"')
                vm.hmp('cont')
                current_file.unlink(missing_ok=True)
                return
            vm.hmp('cont')
        current_file.unlink(missing_ok=True)
        report['checks']['live_vm2'] = {'attempts': 300, 'found': False}

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
            ammo_before = frame[650:702, 420:488, :3]
            fired = False
            for attempt in range(3):
                wait_focus(1)
                for _ in range(5):
                    keys('ctrl')      # fire: effects must play with -nomusic
                time.sleep(2)
                after = np.asarray(Image.open(vm.shot('after-fire'))).astype(np.int16)
                if np.count_nonzero(after[650:702, 420:488, :3] != ammo_before) > 300:
                    fired = True
                    break
            report['checks']['game_input'] = {'fire_confirmed': fired,
                                               'attempts': attempt + 1}
            pcm = audio_path.read_bytes()[max(44, audio_before):]
            energy = sum(abs(int.from_bytes(pcm[i:i+2], 'little', signed=True))
                         for i in range(0, len(pcm)-1, 128))
            if not fired:
                vm_state('game_input_failure')
            assert fired, 'game did not fire after focused Ctrl input'
            assert energy > 10000, f'gameplay PCM is silent after confirmed fire ({energy})'
            report['checks']['game_audio'] = {'sampled_energy': energy,
                                               'sampled_bytes': len(pcm)}
        if a.audio_only:
            vm_state('audio_pass')        # comparison dump for audio diagnosis
            report['checks']['audio_only'] = True
            report['passed'] = True
            print('[m4-audio] PASS', a.output)
            return
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
        pressed_at = click(52, 14)    # CiukiOS logo drops its menu by real PS/2 input
        vm.wait('[DESK] menu start', second, 10)
        menu_latency = time.monotonic() - pressed_at
        assert menu_latency < 5, f'desktop system menu response took {menu_latency:.2f}s'
        pressed_at = click(44, 81)    # Run... (third item of the drop-down menu)
        vm.wait('WINDOW 01 OPEN', second, 10)
        latency = time.monotonic() - pressed_at
        assert latency < 5, f'desktop Run response took {latency:.2f}s'
        report['checks']['desktop_mouse_after_release'] = {
            'menu_open_seconds': round(menu_latency, 2),
            'run_open_seconds': round(latency, 2),
        }
        type_text('\\COMMAND.COM')
        # After the game and a VM focus switch, the guest may still be
        # draining keyboard input while the desktop repaints the Run field.
        # Let the queued characters reach the dialog before sending Enter.
        time.sleep(3)
        vm.shot('second-run-before-enter')
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
        vm_state('before_vm2_keys')
        type_text('echo M4 SECOND VM')
        vm_state('after_vm2_text')
        time.sleep(3)                  # let the focused DOS VM drain queued keys
        keys('ret')
        vm_state('after_vm2_enter')
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
            vm_state('failure')
            try:
                capture_live_vm2()
            except Exception as diagnostic_error:
                report['checks']['live_vm2_error'] = repr(diagnostic_error)
                try:
                    vm.hmp('cont')
                except Exception:
                    pass
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
