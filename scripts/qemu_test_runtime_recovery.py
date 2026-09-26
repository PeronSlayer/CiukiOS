#!/usr/bin/env python3
"""Verify DOS results, nested execution and explicit video changes on the ISO."""
import argparse
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import time
import numpy as np
from PIL import Image
from qemu_test_setup_graphical import SetupVM
from qemu_test_native_desktop import DesktopVM
from analyze_audio_wav import pcm_payload


class RuntimeVM(DesktopVM):
    def __init__(self, disk, iso, output, vga):
        SetupVM.__init__(self, [(0, disk)], iso, output, qemu_args=[
            '-vga', vga, '-audiodev', f'wav,id=snd,path={output}/audio.wav',
            '-device', 'AC97,audiodev=snd'])

    def result(self, command, *expected):
        offset = self.command(command, timeout=60)
        result = subprocess.check_output([
            'scripts/serial_log_normalize.py', '--offset', str(offset), str(self.serial)])
        # Results must follow Enter; matching a typed command is not success.
        body = result.partition(b'\r\n')[2]
        for text in expected:
            assert text.encode() in body, f'{command}: missing output {text!r}: {body!r}'
        for error in (b'execution failed', b'not found', b'SHELL.COM missing', b'WOOF'):
            assert error not in body, f'{command}: {body!r}'
        return body


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--iso', type=Path, default=Path('build/full/CiukiOS_full_cd_0-7-1.iso'))
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--boot', choices=('live', 'safe', 'dos'), default='live')
    ap.add_argument('--vga', choices=('std', 'cirrus'), default='std')
    args = ap.parse_args()
    os.environ.setdefault('CIUKIOS_SETUP_QEMU_ACCEL', 'kvm')
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    target = out/'untouched.img'
    with target.open('wb') as f:
        f.truncate(128*1024*1024)
    original = hashlib.file_digest(target.open('rb'), 'sha256').digest()
    vm = RuntimeVM(target, args.iso.resolve(), out, args.vga)
    try:
        vm.boot_menu({'live': 0, 'safe': 2, 'dos': 3}[args.boot])
        if args.boot == 'live' and args.vga == 'std':
            first = last = None
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline:
                path = vm.shot('boot-frame')
                im = Image.open(path)
                if im.size == (800, 600) and len(set(im.get_flattened_data())) > 64:
                    last = time.monotonic()
                    if first is None:
                        first = last
                        im.save(out/'splash-first.png')
                    im.save(out/'splash-last.png')
                if '[DESKTOP] READY' in vm.serial.read_text(errors='replace'):
                    break
                time.sleep(.15)
            assert first is not None and last-first >= 2.0, f'photo lasted {None if first is None else last-first}s'
            print(f'[runtime-recovery] PASS photo visible >= {last-first:.2f}s', flush=True)
        if args.boot == 'dos':
            vm.wait('[BOOT-SESSION] DOS', timeout=90)
            vm.wait('CiukiOS SHELL D:\\APPS>')
            assert '[DESKTOP] READY' not in vm.serial.read_text(errors='replace')
        else:
            vm.ready()
            boot_size = (1280,800) if args.boot == 'live' and args.vga == 'std' else (640,480)
            vm.shot('desktop', boot_size)
            vm.edges(boot_size)
            vm.dos()
        vm.shot('dos')
        vm.result('dir', 'COMDEMO.COM', 'MZDEMO.EXE', 'SETUP.COM', 'COSTA <DIR>')
        vm.result('dir \\system', 'SHELL.COM', 'CIUKIDOS.SYS')
        vm.result('mkdir RTCHECK')
        vm.result('copy comdemo.com RTCHECK\\COPY.COM', 'File copied')
        vm.result('copy mzdemo.exe RTCHECK\\COPY.EXE', 'File copied')
        vm.result('copy \\command.com RTCHECK\\CMD.COM', 'File copied')
        for _ in range(3):
            # Real COM, relocated MZ, then a second command interpreter which
            # itself EXECs the copied COM and returns to the original shell.
            vm.result('RTCHECK\\COPY.COM', 'COM demo via INT21h')
            vm.result('RTCHECK\\COPY.EXE', 'MZ demo via INT21h')
            vm.result('RTCHECK\\CMD.COM /C RTCHECK\\COPY.COM', 'COM demo via INT21h')
            vm.result('dir RTCHECK', 'COPY.COM', 'COPY.EXE', 'CMD.COM')
        vm.result('del RTCHECK\\COPY.COM')
        body = vm.result('dir RTCHECK', 'COPY.EXE', 'CMD.COM')
        assert b'COPY.COM' not in body, 'deleted file is still listed'
        vm.result('del RTCHECK\\COPY.EXE')
        vm.result('del RTCHECK\\CMD.COM')
        vm.result('rmdir RTCHECK')
        body = vm.result('dir', 'SETUP.COM')
        assert b'RTCHECK' not in body, 'removed directory is still listed'
        vm.shot('dos-results')
        print('[runtime-recovery] PASS DIR results, copy/delete, COM/MZ/nested EXEC, parent recovery', flush=True)
        for mode, size in (('800', (800,600)), ('1024', (1024,768)), ('640', (640,480))):
            vm.result(f'vgasetup set {mode}', 'Shared resolution saved')
            vm.shot(f'dos-{mode}', size)
            vm.result('echo VIDEO READY', 'VIDEO READY')
            vm.desktop(); vm.shot(f'desktop-{mode}', size); vm.pointer()
            vm.dos()
            vm.result('dir', 'COMDEMO.COM', 'MZDEMO.EXE')
        print('[runtime-recovery] PASS explicit video settings apply in DOS and desktop, including safe boot', flush=True)
        vm.desktop(); vm.shot('final-desktop', (640,480)); vm.edges((640,480))
        time.sleep(5)
        vm.dos(); vm.result('echo STILL RUNNING', 'STILL RUNNING')
    except Exception:
        vm.shot('failure')
        (out/'registers.log').write_bytes(vm.hmp('info registers'))
        vm.hmp(f'pmemsave 0 0x100000 "{out}/failure-ram.bin"')
        raise
    finally:
        vm.close()
        assert hashlib.file_digest(target.open('rb'), 'sha256').digest() == original, 'host disk changed'
    pcm, bits = pcm_payload((out/'audio.wav').read_bytes())
    assert bits == 16 and len(pcm) % 4 == 0, 'invalid stereo PCM capture'
    samples = np.frombuffer(pcm, dtype='<i2')
    peak = int(np.abs(samples.astype(np.int32)).max(initial=0))
    if args.boot == 'live':
        assert peak > 3000, f'boot PCM was silent: peak={peak}'
    else:
        assert peak == 0, f'{args.boot} unexpectedly played sound'
    print(f'[runtime-recovery] PASS boot PCM peak={peak}; physical disk untouched', flush=True)


if __name__ == '__main__':
    main()
