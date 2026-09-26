#!/usr/bin/env python3
"""Capture boot/manual PCM or visible failure using the installed public shell."""
import argparse
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
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--image', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--audio', choices=('ac97','sb16','none'), required=True)
    ap.add_argument('--missing-pcm', action='store_true')
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    disk = out/'target.img'
    shutil.copyfile(args.image,disk)
    if args.missing_pcm:
        assert args.audio == 'ac97'
        subprocess.run(['mdel','-i',f'{disk}@@{FAT16(disk).start}','::SYSTEM/BOOT.PCM'],check=True)
    devices = {'ac97':['-device','AC97,audiodev=snd'], 'sb16':[
        '-device','sb16,iobase=0x220,irq=7,dma=1,dma16=5,audiodev=snd',
        '-device','adlib,audiodev=snd'], 'none':[]}[args.audio]
    failed = args.audio == 'none' or args.missing_pcm
    vm = VM(disk,out,('-audiodev',f'wav,id=snd,path={out}/audio.wav',
                     '-machine','pcspk-audiodev=snd',*devices))
    report = {'backend':args.audio,'missing_pcm':args.missing_pcm}
    try:
        vm.wait('CiukiOS SHELL C:\\APPS>',timeout=90)
        boot_end = (out/'audio.wav').stat().st_size
        off = vm.offset()
        vm.text('sound test')
        if failed:
            message = ('Could not read SYSTEM\\BOOT.PCM completely.' if args.missing_pcm
                       else 'No supported PCM device was detected.')
            vm.wait(message,off,20)
            vm.wait('Press any key to return',off,10)
            vm.hmp(f'pmemsave 0xb8000 4000 "{out}/failure.vram"')
            text = (out/'failure.vram').read_bytes()[::2].decode('cp437')
            assert message in text, 'failure diagnostic is not visible on screen'
            Image.open(vm.shot('visible-error')).save(out/'visible-error.png')
            vm.key('esc')
        else:
            vm.wait('startup melody played through',off,30)
        vm.wait('CiukiOS SHELL C:\\APPS>',off,30)
        vm.command('comdemo','COM demo via INT21h')
        report['diagnostic_visible'] = failed
        Image.open(vm.shot('return')).save(out/'return.png')
    finally:
        vm.close()
    raw = (out/'audio.wav').read_bytes()
    if failed:
        pcm,_ = pcm_payload(raw)
        assert not any(pcm), 'failed PCM path emitted audio/beeps'
        report['silent_failure'] = True
    else:
        for name,start,end in [('boot',44,boot_end),('manual',boot_end,len(raw))]:
            data = raw[start:end]
            data = data[:len(data)//2*2]
            samples = np.frombuffer(data,dtype='<i2').astype(float)
            assert len(samples)>30000, (name,'missing recording')
            rms = float(np.std(samples))
            assert rms>100, (name,'silent PCM',rms)
            report[name] = {'rms':rms,'samples':len(samples)}
    (out/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print(report,flush=True)


if __name__ == '__main__':
    main()
