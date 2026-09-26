#!/usr/bin/env python3
"""Run SOUND /D via the public DOS console; preserve actual screen/PCM evidence."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

import numpy as np
from PIL import Image
from analyze_audio_wav import pcm_payload
from qemu_test_full_display_profile import VM
from qemu_test_installed_hdd import FAT16


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--sound', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    disk = out / 'target.img'
    original_sha = hashlib.sha256(args.image.read_bytes()).hexdigest()
    shutil.copyfile(args.image, disk)
    fat = FAT16(disk)
    for path in ('SYSTEM/BOOTSND.COM', 'SYSTEM/DRIVERS/SOUND.COM'):
        fat.read(path)  # require real packaged paths rather than inventing commands
        subprocess.run(['mcopy', '-o', '-i', f'{disk}@@{fat.start}',
                        str(args.sound), '::' + path], check=True)
    vm = VM(disk, out, ('-audiodev', f'wav,id=snd,path={out}/audio.wav',
                       '-device', 'AC97,audiodev=snd'), memory=128)
    report = {'source_sha256': original_sha,
              'sound_sha256': hashlib.sha256(args.sound.read_bytes()).hexdigest(),
              'launch': 'public DOS console SOUND /D typed with PS/2 keys', 'memory_mb': 128}
    try:
        vm.wait('CiukiOS SHELL C:\\APPS>', timeout=90)
        off = vm.offset()
        vm.text('sound /d')
        for stage in ('Entered player; DOS resize', 'Probe native Sound Blaster',
                      'PCI BIOS presence', 'Check output busy', 'Allocate and read PCM',
                      'Configure codec (CAS)', 'DMA start and bounded poll'):
            vm.wait('[SOUND:D] ' + stage, off, 30)
        # The shell restores its console on child exit. Capture the actual
        # visible diagnostic during playback, before that legitimate redraw.
        vm.hmp('stop')
        vm.hmp(f'pmemsave 0xb8000 4000 "{out}/diagnostic.vram"')
        visible = (out / 'diagnostic.vram').read_bytes()[::2].decode('cp437')
        assert '[SOUND:D] DMA start' in visible, 'diagnostic not visible on real VGA text screen'
        Image.open(vm.shot('diagnostic')).save(out / 'diagnostic.png')
        vm.hmp('cont')
        vm.wait('[SOUND:D] DMA stopped; cleanup', off, 30)
        vm.wait('[SOUND:D] Restore PCI command', off, 30)
        vm.wait('startup melody played through AC97.', off, 30)
        vm.wait('CiukiOS SHELL C:\\APPS>', off, 30)
        assert b'Explicit EC access' not in vm.serial.read_bytes()[off:]
        vm.command('comdemo', 'COM demo via INT21h')
        report['qemu_alive_after_dos_child'] = vm.process.poll() is None
        assert report['qemu_alive_after_dos_child']
    finally:
        vm.close()
    pcm, bits = pcm_payload((out / 'audio.wav').read_bytes())
    samples = np.frombuffer(pcm, dtype='<i2').astype(float)
    report['audio'] = {'bits': bits, 'pcm_bytes': len(pcm), 'rms': float(np.std(samples))}
    assert bits == 16 and len(pcm) > 200000 and report['audio']['rms'] > 100
    assert hashlib.sha256(args.image.read_bytes()).hexdigest() == original_sha
    report['passed'] = True
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2), flush=True)


if __name__ == '__main__':
    main()
