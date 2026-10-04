#!/usr/bin/env python3
"""Bounded native-GPU/new-game check. Run alone inside a capped user scope."""
import argparse
import json
import os
from pathlib import Path
import re
import signal
import statistics
import struct
import subprocess
import tempfile
import time

from qemu_test_native_windows import WindowVM
from qemu_test_installed_hdd import listing_address


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--device', choices=('virtio', 'virtio-gl'), default='virtio-gl')
    parser.add_argument('--release-ui', action='store_true', help='also verify the 0.8.3 desktop, network and saved preferences')
    parser.add_argument('--ui-only', action='store_true', help='skip the already measured Doom case while fixing desktop integration')
    parser.add_argument('--trace', action='store_true', help='bounded GPU command trace for a failed integration')
    parser.add_argument('--display', choices=('sdl', 'gtk'), default='sdl')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    vm = None
    wheel_address = None
    report = {'passed': False, 'device': args.device, 'physical_input_tested': False}

    def expired(*_):
        raise TimeoutError('180-second GPU validation limit')

    signal.signal(signal.SIGALRM, expired)
    signal.signal(signal.SIGTERM, expired)
    signal.alarm(300 if args.release_ui else 180)

    def words(address, count=1):
        raw = vm.hmp(f'xp /{count}wx {address:#x}').decode(errors='replace')
        result = []
        for line in raw.splitlines():
            m = re.search(r'[0-9a-fA-F]+:\s+(0x[0-9a-fA-F]+.*)', line)
            if m:
                result.extend(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]+)', m[1]))
        assert len(result) == count, raw
        return result

    def gpu_state():
        values = words(gpu_address + 8, 11)
        return dict(zip(('size', 'version', 'enabled', 'width', 'height', 'pitch',
                         'device_status', 'submits', 'completions', 'skipped', 'error_stage'), values))

    def mouse_state():
        # A handful of native-kernel counters, read only while VM0 owns the
        # desktop. No full-memory snapshot and no continuous input trace.
        listing = Path('build/full/obj/ciukidos.lst').read_text().splitlines()
        vector = words(0x33 * 4)[0]
        base = (vector >> 16) * 16
        result = {'vector': hex(vector)}
        if wheel_address is not None:
            packed = struct.pack('<III', *words(wheel_address + 8, 3))
            result['shell'] = dict(zip(('samples', 'last_delta', 'target', 'sent', 'result'),
                                       struct.unpack_from('<5H', packed)))
        if (vector & 65535) != listing_address(listing, 'int33_handler'):
            return result
        for name, mask in (('mouse_device_id', 255), ('mouse_packet_bytes', 255),
                           ('mouse_diag_packets', 65535), ('mouse_last_wheel_delta', 65535),
                           ('mouse_wheel_count', 65535)):
            result[name] = words(base + listing_address(listing, name))[0] & mask
        return result

    def engine_addresses():
        # These two offsets belong to the unchanged local DOS Doom binary,
        # documented in 2026-10-03-doom-frame-cadence.md. Walk its actual CR3.
        for _ in range(64):
            registers = vm.hmp('info registers').decode(errors='replace')
            if 'CPL=3' in registers and 'CS =01ef 00000000' in registers:
                cr3 = int(re.search(r'CR3=([0-9a-fA-F]+)', registers)[1], 16)
                addresses = {}
                for key, linear in (('gametic', 0x2a23bc), ('frameon', 0x2a1748)):
                    pde = words((cr3 & ~4095) + (linear >> 22) * 4)[0]
                    assert pde & 1 and not pde & 0x80
                    pte = words((pde & ~4095) + ((linear >> 12) & 1023) * 4)[0]
                    assert pte & 1
                    addresses[key] = (pte & ~4095) + (linear & 4095)
                return addresses
            time.sleep(.01)
        raise AssertionError('Doom protected-mode address space not observed')

    def host_cpu_budget():
        group = next((line.split(':', 2)[2] for line in Path('/proc/self/cgroup').read_text().splitlines()
                      if line.startswith('0::')), None)
        if group is None: return {}
        path = Path('/sys/fs/cgroup') / group.lstrip('/') / 'cpu.stat'
        return {key: int(value) for key, value in (line.split() for line in path.read_text().splitlines())}

    def shot(name):
        if args.device != 'virtio-gl':
            return vm.shot(name)
        # QEMU HMP screendump cannot read this GL-only scanout. Capture only
        # the visible VM client window, never the rest of the host desktop.
        windows = subprocess.check_output(['xdotool', 'search', '--onlyvisible',
                                           '--pid', str(vm.process.pid)],
                                          stderr=subprocess.DEVNULL, timeout=5).split()
        assert len(windows) == 1, windows
        path = output / (name + '.png')
        subprocess.run(['xdotool', 'windowactivate', '--sync', windows[0].decode()],
                       check=True, stderr=subprocess.DEVNULL, timeout=5)
        time.sleep(.15)
        # XGetImage/import sees a stale Xwayland pixmap for direct GL buffers.
        # KWin's window capture reads the compositor's actual presented image.
        subprocess.run(['spectacle', '--activewindow', '--background', '--nonotify',
                        '--no-decoration', '--no-shadow', '--output', str(path)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
        return path

    try:
        os.environ['SDL_VIDEODRIVER'] = 'x11'
        os.environ['SDL_MOUSE_RELATIVE_MODE_WARP'] = '0'
        extra = ['-snapshot', '-device', 'virtio-vga' + ('-gl' if args.device == 'virtio-gl' else '') + ',xres=1024,yres=768',
                 '-audiodev', 'none,id=snd', '-device', 'AC97,audiodev=snd']
        if args.device == 'virtio-gl':
            extra += ['-display', args.display + ',gl=on']
        if args.release_ui:
            # Override the generic fixture's reboot=shutdown: this gate checks
            # saved preferences across a real guest reset in the same overlay.
            extra += ['-action', 'reboot=reset']
            extra += ['-netdev', 'user,id=release_net', '-device', 'ne2k_pci,netdev=release_net,mac=52:54:00:12:34:56']
        if args.trace:
            extra += ['-trace', f'enable=virtio_gpu_*,file={output}/gpu.trace',
                      '-d', 'guest_errors', '-D', str(output / 'gpu-errors.log')]
        vm = WindowVM(Path('build/full/ciukios-full.img').resolve(), output,
                      'none', memory=256, palette='platinum', extra_qemu_args=extra)
        vm.ready()
        # One bounded 4-MiB resident region, removed immediately. No full RAM
        # dump, continuous trace, recording or generated audio file.
        with tempfile.TemporaryDirectory(prefix='ciuki-gpu-', dir='/dev/shm') as temp:
            dump = Path(temp) / 'resident.bin'
            vm.hmp('stop')
            try:
                vm.hmp(f'pmemsave 0x100000 0x400000 "{dump}"')
                data = dump.read_bytes()
                candidates = [m.start() for m in re.finditer(b'CVGPU001', data)
                              if struct.unpack_from('<II', data, m.start() + 8) == (52, 1)]
                assert len(candidates) == 1, candidates
                gpu_address = 0x100000 + candidates[0]
                pace = data.find(b'CVPACE01')
                pace_address = 0x100000 + pace if pace >= 0 else None
                low = Path(temp) / 'shell.bin'
                vm.hmp(f'pmemsave 0x10000 0x90000 "{low}"')
                locations = [m.start() for m in re.finditer(b'CWHEEL01', low.read_bytes())]
                if len(locations) == 1:
                    wheel_address = 0x10000 + locations[0]
            finally:
                vm.hmp('cont')
        report['gpu_physical'] = hex(gpu_address)
        until = time.monotonic() + 5
        while gpu_state()['enabled'] != 1 and time.monotonic() < until:
            time.sleep(.05)
        report['desktop'] = gpu_state()
        assert report['desktop']['enabled'] == 1, report['desktop']
        assert report['desktop']['error_stage'] == 0, report['desktop']
        shot('desktop')
        if args.release_ui:
            vm.wait('[ABOUT] open CiukiOS 0.8.3', 0, 5)
            shot('about')
            vm.key('esc')
            time.sleep(.4)
            shot('bare-desktop')
        report['mouse_before_game'] = mouse_state()
        if not args.ui_only:
            start = vm.offset()
            vm.hmp('sendkey f3 100')
            time.sleep(.4)
            vm.text(r'C:\DESKTOP\TestGames\DOOM.COM')
            vm.wait('ST_Init: Init status bar.', start, 60)
            time.sleep(2)
            for key in ('esc', 'ret', 'ret', 'ret'):
                vm.hmp(f'sendkey {key} 100')
                time.sleep(.4)
            time.sleep(3)
            shot('new-game')
            counters = engine_addresses()
            before = gpu_state()
            engine_before = {key: words(address)[0] for key, address in counters.items()}
            vm.hmp('sendkey right 10000')
            host_before = host_cpu_budget()
            began = time.monotonic()
            samples = []
            while time.monotonic() - began < 10:
                frame = words(counters['frameon'])[0]
                submits, completions = words(gpu_address + 36, 2)
                samples.append([time.monotonic() - began, frame, submits, completions])
                time.sleep(.003)
            (output / 'cadence-samples.json').write_text(json.dumps(samples) + '\n')
            observed = {}
            for index, name, unit in ((1, 'engine_loop', 1), (2, 'gpu_submit', 2), (3, 'gpu_completion', 2)):
                prev_time, prev_value = samples[0][0], samples[0][index]
                intervals, missed = [], 0
                for row in samples[1:]:
                    if row[index] == prev_value: continue
                    delta = row[index] - prev_value
                    if delta == unit: intervals.append((row[0] - prev_time) * 1000)
                    elif delta > unit: missed += delta // unit - 1
                    prev_time, prev_value = row[0], row[index]
                observed[name] = {'samples': len(intervals), 'missed': missed,
                                  'median_ms': statistics.median(intervals) if intervals else None,
                                  'p95_ms': statistics.quantiles(intervals, n=100, method='inclusive')[94] if len(intervals) > 1 else None,
                                  'max_ms': max(intervals) if intervals else None,
                                  'over_42ms': sum(t > 42 for t in intervals)}
            report['observed_cadence'] = observed
            host_after = host_cpu_budget()
            report['host_cpu_budget_delta'] = {key: value - host_before.get(key, 0)
                                               for key, value in host_after.items()}
            if pace_address:
                with tempfile.TemporaryDirectory(prefix='ciuki-pace-', dir='/dev/shm') as temp:
                    dump = Path(temp) / 'pace.bin'
                    vm.hmp('stop')
                    try:
                        vm.hmp(f'pmemsave {pace_address:#x} 32788 "{dump}"')
                        (output / 'cadence.bin').write_bytes(dump.read_bytes())
                    finally:
                        vm.hmp('cont')
            after = gpu_state()
            elapsed = time.monotonic() - began
            engine_after = {key: words(address)[0] for key, address in counters.items()}
            report['game'] = {'before': before, 'after': after, 'elapsed': elapsed,
                              'engine_before': engine_before, 'engine_after': engine_after,
                              'engine_rates': {key: (engine_after[key] - engine_before[key]) / elapsed
                                               for key in counters}}
            assert after['completions'] > before['completions'] + 30, report['game']
            assert after['error_stage'] == 0 and after['enabled'] == 1, report['game']
            shot('game-after-turn')
            assert (output / 'new-game.png').read_bytes() != (output / 'game-after-turn.png').read_bytes(), 'unchanged game capture'
            vm.hmp('sendkey f10 300')
            time.sleep(1)
            vm.hmp('sendkey y 300')
            vm.wait('[DOSVM] ended', start, 30)
            time.sleep(1)
            report['after_game_exit'] = gpu_state()
            report['mouse_after_game'] = mouse_state()
            shot('desktop-after-game')
        if args.release_ui:
            from qemu_release_ui import validate_release_ui
            validate_release_ui(vm, shot, gpu_state, report, mouse_state)
        # Normal full-screen DOS transition must reset VirtIO before freeing
        # its DMA backing.
        vm.hmp('sendkey f4 200')
        time.sleep(2)
        report['after_video_release'] = gpu_state()
        assert report['after_video_release']['enabled'] == 0, report['after_video_release']
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
            finally:
                if vm.process.poll() is None:
                    vm.process.kill()
                    vm.process.wait(timeout=5)
        (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
