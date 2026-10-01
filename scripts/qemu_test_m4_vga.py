#!/usr/bin/env python3
"""QEMU gate for five VGA modes from an ordinary DOS COM program in M4."""
import argparse
import json
import shutil
import subprocess
import time
from pathlib import Path

import numpy as np
from PIL import Image

from qemu_test_installed_hdd import FAT16
from qemu_test_native_windows import WindowVM

ROOT = Path(__file__).resolve().parent.parent


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    disk = a.output / 'disk.img'
    shutil.copyfile(a.image, disk)
    probe = a.output / 'VGASEMW.COM'
    subprocess.run(['nasm', '-f', 'bin', '-DHOLD_ALWAYS',
                    str(ROOT / 'src/probes/vm/vga_semantics.asm'), '-o',
                    str(probe)], check=True)
    subprocess.run(['mcopy', '-o', '-i', f'{disk}@@{FAT16(disk).start}',
                    str(probe), '::APPS/VGASEMW.COM'], check=True)
    vm = WindowVM(disk, a.output, 'std', boot_capture=True,
                  memory=128, palette='platinum')
    report = {'passed': False, 'host': 'QEMU', 'checks': {}}

    def serial(offset=0):
        return subprocess.check_output([
            str(ROOT / 'scripts/serial_log_normalize.py'), '--offset',
            str(offset), str(vm.serial)]).decode('cp437', 'replace')

    def send(key):
        vm.hmp(f'sendkey {key} 180')
        time.sleep(.3)

    def click(x, y):
        vm.position(x, y)
        vm.hmp('mouse_button 1')
        time.sleep(.2)
        vm.hmp('mouse_button 0')
        time.sleep(.5)

    try:
        vm.ready()
        before = vm.offset()
        send('meta_l-r')
        vm.wait('WINDOW 01 OPEN', before, 20)
        vm.text('\\APPS\\VGASEMW.COM \\VGAM4.OUT')
        send('ret')
        vm.wait('[DOSVM] fork', before, 45)
        vm.wait('[VGASEM] checkpoints hold for keys', before, 45)
        vm.wait('[DOSVM] video ready', before, 45)
        frames = []
        counts = []
        for index in range(1, 6):
            time.sleep(1.3)
            frame = np.asarray(Image.open(vm.shot(f'vga-checkpoint-{index}')).convert('RGB'))
            # Only the DOS client is sampled; the shell frame is constant.
            client = frame[286:714, 393:1067, :3].copy()
            colors = int(np.unique(client.reshape(-1, 3), axis=0).shape[0])
            frames.append(client)
            counts.append(colors)
            if index == 1:
                time.sleep(2)
                still = np.asarray(Image.open(vm.shot('vga-static-check')).convert('RGB'))[
                    286:714, 393:1067, :3]
                stable_difference = int(np.any(still != client, axis=2).sum())
                assert stable_difference < 1000, stable_difference
                report['checks']['static_guest_frame_pixels_different'] = stable_difference
                click(1002, 272)  # minimize the live VGA DOS window
                time.sleep(.8)
                hidden = np.asarray(Image.open(vm.shot('vga-minimized')).convert('RGB'))[
                    286:714, 393:1067, :3]
                hidden_change = int(np.any(hidden != client, axis=2).sum())
                assert hidden_change > 100000, hidden_change
                click(190, 783)   # restore the DOS taskbar button after Library
                time.sleep(.8)
                restored = np.asarray(Image.open(vm.shot('vga-restored')).convert('RGB'))[
                    286:714, 393:1067, :3]
                restore_difference = int(np.any(restored != client, axis=2).sum())
                assert restore_difference < 10000, restore_difference
                report['checks']['minimize_restore'] = {
                    'hidden_pixels_changed': hidden_change,
                    'restored_pixels_different': restore_difference}
                click(1064, 714)  # refresh the grip hit after taskbar restore
                vm.position(1064, 714)  # lower-right resize grip
                vm.hmp('mouse_button 1')
                time.sleep(.2)
                vm.position(964, 654)
                vm.hmp('mouse_button 0')
                time.sleep(.8)
                shrunk = np.asarray(Image.open(vm.shot('vga-resized')).convert('RGB'))[
                    286:714, 393:1067, :3]
                vacated = int(np.any(shrunk[280:, 450:] != client[280:, 450:], axis=2).sum())
                assert vacated > 10000, vacated
                vm.position(964, 654)
                vm.hmp('mouse_button 1')
                time.sleep(.2)
                vm.position(1060, 710)  # recover the original outer bounds
                vm.hmp('mouse_button 0')
                time.sleep(3)
                expanded = np.asarray(Image.open(vm.shot('vga-expanded')).convert('RGB'))[
                    286:714, 393:1067, :3]
                expand_difference = int(np.any(expanded != client, axis=2).sum())
                report['checks']['resize_restore'] = {
                    'vacated_pixels_changed': vacated,
                    'restored_pixels_different': expand_difference,
                    'exact_restoration': expand_difference < 10000}
            send('spc')
        vm.wait('[VGASEM] results written', before, 45)
        vm.wait('[DOSVM] ended', before, 30)
        changes = [int(np.any(frames[i] != frames[i + 1], axis=2).sum())
                   for i in range(4)]
        assert all(n >= minimum for n, minimum in
                   zip(counts, (100, 10, 5, 4, 3))), counts
        assert all(n > 10000 for n in changes), changes
        assert '[VGASEM] FAIL' not in serial(before), serial(before)
        report['checks'] = {'mode_color_counts': counts,
                            'changed_pixels_between_modes': changes,
                            'clean_exit': True,
                            **report['checks']}
        assert report['checks']['resize_restore']['exact_restoration'], \
            'VGA pixels changed after restoring the original window size'
        report['passed'] = True
    except Exception as exc:
        report['error'] = repr(exc)
        try:
            vm.shot('failure')
        except Exception:
            pass
        raise
    finally:
        (a.output / 'serial.log').write_text(serial())
        vm.close()
        (a.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('[m4-vga] PASS', a.output)


if __name__ == '__main__':
    main()
