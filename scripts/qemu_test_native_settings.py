#!/usr/bin/env python3
"""Exercise native settings through real keys, checking saved files and output."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import time

import numpy as np
from PIL import Image

from analyze_audio_wav import pcm_payload
from qemu_test_native_desktop import DesktopVM


def file_digest(data):
    value = 0x811c9dc5
    for byte in data:
        value = ((value ^ byte) * 0x01000193) & 0xffffffff
    return f'{value:08X}', f'{len(data):08X}'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--iso', type=Path, default=Path('build/full/CiukiOS_full_cd_0-8-0.iso'))
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--case', choices=('all', 'error'), default='all')
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    target = out / 'untouched.img'
    with target.open('wb') as stream:
        stream.truncate(128 * 1024 * 1024)
    before = hashlib.sha256(target.read_bytes()).digest()
    os.environ.setdefault('CIUKIOS_SETUP_QEMU_ACCEL', 'kvm')
    vm = DesktopVM([(0, target)], args.iso.resolve(), out)
    report = {'case': args.case, 'checks': []}
    sound_window = None

    def open_menu_item(index):
        size = Image.open(vm.shot('current')).size
        vm.click_at(44, size[1] - 18)
        # Each newly opened CiukiOS menu starts focused on Programs.
        for _ in range(index):
            vm.repaint_key('tab')
        offset = vm.offset()
        vm.key('ret')
        vm.wait('[DESKTOP] DOS' if index == 3 else '[DESKTOP] PAINT', offset, 30)
        if index != 3:
            vm.pointer()

    def open_settings(index):
        open_menu_item(index)
        # Panels survive child execution, including their keyboard focus.
        # Close and reopen explicitly so navigation starts at their first field.
        vm.repaint_key('esc')
        open_menu_item(index)

    def activate(tab_count):
        for _ in range(tab_count):
            vm.repaint_key('tab')
        offset = vm.offset()
        vm.key('ret')
        return offset

    def returned(offset, name, size):
        vm.ready(offset)
        vm.shot(name, size)
        vm.pointer()

    def digest(path):
        # TYPE streams every file byte through INT21 open/read. FILECHK is a
        # fixture injected by the HDD tests, not an application on the live CD.
        command = f'type {path}'
        offset = vm.command(command)
        output = vm.serial.read_bytes()[offset:]
        prefix = command.encode() + b'\r\n'
        suffix = b'\r\nCiukiOS SHELL D:\\APPS> '
        assert output.startswith(prefix) and output.endswith(suffix), output
        data = output[len(prefix):-len(suffix)]
        assert b'type: cannot open file' not in data, f'cannot read {path}'
        (out / f'file-{len(report["checks"])}-{Path(path.replace(chr(92), "/")).name}').write_bytes(data)
        return file_digest(data)

    def settings(size, profile=None, sound=None):
        # Reopen each file in the guest after returning from the child process.
        # The live image is a RAM disk; the physical target must remain untouched.
        vm.dos()
        result = {'display': digest(r'\SYSTEM\VIDEO\DISPLAY.CFG'),
                  'windows': digest(r'\WINDOWS\SYSTEM.INI')}
        if profile is not None:
            assert result['display'] == file_digest(profile), result
        if sound is not None:
            result['sound'] = digest(r'\SYSTEM\BOOT.SND')
            assert result['sound'] == file_digest(sound), result
        vm.desktop()
        vm.shot(f'persistent-{len(report["checks"])}', size)
        report['checks'].append(result)
        return result

    def preview(tabs, dimensions, response, return_size):
        open_settings(4)
        offset = activate(tabs)
        vm.wait(f'[VGASETUP] DESKTOP PREVIEW {dimensions[0]} x {dimensions[1]}', offset, 60)
        started = time.monotonic()
        vm.shot(f'preview-{dimensions[0]}-{response}', dimensions)
        if response == 'timeout':
            vm.wait('Preview cancelled; settings preserved.', offset, 20)
            elapsed = time.monotonic() - started
            assert 10 <= elapsed <= 16, f'preview timeout: {elapsed:.3f}s'
            report['timeout_seconds'] = elapsed
        else:
            # The preview drains the launching key before accepting confirmation.
            time.sleep(.3)
            vm.key('ret' if response == 'confirm' else 'esc')
            marker = ('Desktop resolution saved; Windows profile preserved.'
                      if response == 'confirm' else 'Preview cancelled; settings preserved.')
            vm.wait(marker, offset, 30)
        returned(offset, f'returned-{dimensions[0]}-{response}', return_size)

    try:
        vm.ready()
        original_size = Image.open(vm.shot('initial')).size
        assert original_size == (1280, 800), original_size
        # A fresh live image intentionally has no DISPLAY.CFG (default AUTO).
        # Save an explicit baseline through the public control before checking
        # transactional persistence; never create a profile in the host image.
        open_settings(4)
        offset = activate(0)
        vm.wait('[DESKTOP] RUN VGASETUP AUTO', offset, 60)
        returned(offset, 'automatic-baseline', original_size)
        original = settings(original_size, b'AUTO')
        if args.case == 'error':
            vm.dos()
            vm.command(r'mkdir \SYSTEM\VIDEO\DISPLAY.NEW')
            vm.desktop()
            open_settings(4)
            offset = activate(0)  # Automatic mode writes the same transactional file.
            vm.wait('profile not changed', offset, 60)
            returned(offset, 'write-error', original_size)
            vm.repaint_key('esc')  # Dismiss the real command-error dialog.
            after = settings(original_size, b'AUTO')
            assert after == original, 'failed profile save changed saved settings'
            vm.dos()
            vm.command(r'rmdir \SYSTEM\VIDEO\DISPLAY.NEW')
            vm.desktop()
            vm.pointer()
            report['write_failure_preserved_settings'] = True
            print('[native-settings] PASS failed write preserves files, resolution and input', flush=True)
        else:
            # Current desktop controls start at 800; 640 remains a DOS/SAFE option.
            preview(1, (800, 600), 'escape', original_size)
            assert settings(original_size, b'AUTO') == original
            preview(1, (800, 600), 'timeout', original_size)
            assert settings(original_size, b'AUTO') == original
            preview(1, (800, 600), 'confirm', (800, 600))
            assert settings((800, 600), b'0800')['windows'] == original['windows']
            preview(2, (1024, 768), 'confirm', (1024, 768))
            assert settings((1024, 768), b'1024')['windows'] == original['windows']
            open_settings(4)
            offset = activate(0)
            vm.wait('[DESKTOP] RUN VGASETUP AUTO', offset, 60)
            returned(offset, 'automatic', original_size)
            assert settings(original_size, b'AUTO') == original
            for tabs, value in ((2, b'0'), (1, b'1')):
                open_settings(5)
                offset = activate(tabs)
                vm.wait('Startup sound preference saved', offset, 60)
                returned(offset, f'sound-{value.decode()}', original_size)
                result = settings(original_size, b'AUTO', value)
                assert result['windows'] == original['windows']
            open_settings(5)
            start = len(pcm_payload((out / 'audio.wav').read_bytes())[0])
            offset = activate(0)
            vm.wait('CiukiOS startup melody played through AC97', offset, 60)
            returned(offset, 'sound-played', original_size)
            end = len(pcm_payload((out / 'audio.wav').read_bytes())[0])
            sound_window = (start, end)
            open_menu_item(3)
            vm.command('echo SETTINGS READY', 'SETTINGS READY\r\nCiukiOS SHELL')
            vm.desktop()
            vm.pointer()
            vm.shot('desktop-return', original_size)
    except Exception:
        vm.shot('failure')
        raise
    finally:
        vm.close()
        assert hashlib.sha256(target.read_bytes()).digest() == before, 'settings changed the physical disk'

    if sound_window is not None:
        payload, bits = pcm_payload((out / 'audio.wav').read_bytes())
        assert bits == 16, bits
        samples = np.frombuffer(payload[sound_window[0]:sound_window[1]], dtype='<i2').astype(np.float64)
        assert len(samples) >= 4410, 'Play sound produced no substantial PCM capture'
        rms = float(np.sqrt(np.mean((samples - samples.mean()) ** 2)))
        unique = len(np.unique(samples))
        assert rms > 100 and unique > 32, f'Play sound is silent/constant: RMS={rms}, values={unique}'
        report['sound_test'] = {'pcm_bytes': list(sound_window), 'ac_rms': rms, 'unique_samples': unique}
        print('[native-settings] PASS preview cancel/timeout/confirm, saved profiles, sound preferences and real PCM', flush=True)
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
