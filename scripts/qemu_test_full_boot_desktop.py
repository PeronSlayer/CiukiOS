#!/usr/bin/env python3
"""Observe real boot progress, startup sound, keyboard and muted reboot."""
import argparse
from pathlib import Path
import shutil
import subprocess
import time
from qemu_test_full_display_profile import VM

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
ap.add_argument('--iso', type=Path)
ap.add_argument('--output', type=Path, required=True)
args = ap.parse_args()
out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
disk = out / 'test.img'
if args.iso:
    disk.write_bytes(bytes(512))
else:
    shutil.copyfile(args.image, disk)
extra = ['-audiodev', f'wav,id=snd,path={out}/startup.wav', '-device', 'AC97,audiodev=snd']
if args.iso:
    extra += ['-boot', 'd', '-cdrom', str(args.iso.resolve())]
v = VM(disk, out, extra)
try:
    states = set()
    deadline = time.monotonic() + 45
    i = 0
    while time.monotonic() < deadline:
        shot = v.shot(f'boot-{i:03d}')
        data = shot.read_bytes().split(maxsplit=4)
        if data[1:3] == [b'800', b'600']:
            pixels = data[4]
            count = 0
            for y in range(584, 592):
                for x in range(80, 720):
                    r, g, b = pixels[(y*800+x)*3:(y*800+x)*3+3]
                    count += int(70 < r < 150 and g > 150 and b > 180)
            states.add(count)
        if v.serial.exists() and b'SSHHEELLLL' in v.serial.read_bytes():
            break
        i += 1
        time.sleep(.04)
    assert len(states) >= 2 and max(states) > 1000, f'progress states: {states}'
    print(f'[boot-desktop] PASS animated progress: {sorted(states)}', flush=True)
    v.wait('CiukiOS SHELL', timeout=45)
    offset = v.offset(); v.text('echo BOOT KEYBOARD READY')
    v.wait('BOOT KEYBOARD READY\r\nCiukiOS SHELL', offset)
    v.shot('shell-ready')
    print('[boot-desktop] PASS keyboard after boot', flush=True)
    # HDD preferences survive reboot; a MEMDISK CD is recreated from the ISO.
    if not args.iso:
        offset=v.offset();v.text('sound off');v.wait('preference saved',offset)
        v.close()
        muted = out / 'muted'
        muted.mkdir(exist_ok=True)
        v = VM(disk, muted, ('-audiodev',f'wav,id=snd,path={muted}/startup.wav',
                            '-device','AC97,audiodev=snd'))
        v.wait('CiukiOS SHELL',timeout=45)
        time.sleep(1.5)
        print('[boot-desktop] PASS muted preference survives HDD restart',flush=True)
finally:
    v.close()
subprocess.run(['python3','scripts/analyze_audio_wav.py',str(out/'startup.wav'),
                '--label','BOOT-CHIME','--min-bytes','100000'],check=True)
if not args.iso:
    import wave, array
    muted_wav = out/'muted/startup.wav'
    raw = muted_wav.read_bytes()
    # QEMU leaves an empty, unfinalized 44-byte RIFF header when the audio
    # device never starts. Otherwise inspect every captured PCM sample.
    if len(raw) == 44 and raw[:8] == b'RIFF\0\0\0\0' and raw[36:] == b'data\0\0\0\0':
        samples = ()
    else:
        with wave.open(str(muted_wav),'rb') as wav:
            samples = array.array('h',wav.readframes(wav.getnframes()))
    assert not samples or max(map(abs,samples)) == 0, 'muted restart emitted PCM audio'
    print('[boot-desktop] PASS muted restart is silent',flush=True)
