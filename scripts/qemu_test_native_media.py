#!/usr/bin/env python3
"""Native removable media through rendered controls and real QEMU PS/2 input.

Guest RAM is observed only. Source fixtures are attached read-only; imports
are checked independently after normal QEMU shutdown. No physical-device claim.
"""
import argparse,hashlib,json,shutil,subprocess,time
from pathlib import Path
from qemu_test_native_windows import WindowVM
from qemu_test_full_display_profile import VM
from qemu_test_native_utilities import Utilities
from qemu_test_installed_hdd import FAT16

class MediaVM(WindowVM):
    def __init__(self,disk,output,fixtures):
        output.mkdir(parents=True,exist_ok=True)
        self.palette='platinum'; self.control_latencies=[]
        self.cursor_colors=((36,40,48),(246,246,242))
        VM.__init__(self,disk,output,memory=128,qemu_args=[
            '-vga','std','-drive',f'file={fixtures}/fat12.img,format=raw,if=floppy,index=0,readonly=on',
            '-drive',f'file={fixtures}/data.iso,format=raw,if=ide,index=2,media=cdrom,readonly=on',
            '-usb','-drive',f'file={fixtures}/fat16.img,format=raw,if=none,id=usbmedia,readonly=on',
            '-device','usb-storage,drive=usbmedia'])

def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--image',type=Path,default=Path('build/full/native-media-2026-09-26/native-media.img'))
    ap.add_argument('--shell',type=Path,default=Path('build/full/native-media-2026-09-26/obj/shell.com'))
    ap.add_argument('--listing',type=Path,default=Path('build/full/native-media-2026-09-26/obj/shell.lst'))
    ap.add_argument('--fixtures',type=Path,default=Path('build/full/native-media-2026-09-26/driver-tests/fixtures'))
    ap.add_argument('--output',type=Path,default=Path('build/full/native-media-2026-09-26/qemu'))
    args=ap.parse_args(); args.output.mkdir(parents=True,exist_ok=True)
    disk=args.output/'test.img'
    assert not disk.exists(), 'Choose a new output directory to preserve previous evidence'
    shutil.copyfile(args.image,disk)
    display=args.output/'DISPLAY.CFG'; display.write_bytes(b'0800')
    subprocess.run(['mcopy','-o','-i',str(disk),str(display),'::SYSTEM/VIDEO/DISPLAY.CFG'],check=True)
    source_paths=[args.fixtures/name for name in ('fat12.img','fat16.img','data.iso')]
    hashes={str(p):digest(p) for p in source_paths}; checks=[]
    vm=MediaVM(disk.resolve(),args.output.resolve(),args.fixtures.resolve())
    try:
        vm.ready(); ui=Utilities(vm,args.shell.read_bytes(),args.listing)
        for kind,action,device,target in [('Floppy',14,1,'FLOPPY.BIN'),('USB BIOS disk',15,2,'USB.BIN'),('CD-ROM',16,3,'CD.BIN')]:
            if ui.b('ui_window_flags',8):
                ui.click(18,8); ui.until(lambda:ui.b('ui_window_flags',8)==0,'close Files')
            offset=vm.offset(); ui.click(action)
            ui.until(lambda:ui.b('md_active')==device and ui.b('ui_active_window')==8 and ui.b('fm_count')==2,f'{kind}: native list',60)
            assert set(ui.rows())=={'NESTED','PAYLOAD.BIN'},ui.rows()
            ui.shot(f'{device}-root')
            ui.select('NESTED'); vm.key('ret')
            ui.until(lambda:ui.z('fm_path').upper()=='/NESTED',f'{kind}: enter nested')
            ui.select('DEEP'); vm.key('ret')
            ui.until(lambda:ui.z('fm_path').upper()=='/NESTED/DEEP',f'{kind}: deep path')
            ui.select('README.TXT'); vm.key('ret')
            ui.until(lambda:ui.b('md_preview')==1,f'{kind}: native text preview')
            assert '511 bytes' in ui.status(),ui.status()
            ui.shot(f'{device}-text-preview')
            ui.click(102,8)
            ui.until(lambda:ui.b('md_preview')==0 and ui.rows()==['README.TXT'],f'{kind}: return from preview')
            ui.click(101,8); ui.until(lambda:ui.z('fm_path').upper()=='/NESTED',f'{kind}: Up one')
            ui.click(101,8); ui.until(lambda:ui.z('fm_path')=='/',f'{kind}: Up root')
            ui.select('PAYLOAD.BIN'); ui.prompt(105)
            ui.replace_text('C:\\'+target)
            ui.until(lambda:ui.b('fm_prompt')==0 and 'File imported' in ui.status(),f'{kind}: import bytes',60)
            assert set(ui.rows())=={'NESTED','PAYLOAD.BIN'},ui.rows()
            ui.shot(f'{device}-imported')
            ui.select('PAYLOAD.BIN'); ui.prompt(105); ui.replace_text('C:\\'+target)
            ui.until(lambda:'already exists' in ui.status(),f'{kind}: overwrite rejected')
            ui.shot(f'{device}-existing-protected')
            ui.click(102,8); ui.until(lambda:ui.b('fm_count')==2,f'{kind}: refresh after rejected copy')
            ui.select('PAYLOAD.BIN'); vm.key('ret')
            ui.until(lambda:'binary file' in ui.status(),f'{kind}: binary preview explains Import')
            ui.click(102,8); ui.until(lambda:ui.b('fm_count')==2,f'{kind}: refresh after binary rejection')
            ui.click(107,8); ui.until(lambda:'Read-only' in ui.status(),f'{kind}: no source deletion')
            serial=subprocess.check_output(['scripts/serial_log_normalize.py','--offset',str(offset),str(vm.serial)])
            assert b'CiukiOS SHELL' not in serial and b'Media />' not in serial,'device action escaped to DOS'
            checks.append({'device':kind,'native_browse_preview_import':'PASS','destination':target,'no_DOS_transition':True})
        ui.click(18,8); ui.until(lambda:ui.b('ui_window_flags',8)==0,'close CD Files')
        vm.hmp('eject -f floppy0'); ui.click(14)
        ui.until(lambda:ui.b('md_active')==1 and ui.b('fm_count')==0 and 'failed' in ui.status().lower(),'missing floppy native error',60)
        ui.shot('missing-floppy')
        ui.click(18,8); ui.until(lambda:ui.b('ui_window_flags',8)==0,'close unavailable device')
        ui.click(2); ui.until(lambda:ui.b('md_active')==0 and ui.z('fm_path')=='C:\\' and ui.b('fm_count')>0,'return to local Files')
        ui.shot('local-files-after-media')
        report={'result':'PASS','checks':checks,'events':ui.events,'image_input_sha256':digest(args.image),'shell_sha256':digest(args.shell),
                'hardware':'QEMU KVM, Pentium III CPU model, 128MiB; no physical T23 qualification'}
    except BaseException:
        vm.shot('failure')
        raise
    finally: vm.close()
    fs=FAT16(disk); expected=(args.fixtures/'PAYLOAD.BIN').read_bytes()
    for item in checks:
        actual=fs.read(item['destination']); assert actual==expected,item
        item['import_sha256']=hashlib.sha256(actual).hexdigest()
    assert {str(p):digest(p) for p in source_paths}==hashes,'source media changed'
    report['source_media_sha256']=hashes
    (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n'); print(json.dumps(report,indent=2))
if __name__=='__main__': main()
