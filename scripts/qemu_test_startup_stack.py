#!/usr/bin/env python3
"""Boot installed payloads with a modeled 1 KiB BIOS scratch stack.

An option ROM adds a stack footprint and delegates to the real BIOS. It does
not substitute fake DOS results or a fake application. Captures and complete
RAM are retained even when boot fails. The option ROM is test-only.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import time

import numpy as np
from PIL import Image

from analyze_audio_wav import pcm_payload
from qemu_test_full_display_profile import VM
from qemu_test_installed_hdd import FAT16


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--image', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--sound', type=Path)
    ap.add_argument('--shell', type=Path)
    ap.add_argument('--fault', choices=('disk', 'pci', 'both', 'none'), default='both')
    ap.add_argument('--timeout', type=int, default=60)
    a = ap.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    disk = out/'installed.img'
    shutil.copyfile(a.image, disk)
    source = FAT16(disk)
    for file, path in ((a.sound, 'SYSTEM/BOOTSND.COM'), (a.shell, 'SYSTEM/SHELL.COM')):
        if file:
            subprocess.run(['mcopy', '-o', '-i', f'{disk}@@{source.start}', str(file), '::'+path], check=True)
    source = FAT16(disk)
    report = {'source_sha256': hashlib.sha256(a.image.read_bytes()).hexdigest(),
              'fault': a.fault, 'payload_sha256': {
                  p: hashlib.sha256(source.read(p)).hexdigest()
                  for p in ('SYSTEM/CIUKIDOS.SYS', 'SYSTEM/SHELL.COM', 'SYSTEM/BOOTSND.COM')}}
    extra = ['-audiodev', f'wav,id=snd,path={out}/audio.wav',
             '-device', 'AC97,audiodev=snd', '-debugcon', f'file:{out}/debug.log']
    if a.fault != 'none':
        rom = out/'firmware.rom'
        subprocess.run(['nasm', '-f', 'bin', 'scripts/fixtures/firmware_stack.asm',
                        f'-DSTACK_DISK={int(a.fault in ("disk", "both"))}',
                        f'-DSTACK_PCI={int(a.fault in ("pci", "both"))}',
                        '-o', str(rom)], check=True)
        data = bytearray(rom.read_bytes())
        data[-1] = (-sum(data)) & 255
        rom.write_bytes(data)
        extra += ['-option-rom', str(rom)]
    v = VM(disk, out, extra)
    v.auto_enter_dos = False
    error = None
    try:
        v.wait('[DESKTOP] READY', timeout=a.timeout)
        report['desktop_ready'] = True
        frame = v.shot('desktop')
        Image.open(frame).save(out/'desktop.png')
        v.key('f4')
        v.wait('CiukiOS SHELL C:\\APPS>')
        v.command('echo STARTUP STACK READY', 'STARTUP STACK READY\r\nCiukiOS SHELL')
        v.command('comdemo.com', 'COM demo via INT21h')
        off = v.offset()
        v.text('exit')
        v.wait('[DESKTOP] READY', off, 60)
        report['dos_and_desktop_return'] = True
    except Exception as exc:
        error = exc
        report['error'] = str(exc)
        Image.open(v.shot('failure')).save(out/'failure.png')
    finally:
        v.hmp('stop')
        (out/'registers.log').write_bytes(v.hmp('info registers'))
        v.hmp(f'pmemsave 0 0x100000 "{out}/ram.bin"')
        ram = (out/'ram.bin').read_bytes()
        if a.fault != 'none':
            top = struct.unpack_from('<H', ram, 0x413)[0]*1024
            report['firmware_scratch_counts'] = list(struct.unpack_from('<HH', ram, top+8))
        v.close()
        pcm, bits = pcm_payload((out/'audio.wav').read_bytes())
        samples = np.frombuffer(pcm, dtype='<i2').astype(float)
        report['audio'] = {'bits': bits, 'pcm_bytes': len(pcm),
                           'rms': float(np.std(samples)) if len(samples) else 0,
                           'peak': float(np.max(np.abs(samples))) if len(samples) else 0}
        exercised = True
        if a.fault != 'none':
            disk_calls, pci_calls = report['firmware_scratch_counts']
            exercised = ((a.fault not in ('disk', 'both') or disk_calls > 100)
                         and (a.fault not in ('pci', 'both') or pci_calls > 0))
        report['firmware_exercised'] = exercised
        report['passed'] = error is None and report['audio']['rms'] > 20 and exercised
        (out/'result.json').write_text(json.dumps(report, indent=2)+'\n')
        print(json.dumps(report, indent=2), flush=True)
    if error:
        raise error
    assert report['passed'], 'startup audio missing or firmware fixture not exercised'


if __name__ == '__main__':
    main()
