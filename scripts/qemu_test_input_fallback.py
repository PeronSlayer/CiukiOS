#!/usr/bin/env python3
"""Controlled native-init-disabled boot using stock CuteMouse and real PS/2 I/O.

The only guest override is a kernel built with ENABLE_PS2_MOUSE_INIT=0.
This verifies the BIOS fallback path under QEMU; it is not the shipping default
and does not establish E500/T23 firmware compatibility.
"""
import argparse, hashlib, json, shutil, subprocess, time
from pathlib import Path
from PIL import Image
from qemu_test_installed_hdd import FAT16
from qemu_test_native_windows import WindowVM

def sha(data):return hashlib.sha256(data).hexdigest()

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('image','kernel','output'):p.add_argument('--'+n,type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    disk=a.output/'controlled-native-disabled.img';shutil.copyfile(a.image,disk)
    stock=FAT16(a.image)
    names=('SYSTEM/SHELL.COM','DRIVERS/INPUTINI.COM','DRIVERS/MOUSE/CTMOUSE.EXE')
    report={'fixture':'ENABLE_PS2_MOUSE_INIT=0; not shipping default or physical hardware',
            'stock_image_sha256':sha(a.image.read_bytes()),'kernel_sha256':sha(a.kernel.read_bytes()),
            'stock_payload_sha256':{n:sha(stock.read(n)) for n in names},'memory_mib':128}
    subprocess.run(['mcopy','-o','-i',str(disk),str(a.kernel),'::SYSTEM/CIUKIDOS.SYS'],check=True)
    modified=FAT16(disk)
    assert all(sha(modified.read(n))==report['stock_payload_sha256'][n] for n in names)
    vm=WindowVM(disk,a.output,'std',memory=128,palette='platinum')
    report['qemu_command']=vm.process.args
    try:
        vm.wait('[INPUT] Trying BIOS PS/2 mouse driver.',timeout=90)
        vm.wait('[INPUT] BIOS PS/2 mouse ready.',timeout=60)
        vm.ready();vm.shot('fallback-desktop')
        report['mode']=list(Image.open(a.output/'fallback-desktop.ppm').size)
        before=vm.pointer();vm.move(26,-17);after=vm.pointer()
        assert before!=after,'actual PS/2 movement did not move the rendered cursor'
        report['pointer_before_after']=[before,after]
        # WindowVM's native-driver harness assumes two pixels per packet.
        # CuteMouse has its own sensitivity/acceleration. Calibrate against
        # rendered movement rather than modifying guest driver settings.
        gain=[abs((after[0]-before[0])/26),abs((after[1]-before[1])/-17)]
        report['measured_pixels_per_packet']=gain.copy()
        report['pointer_positioning']=[]
        def position(x,y):
            for _ in range(50):
                old=vm.pointer();delta=[x-old[0],y-old[1]]
                if all(abs(d)<=3 for d in delta):return
                packet=[max(-80,min(80,round(delta[i]/max(gain[i],1)))) for i in range(2)]
                for i in range(2):
                    if abs(delta[i])>3 and not packet[i]:packet[i]=1 if delta[i]>0 else -1
                vm.move(*packet);new=vm.pointer()
                report['pointer_positioning'].append({'from':old,'packet':packet,'to':new})
                for i in range(2):
                    if packet[i] and new[i]!=old[i]:gain[i]=abs((new[i]-old[i])/packet[i])
            raise AssertionError(f'calibrated pointer did not reach {(x,y)}: {vm.pointer()}')
        vm.position=position
        original=vm.active_rect()
        vm.repaint_key('f3');opened=vm.active_rect();vm.shot('keyboard-opened-run')
        assert opened!=original,'F3 did not open Run'
        x,y,w=opened
        vm.click_at(x+w-16,y+15)
        assert vm.active_rect()==original,'real mouse click did not close Run'
        vm.shot('mouse-closed-run')
        offset=vm.offset();vm.key('f4');vm.wait('[DESKTOP] DOS',offset,30)
        offset=vm.offset();vm.text('echo FALLBACK KEYBOARD OK')
        vm.wait('\r\nFALLBACK KEYBOARD OK\r\nCiukiOS SHELL',offset,30)
        offset=vm.offset();vm.text('mouse info')
        vm.wait('ver=0x0705',offset,30)
        vm.wait('type=0x0004',offset,30)
        vm.shot('cutemouse-version-and-keyboard')
        offset=vm.offset();vm.text('exit');vm.ready(offset)
        before=vm.pointer();vm.move(-19,13);after=vm.pointer()
        assert before!=after,'resident fallback mouse failed after DOS roundtrip'
        vm.shot('fallback-after-dos')
        report['checks']=['stock helper/driver retained','fallback reported ready','rendered cursor moved',
                          'keyboard opened Run','mouse closed Run','DOS echo executed',
                          'actual CuteMouse 7.05 PS/2 INT33 interface','mouse survived DOS roundtrip']
        report['passed']=True
    except Exception as exc:
        report['passed']=False;report['error']=repr(exc)
        try:vm.shot('failure')
        except Exception:pass
        raise
    finally:
        vm.close()
        (a.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
        assert sha(a.image.read_bytes())==report['stock_image_sha256'],'source image changed'
    print('PASS controlled BIOS CuteMouse fallback and real GUI/DOS input')

if __name__=='__main__':main()
