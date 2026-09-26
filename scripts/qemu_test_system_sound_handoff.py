#!/usr/bin/env python3
"""Verify native SB sound leaves IRQ vectors/masks and subsequent DOS usable."""
import argparse
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import time

from qemu_test_system_sounds import SoundVM
from qemu_test_native_utilities import Utilities
from qemu_test_installed_hdd import FAT16


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image',type=Path,required=True)
    p.add_argument('--shell',type=Path,required=True)
    p.add_argument('--listing',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    disk=out/'test.img';shutil.copyfile(a.image,disk)
    cfg=out/'BOOT.SND';cfg.write_bytes(b'0')
    subprocess.run(['mcopy','-o','-i',str(disk),str(cfg),'::SYSTEM/BOOT.SND'],check=True)
    display=out/'DISPLAY.CFG';display.write_bytes(b'0800')
    subprocess.run(['mcopy','-o','-i',str(disk),str(display),'::SYSTEM/VIDEO/DISPLAY.CFG'],check=True)
    vm=SoundVM(disk,out,'sb16');r={'passed':False,'guest_memory_writes':False}
    def state(name):
        pic=vm.hmp('info pic');(out/(name+'-pic.txt')).write_bytes(pic)
        regs={int(i):dict(irr=int(irr,16),imr=int(imr,16),isr=int(isr,16))
              for i,irr,imr,isr in re.findall(rb'pic([01]): irr=([0-9a-f]+) imr=([0-9a-f]+) isr=([0-9a-f]+)',pic)}
        path=out/(name+'-ivt.bin');vm.hmp(f'pmemsave 0 1024 "{path}"')
        return regs,path.read_bytes()
    try:
        vm.ready();ui=Utilities(vm,a.shell.read_bytes(),a.listing)
        ui.click(13);ui.click(9)
        ui.until(lambda:ui.b('ui_active_window')==6,'Sound')
        ui.click(56,6);time.sleep(.8)
        before,ivt=state('before')
        assert len(before)==2, before
        ui.click(55,6)
        ui.until(lambda:ui.w('ui_sfx_status')==1,'SB playback started')
        time.sleep(.8)
        after,ivt_after=state('after')
        assert ivt==ivt_after,'native sound changed interrupt vectors'
        assert {i:v['imr'] for i,v in before.items()}=={i:v['imr'] for i,v in after.items()},(before,after)
        assert not ((after[0]['irr']|after[0]['isr']) & 0x80),after
        ui.click(57,6)
        ui.until(lambda:ui.b('ui_sound_preference_value')==ord('0'),'mute')
        mark=vm.offset();vm.key('f4');vm.wait('[DESKTOP] DOS',mark,20)
        vm.wait('CiukiOS SHELL',mark,20)
        mark=vm.offset();vm.text('comdemo');vm.wait('COM demo via INT21h',mark,20)
        vm.wait('CiukiOS SHELL',mark,20)
        ui.refresh();assert ui.w('ui_sfx_entry',1)==0,'audio module remains owned in DOS'
        mark=vm.offset();vm.text('exit');vm.ready(mark)
        vm.key('f3');ui.until(lambda:ui.b('ui_active_window')==1,'Run after DOS')
        ui.click(18,1);ui.shot('desktop-after-native-sound-and-DOS')
        r.update(passed=True,pic_before=before,pic_after=after,ivt_bytes_unchanged=1024,
                 original_masks_restored=True,no_irq7_pending=True,COMDEMO_completed=True,
                 audio_module_released_before_DOS=True,native_input_after_DOS=True)
    except Exception as e:
        r['error']=repr(e);raise
    finally:
        vm.close();(out/'report.json').write_text(json.dumps(r,indent=2)+'\n')


if __name__=='__main__':main()
