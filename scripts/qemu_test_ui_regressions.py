#!/usr/bin/env python3
"""Targeted real-screen/input regressions for native UI repaint and focus."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import time

import numpy as np
from PIL import Image

from qemu_test_installed_hdd import FAT16, listing_address
from qemu_test_native_windows import WindowVM, pixels
from qemu_test_full_display_profile import VM


def measured_click(vm,name,x,y):
    vm.position(x,y)
    offset=vm.offset();vm.hmp('mouse_button 1')
    vm.wait('[DESKTOP] PAINT',offset,10)
    pointer=vm.pointer();before=pixels(vm,f'{name}-before-release')
    offset=vm.offset();start=time.monotonic();vm.hmp('mouse_button 0')
    frames=[];times=[]
    while time.monotonic()-start<15:
        if len(frames)<64:
            frames.append(VM.shot(vm,f'{name}-frame-{len(frames):03d}'))
            times.append(time.monotonic()-start)
        if b'[DESKTOP] PAINT' in vm.serial.read_bytes()[offset:]:break
        time.sleep(.025)
    else:raise AssertionError(f'{name}: paint did not finish within 15 seconds')
    completed=time.monotonic()-start
    after=pixels(vm,f'{name}-completed')
    height,width=before.shape[:2];stable=np.ones((height,width),bool)
    px,py=pointer;stable[py:py+16,px:px+24]=False
    stable[height-32:,width-78:]=False
    before_stable=before[stable];after_stable=after[stable]
    states=[]
    for path in frames:
        with Image.open(path) as im:
            sample=np.array(im);im.save(path.with_suffix('.png'))
        sample_stable=sample[stable]
        old_match=np.array_equal(sample_stable,before_stable)
        new_match=np.array_equal(sample_stable,after_stable)
        states.append('old' if old_match else 'new' if new_match else 'mixed')
    return {'release_to_completed_paint_seconds':completed,
            'sample_elapsed_seconds':times,'frames':len(frames),
            'sampled_presentation_states':states,'mixed_frames':states.count('mixed')}


def cpu_seconds(process):
    raw=Path(f'/proc/{process.pid}/stat').read_text()
    fields=raw[raw.rfind(')')+2:].split()
    return (int(fields[11])+int(fields[12]))/os.sysconf('SC_CLK_TCK')


def ui_hit_binding(lines, base, read_word):
    """Locate inline legacy hits or the current DOS-owned asset allocation.

    Addresses come from the matching NASM listing and observed segment value;
    this performs no guest writes and never assumes a fixed heap address.
    """
    def equ(name, depth=0):
        assert depth < 8, 'cyclic listing equate'
        for line in lines:
            m=re.search(r'\b'+re.escape(name)+r'\s+equ\s+([A-Za-z_][\w]*|0[xX][0-9a-fA-F]+|[0-9]+)\s*(?:;.*)?$',line)
            if m:
                value=m[1]
                return int(value,0) if value[0].isdigit() else equ(value,depth+1)
        raise AssertionError(f'listing has no simple equate {name}')
    if any(re.search(r'\bui_hits\s+equ\b',line) for line in lines):
        offset=equ('ui_hits');segment=read_word('ui_assets_seg')
        assert segment and 0 <= offset <= 65536-1920, 'invalid hit allocation'
        address=segment*16+offset
        assert address+1920 <= 0xA0000, 'hits outside conventional memory'
        return {'kind':'asset-heap','segment':segment,'offset':offset,'address':address}
    for line in lines:
        if re.search(r'\bui_hits\s+(?:times|db|dw|dd)',line):
            m=re.match(r'\s*\d+\s+([0-9A-F]{8})\s',line)
            if m:return {'kind':'inline','offset':int(m[1],16),'address':base+int(m[1],16)}
    raise AssertionError('listing has no hit storage declaration')


def focus_diagnostic(vm,output,name,source,listing):
    if listing is None:return {}
    path=output/f'{name}-ram.bin';vm.hmp(f'pmemsave 0 1048576 "{path}"')
    ram=path.read_bytes();needle=source.read('SYSTEM/SHELL.COM')[:64]
    base=ram.find(needle);assert base>=0 and ram.count(needle)==1
    lines=listing.read_text().splitlines()
    def word(label):
        off=base+listing_address(lines,label);return int.from_bytes(ram[off:off+2],'little')
    active_off=base+listing_address(lines,'ui_active_window');active=ram[active_off]
    count=word('ui_hit_count')
    binding=ui_hit_binding(lines,base,word)
    assert 0 <= count <= 192, f'invalid hit count {count}'
    hits=[]
    for index in range(count):
        pos=binding['address']+index*10
        values=[int.from_bytes(ram[pos+n:pos+n+2],'little') for n in range(0,10,2)]
        hits.append({'rect':values[:4],'action':values[4]&255,'owner':values[4]>>8})
    return {'focus':word('ui_focus'),'active_window':active,'hit_records':hits,'hit_binding':binding}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disk',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--case',choices=['selection','focus','release','idle','maximize','keyboard-minimize'],required=True)
    parser.add_argument('--listing',type=Path)
    parser.add_argument('--profile',default='0800')
    parser.add_argument('--video')
    parser.add_argument('--memory',type=int,default=128)
    parser.add_argument('--require-atomic',action='store_true')
    parser.add_argument('--palette',choices=['legacy','platinum'],default='platinum')
    args=parser.parse_args()
    output=args.output.resolve();output.mkdir(parents=True,exist_ok=True)
    disk=output/'target.img';shutil.copyfile(args.disk,disk)
    profile=output/'DISPLAY.CFG';profile.write_bytes(args.profile.encode())
    subprocess.run(['mcopy','-o','-i',f'{disk}@@{FAT16(disk).start}',str(profile),
                    '::SYSTEM/VIDEO/DISPLAY.CFG'],check=True)
    source=FAT16(disk)
    report={'case':args.case,'memory_mib':args.memory,
            'harness_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            'shell_sha256':hashlib.sha256(source.read('SYSTEM/SHELL.COM')).hexdigest()}
    vm=WindowVM(disk,output,'std',memory=args.memory,video=args.video,palette=args.palette)
    report['qemu_command']=vm.process.args
    try:
        vm.ready();screen=pixels(vm,'desktop');height,width=screen.shape[:2]
        report['mode']=[width,height]
        if args.video:
            assert (width,height)==tuple(map(int,args.video.split('x')))
        if args.case=='selection':
            x,y,w=vm.active_rect()
            tile_x=x+(w-444+(146 if w>=680 else 0))//2;tile_y=y+105
            vm.click_at(tile_x+69,tile_y+35);vm.position(width-35,height-55)
            first=pixels(vm,'first-selected')
            vm.click_at(tile_x+148+69,tile_y+35);vm.position(width-35,height-55)
            second=pixels(vm,'second-selected')
            old_band=np.any(first[tile_y:tile_y+99,tile_x:tile_x+3]!=
                            second[tile_y:tile_y+99,tile_x:tile_x+3],axis=2)
            report['old_selection_band_changed_pixels']=int(old_band.sum())
            assert old_band.sum()==297,'old selection highlight was not completely removed'
            vm.click_at(tile_x+69,tile_y+35);vm.position(width-35,height-55)
            restored=pixels(vm,'selection-restored')
            region=(slice(tile_y,tile_y+209),slice(tile_x,tile_x+433))
            changed=int(np.any(first[region]!=restored[region],axis=2).sum())
            report['roundtrip_grid_changed_pixels']=changed
            assert changed==0,'selection round trip left stale pixels in the item grid'
        elif args.case=='focus':
            vm.repaint_key('f3');run=vm.active_rect()
            for ch in 'echo FOCUSRETURN':
                vm.repaint_key('spc' if ch==' ' else 'shift-'+ch.lower() if ch.isupper() else ch)
            vm.repaint_key('f1');assert vm.active_rect()!=run
            vm.repaint_key('alt-tab');assert vm.active_rect()==run
            vm.shot('run-refocused');offset=vm.offset();vm.key('ret')
            vm.wait('FOCUSRETURN\r\n',offset,12)
            vm.wait('Press any key to return to the desktop.',offset,12)
            vm.shot('command-executed');offset=vm.offset();vm.key('spc');vm.ready(offset)
            vm.pointer();report['command_executed_after_refocus']=True
        elif args.case=='release':
            vm.repaint_key('f3');rx,ry,rw=vm.active_rect()
            vm.repaint_key('f1');ax,ay,aw=vm.active_rect()
            # Make both close controls separately exposed, using a real drag.
            vm.position(ax+170,ay+12);vm.hmp('mouse_button 1');time.sleep(.15)
            vm.move(20,20);vm.hmp('mouse_button 0');time.sleep(.4)
            ax,ay,aw=vm.active_rect()
            assert ax-rx>=25 and ay-ry>=25,'could not expose both close controls'
            # Press the exposed Run close button. Activation brings Run in front.
            vm.position(rx+448,ry+10);vm.hmp('mouse_button 1');time.sleep(.2)
            assert vm.active_rect()==(rx,ry,rw),'press did not activate Run'
            vm.shot('close-pressed-on-run')
            # About's close button remains exposed to Run's right. Both controls
            # use the same action ID but belong to different windows.
            vm.position(ax+448,ay+10);vm.hmp('mouse_button 0');time.sleep(.4)
            vm.shot('released-on-about')
            assert vm.active_rect()==(rx,ry,rw),'release on another window activated the pressed action'
            vm.repaint_key('alt-tab')
            assert vm.active_rect()==(ax,ay,aw),'the other window unexpectedly closed'
            report['both_windows_retained_after_cross_owner_release']=True
        elif args.case=='keyboard-minimize':
            vm.repaint_key('f3');run=vm.active_rect();rx,ry,rw=run
            for ch in 'echo retained':vm.repaint_key('spc' if ch==' ' else ch)
            vm.repaint_key('f1');vm.repaint_key('alt-f4')
            assert vm.active_rect()==run
            vm.click_at(rx+424,ry+14);vm.click_at(280,height-19)
            assert vm.active_rect()==run
            report['before_tab']=focus_diagnostic(vm,output,'before-tab',source,args.listing)
            vm.shot('before-tab');vm.repaint_key('tab')
            report['after_tab']=focus_diagnostic(vm,output,'after-tab',source,args.listing)
            vm.shot('after-tab');vm.repaint_key('tab')
            report['after_second_tab']=focus_diagnostic(vm,output,'after-second-tab',source,args.listing)
            vm.shot('after-second-tab');vm.repaint_key('ret')
            vm.shot('after-enter')
            vm.click_at(280,height-19)
            assert vm.active_rect()==run,'keyboard minimize did not retain its task button'
        elif args.case=='maximize':
            rx,ry,rw=vm.active_rect();vm.position(width-35,height-55)
            before_pointer=vm.pointer()
            before=pixels(vm,'before-maximize')
            report['maximize']=measured_click(vm,'maximize',rx+rw-40,ry+15)
            assert vm.active_rect()==(8,34,width-16),'maximize did not finish at work-area dimensions'
            report['restore']=measured_click(vm,'restore',width-48,49)
            assert vm.active_rect()==(rx,ry,rw),'restore did not recover original geometry'
            vm.position(width-35,height-55);time.sleep(.4)
            after_pointer=vm.pointer()
            after=pixels(vm,'restored')
            stable=np.ones((height,width),bool);stable[height-32:,width-78:]=False
            for px,py in (before_pointer,after_pointer):stable[py:py+16,px:px+24]=False
            report['excluded_pointer_footprints']=[before_pointer,after_pointer]
            changes=int(np.any(after!=before,axis=2)[stable].sum())
            report['settled_roundtrip_changed_pixels']=changes
            assert changes==0,'completed maximize/restore left visible pixel changes'
            if args.require_atomic:
                assert report['maximize']['mixed_frames']==0,'maximize exposed a partially drawn frame'
                assert report['restore']['mixed_frames']==0,'restore exposed a partially drawn frame'
        else:
            vm.position(width-35,height-55);time.sleep(1)
            before=pixels(vm,'idle-before');offset=vm.offset()
            start=time.monotonic();cpu_start=cpu_seconds(vm.process);samples=[]
            for index in range(4):
                time.sleep(.5);samples.append(pixels(vm,f'idle-{index:02d}'))
            elapsed=time.monotonic()-start;used=cpu_seconds(vm.process)-cpu_start
            stable=np.ones((height,width),bool);stable[height-32:,width-78:]=False
            changes=[int(np.any(frame!=before,axis=2)[stable].sum()) for frame in samples]
            serial=subprocess.check_output(['scripts/serial_log_normalize.py','--offset',str(offset),str(vm.serial)])
            report['idle']={'sample_seconds':elapsed,'qemu_host_cpu_seconds':used,
                            'qemu_host_cpu_fraction':used/elapsed,
                            'sampled_outside_clock_changed_pixels':changes,
                            'paint_markers':serial.count(b'[DESKTOP] PAINT'),
                            'interpretation':'Host idle observation, not physical Pentium III speed.'}
            assert not any(changes),'settled desktop changed outside its clock without input'
            assert report['idle']['paint_markers']<=2,'idle desktop repeatedly repainted'
        report['status']='passed'
        (output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps(report),flush=True)
    except Exception as exc:
        report['status']='failed';report['error']=str(exc)
        (output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
        vm.shot('failure');(output/'registers.log').write_bytes(vm.hmp('info registers'))
        raise
    finally:
        vm.close()


if __name__=='__main__':
    main()
