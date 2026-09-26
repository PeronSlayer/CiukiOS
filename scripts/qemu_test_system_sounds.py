#!/usr/bin/env python3
"""Native event sounds via actual GUI input and captured QEMU PCM, on a copy."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time

import numpy as np

from analyze_audio_wav import pcm_payload
from qemu_test_full_display_profile import VM
from qemu_test_native_windows import WindowVM
from qemu_test_native_utilities import Utilities
from qemu_test_installed_hdd import FAT16


class SoundVM(WindowVM):
    def __init__(self, disk, output, backend):
        self.palette = 'platinum'
        self.cursor_colors = ((36,40,48),(246,246,242))
        self.control_latencies = []
        devices = {'ac97':['-device','AC97,audiodev=snd'],
                   'sb16':['-device','sb16,iobase=0x220,irq=7,dma=1,dma16=5,audiodev=snd'],
                   'none':[]}[backend]
        VM.__init__(self,disk,output,memory=128,qemu_args=['-vga','std',
            '-audiodev',f'wav,id=snd,path={output}/audio.wav',*devices])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--image',type=Path,required=True)
    ap.add_argument('--shell',type=Path,required=True)
    ap.add_argument('--listing',type=Path,required=True)
    ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--backend',choices=['ac97','sb16','none'],default='ac97')
    ap.add_argument('--missing-file',action='store_true')
    args = ap.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True,exist_ok=True)
    disk = out/'test.img'; shutil.copyfile(args.image,disk)
    volume = f'{disk}@@{FAT16(disk).start}'
    muted = out/'BOOT.SND'; muted.write_bytes(b'0')
    subprocess.run(['mcopy','-o','-i',volume,str(muted),'::SYSTEM/BOOT.SND'],check=True)
    profile=out/'DISPLAY.CFG'; profile.write_bytes(b'0800')
    subprocess.run(['mcopy','-o','-i',volume,str(profile),'::SYSTEM/VIDEO/DISPLAY.CFG'],check=True)
    if args.missing_file:
        extension = 'SB' if args.backend == 'sb16' else 'PCM'
        subprocess.run(['mdel','-i',volume,'::SYSTEM/SOUNDS/INFO.'+extension],check=True)
    vm = SoundVM(disk,out,args.backend)
    results = {'backend':args.backend,'missing_file':args.missing_file,'input_image_sha256':hashlib.sha256(args.image.read_bytes()).hexdigest()}
    try:
        vm.ready(); ui = Utilities(vm,args.shell.read_bytes(),args.listing)
        mark = vm.offset()
        ui.click(13); ui.click(9)
        ui.until(lambda:ui.b('ui_active_window')==6,'Sound window')
        ui.click(56,6)
        ui.until(lambda:ui.b('ui_sound_preference_value')==ord('1'),'enabled')
        time.sleep(.6)
        # QEMU's SB voice creates the WAV only when its first DMA starts.
        start = (out/'audio.wav').stat().st_size if (out/'audio.wav').exists() else 0
        before = time.monotonic()
        ui.click(55,6)
        latency = time.monotonic()-before
        expected = 3 if args.backend=='none' else 4 if args.missing_file else 1
        ui.until(lambda:ui.w('ui_sfx_status')==expected,f'play result {expected}')
        time.sleep(.7)
        end = (out/'audio.wav').stat().st_size if (out/'audio.wav').exists() else 0
        ui.shot('native-sound-result')
        ui.click(57,6)
        ui.until(lambda:ui.b('ui_sound_preference_value')==ord('0'),'disabled')
        time.sleep(.4)
        mute_start = (out/'audio.wav').stat().st_size if (out/'audio.wav').exists() else 0
        ui.click(55,6); time.sleep(.6)
        ui.shot('native-sound-muted')
        mute_end = (out/'audio.wav').stat().st_size if (out/'audio.wav').exists() else 0
        assert ui.b('ui_active_window')==6
        serial = subprocess.check_output(['scripts/serial_log_normalize.py','--offset',str(mark),str(vm.serial)])
        assert b'[DESKTOP] DOS' not in serial and b'CiukiOS SHELL' not in serial, 'Sound escaped to DOS'
        results.update(result='PASS',status=expected,harness_control_seconds_including_settling=latency,play_range=[start,end],muted_range=[mute_start,mute_end],no_DOS_transition=True)
    except BaseException:
        vm.shot('failure')
        vm.hmp(f'pmemsave 0 1048576 "{out}/failure-memory.bin"')
        (out/'failure-pic.txt').write_bytes(vm.hmp('info pic'))
        raise
    finally:
        vm.close()
    raw = (out/'audio.wav').read_bytes() if (out/'audio.wav').exists() else b''
    if args.backend!='none' and (raw or not args.missing_file):
        payload,bits = pcm_payload(raw); assert bits==16
        # Derive the PCM chunk offset; never interpret a RIFF header as audio
        # when a first-use backend had no output file at the starting sample.
        at=12
        while raw[at:at+4] != b'data':
            length=int.from_bytes(raw[at+4:at+8],'little')
            at += 8 + length + (length & 1)
            assert at+8 <= len(raw), 'WAV data chunk missing'
        pcm_start=at+8
        def samples(a,b):
            a=max(a,pcm_start);b=max(a,b)
            return np.frombuffer(raw[a:b-(b-a)%2],dtype='<i2').astype(np.float64)
        played = samples(start,end)
        rms = float(np.sqrt(np.mean(played**2))) if len(played) else 0
        if args.missing_file: assert not len(played) or np.max(np.abs(played))==0, 'missing file played stale data'
        else: assert rms>100 and len(np.unique(played))>64, ('silent event sound',rms,len(played))
        silent=samples(mute_start,mute_end)
        assert not len(silent) or np.max(np.abs(silent))==0,'muted event produced audio'
        results['event_pcm_rms']=rms
    assert FAT16(disk).read('SYSTEM/BOOT.SND')==b'0','mute preference not saved'
    (out/'report.json').write_text(json.dumps(results,indent=2)+'\n')
    print(json.dumps(results,indent=2))


if __name__=='__main__':main()
