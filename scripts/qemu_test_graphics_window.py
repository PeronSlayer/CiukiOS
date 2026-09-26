#!/usr/bin/env python3
"""Real DPMI source-port graphics in the native window, measured on screen.

The frame gate uses completed visible CG commits, never IRQs, polls, UI paints
or submitted buffers. This qualifies this source port under this QEMU setup;
it does not establish old-PC speed, hardware acceleration or arbitrary DOS
executable compatibility. Guest RAM is observed, never injected or paused.
"""
import argparse
import hashlib
import json
import re
import shutil
import struct
import subprocess
import time
from pathlib import Path

import numpy as np

from qemu_test_dos_window import Session
from qemu_test_installed_hdd import FAT16
from qemu_test_native_windows import WindowVM


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Graphics(Session):
    def vectors(self):
        path=self.vm.output/'graphics-ivt.bin'
        self.vm.hmp(f'pmemsave 0 1024 "{path}"')
        data=path.read_bytes()
        return {f'{number:02X}':struct.unpack_from('<I',data,number*4)[0]
                for number in (8,0x10,0x15,0x16,0x2f,0x33)}

    def unhooked(self):
        current=self.vectors()
        assert current==self.original_vectors,('graphics/input interrupt vectors not restored',current,self.original_vectors)
        self.event('graphics/input interrupt vectors restored',vectors=current)

    def launch(self,command):
        assert len(command)<=96,'Run text exceeds its public input capacity'
        self.vm.key('f3')
        self.until(lambda:self.b('ui_active_window')==1,'Run did not open')
        length=self.b('ui_run_len');self.vm.key('home')
        for _ in range(length):self.vm.key('delete')
        self.type_only(command)
        # sendkey enqueues physical PS/2 events; returning from HMP does not
        # mean the final printable character has reached the Run field yet.
        # Observe exact user-visible text before clicking its launch button.
        self.until(lambda:self.b('ui_run_len')==len(command) and
                   self.ram[self.offset('ui_run_text'):self.offset('ui_run_text')+len(command)]==command.encode('ascii'),
                   'Run did not accept the complete command',10)
        offset=self.vm.offset();self.click(58,1)
        self.until(lambda:self.w('dw_segment')!=0,'runtime allocation missing')
        self.segment=self.w('dw_segment')
        return offset

    def read(self,address,size,name):
        path=self.vm.output/(name+'.bin')
        self.vm.hmp(f'pmemsave {address} {size} "{path}"')
        return path.read_bytes()

    def graphics(self):
        begin=time.perf_counter()
        header=self.read(self.segment*16,256,'graphics-header')
        assert header[4:12]==b'CWRT0001' and struct.unpack_from('<H',header,12)[0]>=3,'graphics runtime ABI mismatch'
        pointer=struct.unpack_from('<H',header,206)[0]
        assert pointer and pointer+48<=65536,'invalid graphics descriptor offset'
        descriptor=self.read(self.segment*16+pointer,48,'graphics-descriptor')
        version,size,width,height,pitch,flags=struct.unpack_from('<6H',descriptor)
        assert version==1 and size>=48 and (width,height,pitch)==(320,200,320),'graphics descriptor mismatch'
        state=dict(host_observed=time.perf_counter(),read_seconds=time.perf_counter()-begin,
            live=header[37],busy=header[35],graphics_active=header[192],
            runtime_errors=struct.unpack_from('<H',header,44)[0],
            completed_callbacks=struct.unpack_from('<I',header,52)[0],
            completed_ui_paints=struct.unpack_from('<I',header,80)[0],
            focused=bool(flags&1),close_requested=bool(flags&2),
            guest_milliseconds=struct.unpack_from('<I',descriptor,28)[0],
            completed_visible_game_frames=struct.unpack_from('<I',descriptor,32)[0],
            status=struct.unpack_from('<H',descriptor,36)[0],
            host_bpp=struct.unpack_from('<H',descriptor,38)[0])
        return state

    def await_graphics(self,description,timeout=30):
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            state=self.graphics()
            if state['status'] or state['runtime_errors']:
                raise AssertionError((description,state))
            if state['graphics_active'] and state['completed_visible_game_frames']>0 and state['live']:
                self.event(description,**state)
                return state
            if not state['live']:
                raise AssertionError((description+' returned before a visible presentation',state,self.module()[1]['text']))
            time.sleep(.05)
        raise AssertionError((description+' timed out',state))

    def crop(self,name):
        position=self.geometry(11)
        frame=self.screen(name)
        x,y=position[0]+10,position[1]+34
        return frame[y:y+400,x:x+640,:3]

    def finish(self,name):
        self.until(lambda:self.b('dos_host_active')==0,name+' did not return',30)
        self.unhooked()
        self.refresh()
        if self.b('ui_window_flags',11):
            self.click(18,11)
            self.until(lambda:self.b('ui_window_flags',11)==0,name+' window did not close')
        assert self.w('dw_segment')==0,name+' runtime allocation remains'


def normalized(vm,offset):
    return subprocess.check_output(['scripts/serial_log_normalize.py','--offset',str(offset),str(vm.serial)]).decode('ascii','replace')


def smoke(ui,command):
    offset=ui.launch(command)
    first=ui.await_graphics('real DPMI checker became visible')
    images=[];states=[]
    for number in range(3):
        states.append(ui.graphics());images.append(ui.crop(f'cgsmoke-live-{number}'))
        time.sleep(.25)
    # The smoke's palette is (i,255-i,i XOR55h). The software pointer may cover
    # a few pixels, so require the palette identity for >99% of sampled cells.
    sampled=images[0][::4,::4].astype(np.int16)
    palette=(sampled[:,:,0]+sampled[:,:,1]==255)&(sampled[:,:,2]==(sampled[:,:,0]^0x55))
    assert float(palette.mean())>.99,('checker palette not visible',float(palette.mean()))
    unique=len(np.unique(images[0].reshape(-1,3),axis=0))
    changed=int(np.count_nonzero(np.any(images[0]!=images[-1],axis=2)))
    assert unique>64 and changed>10000,('checker animation absent',unique,changed)
    ui.vm.wait('[CGFXSMOKE] frames=',offset,20)
    text=normalized(ui.vm,offset)
    match=re.search(r'\[CGFXSMOKE\] frames=(\d+) elapsed_ms=(\d+) host_frames=(\d+) status=(\d+)',text)
    assert match and int(match[4])==0,text
    ui.finish('CGSMOKE')
    return dict(first=first,samples=states,palette_match_fraction=float(palette.mean()),
        distinct_displayed_colors=unique,changed_actual_pixels=changed,
        client_submissions=int(match[1]),guest_elapsed_ms=int(match[2]),
        completed_visible_presentations=int(match[3]),status=int(match[4]))


def measure_game(ui,seconds):
    start=ui.graphics();samples=[start]
    assert start['live'] and start['focused'] and not start['status'],start
    deadline=start['host_observed']+seconds
    while time.perf_counter()<deadline:
        time.sleep(min(.5,max(0,deadline-time.perf_counter())))
        sample=ui.graphics();samples.append(sample)
        assert sample['live'] and sample['focused'] and not sample['status'] and not sample['runtime_errors'],sample
    end=samples[-1]
    wall=end['host_observed']-start['host_observed']
    frames=(end['completed_visible_game_frames']-start['completed_visible_game_frames'])&0xffffffff
    guest_ms=(end['guest_milliseconds']-start['guest_milliseconds'])&0xffffffff
    assert .9 < guest_ms/(wall*1000) < 1.1,('guest timer does not track real elapsed time',guest_ms,wall)
    return dict(host_elapsed_seconds=wall,completed_visible_game_frames=frames,
        completed_game_frames_per_host_second=frames/wall,guest_elapsed_ms=guest_ms,
        guest_clock_to_host_ratio=guest_ms/(wall*1000),samples=samples,
        counter_excludes_clock_polls=True,observer_read_seconds=sum(s['read_seconds'] for s in samples))


def doom(ui,command,seconds,game='Doom'):
    # Open About before suspending the foreground loop. Only retained native
    # window controls may be manipulated while the DPMI client owns execution.
    ui.click(13);ui.click(6)
    ui.until(lambda:ui.b('ui_active_window')==2,'About did not open')
    x,y=ui.geometry(2);ui.drag(2,50-x,70-y)
    offset=ui.launch(command)
    ui.await_graphics(game+' source port produced a visible game frame',60)
    ui.vm.wait('[CGFX] '+game+' source port 320x200',offset,10)
    time.sleep(1)
    ui.vm.position(1160,650)
    initial=ui.crop('doom-game-before-input')
    assert len(np.unique(initial.reshape(-1,3),axis=0))>32,'game framebuffer is blank or monochrome'
    ui.vm.hmp('sendkey up 1000');time.sleep(1.3)
    ui.vm.hmp('sendkey right 800');time.sleep(1.1)
    moved=ui.crop('doom-game-after-input')
    changed=int(np.count_nonzero(np.any(initial[:320]!=moved[:320],axis=2)))
    assert changed>2000,('real movement keys did not change the game viewport',changed)
    # Measure while continuously turning. Ports suppress unchanged buffers,
    # so a stationary wall cannot be mistaken for newly animated frames.
    ui.vm.hmp(f'sendkey right {int((seconds+2)*1000)}')
    measurement=measure_game(ui,seconds)
    ui.vm.hmp('sendkey right 1');time.sleep(.2)
    measurement['unchanged_frame_submissions_suppressed']=True
    ui.crop('doom-after-frame-measurement')
    before=ui.graphics();ui.drag(2,100,35)
    ui.screen('doom-about-dragged')
    blurred=ui.graphics()
    assert blurred['live'] and not blurred['focused'],blurred
    time.sleep(.5)
    ui.click(62,11)
    ui.until(lambda:ui.b('ui_active_window')==11,'Doom did not regain native focus')
    ui.click(17,11)
    ui.until(lambda:ui.b('ui_window_flags',11)==2,'Doom did not minimize')
    # Let the single in-flight presentation finish before checking invisibility.
    time.sleep(.2);hidden_start=ui.graphics();time.sleep(1);hidden_end=ui.graphics()
    assert hidden_end['live'] and hidden_end['completed_visible_game_frames']==hidden_start['completed_visible_game_frames'],('invisible frames were counted',hidden_start,hidden_end)
    ui.screen('doom-minimized-native-desktop')
    ui.click(211)
    ui.until(lambda:ui.b('ui_active_window')==11 and ui.b('ui_window_flags',11)==1,'Doom taskbar restore failed')
    restored=ui.await_graphics('Doom visible after taskbar restore')
    deadline=time.monotonic()+5
    while restored['completed_visible_game_frames']<=hidden_end['completed_visible_game_frames']:
        assert time.monotonic()<deadline,'restored game did not present another frame'
        time.sleep(.05);restored=ui.graphics()
    ui.crop('doom-restored-game')
    ui.click(18,11)
    ui.finish('Doom cooperative close')
    ui.vm.key('f3');ui.until(lambda:ui.b('ui_active_window')==1,'native keyboard did not recover')
    ui.click(18,1);ui.until(lambda:ui.b('ui_window_flags',1)==0,'native mouse did not recover')
    ui.screen('desktop-after-graphics-children')
    return dict(measurement=measurement,movement_changed_game_pixels=changed,
        before_native_controls=before,blurred=blurred,minimized_start=hidden_start,
        minimized_end=hidden_end,restored=restored,clean_return=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image',type=Path,required=True)
    parser.add_argument('--listing',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--smoke-command',default='CGSMOKE.EXE')
    parser.add_argument('--doom-command',default='DOOMWIN.EXE -iwad C:\\APPS\\DOOM\\DOOM.WAD -nosound -warp 1 1 -nomonsters')
    parser.add_argument('--smoke-only',action='store_true')
    parser.add_argument('--game',choices=('Doom','Wolf'),default='Doom')
    parser.add_argument('--seconds',type=float,default=10)
    parser.add_argument('--minimum-fps',type=float,default=30)
    args=parser.parse_args();assert args.seconds>=10,'the requested game frame gate needs at least10 seconds'
    args.output.mkdir(parents=True,exist_ok=True)
    assert not (args.output/'report.json').exists(),'preserve earlier evidence; choose a new output directory'
    original=sha(args.image);disk=args.output/'private.img';shutil.copyfile(args.image,disk)
    source=FAT16(disk);shell=source.read('SYSTEM/SHELL.COM')
    report=dict(passed=False,events=[],source_image_sha256=original,
        shell_sha256=hashlib.sha256(shell).hexdigest(),listing_sha256=sha(args.listing),
        memory_mib=128,guest_memory_writes=False,guest_pauses=False,
        scope='cooperative DPMI graphics source port in QEMU; no hardware acceleration or old-PC speed claim',
        smoke_command=args.smoke_command,doom_command=args.doom_command,
        game=args.game,
        requested_minimum_completed_game_fps=args.minimum_fps)
    vm=WindowVM(disk,args.output,'std',memory=128,palette='platinum');report['qemu']=vm.process.args
    try:
        vm.ready();ui=Graphics(vm,shell,args.listing,report)
        ui.screen('desktop-before-graphics')
        report['smoke']=smoke(ui,args.smoke_command)
        if not args.smoke_only:
            report['doom']=doom(ui,args.doom_command,args.seconds,args.game)
            fps=report['doom']['measurement']['completed_game_frames_per_host_second']
            assert fps>=args.minimum_fps,f'Actual Doom visible game rate {fps:.3f}/s is below requested {args.minimum_fps}/s'
        report['passed']=True
    except Exception as exc:
        report.update(passed=False,error=repr(exc))
        try:
            ui.screen('failure');report['failure_graphics']=ui.graphics();ui.state('failure text state')
        except Exception as diagnostic:report['diagnostic_error']=repr(diagnostic)
        raise
    finally:
        vm.close()
        if sha(args.image)!=original:report.update(passed=False,error='source image changed')
        (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    assert report['passed'],report.get('error')


if __name__=='__main__':main()
