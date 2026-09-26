#!/usr/bin/env python3
"""Real BIOS-text DOS child, keyboard and concurrent native-window acceptance.

Only physical QEMU keyboard/mouse events mutate the guest. RAM observations bind
the matching shell listing and DOSWIN header; screenshots independently verify
the actual firmware font and attributes. This does not qualify B8000, graphics,
protected-mode programs, Windows 3.x, or physical T23/E500 firmware.
"""
import argparse
import hashlib
import json
import re
import shutil
import struct
import time
from pathlib import Path

import numpy as np
from PIL import Image

from qemu_test_installed_hdd import FAT16
from qemu_test_native_utilities import Utilities
from qemu_test_native_windows import WindowVM


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Session(Utilities):
    def __init__(self, vm, shell, listing, report):
        super().__init__(vm, shell, listing)
        self.report = report
        self.started = time.monotonic()
        self.original_vectors = self.vectors()
        report['original_vectors'] = self.original_vectors

    def event(self, name, **details):
        self.report['events'].append(dict(name=name, host_seconds=time.monotonic()-self.started, **details))
        print('[dos-window] '+name, flush=True)

    def until(self, predicate, description, timeout=15):
        deadline = time.monotonic()+timeout
        while time.monotonic()<deadline:
            self.refresh()
            if predicate():
                return
            time.sleep(.08)
        raise AssertionError(description)

    def point(self, action, owner=None):
        # A read-only debugger can sample the table midway through rebuilding
        # it. Real guest hit testing runs before painting, on the same guarded
        # callback. Observe that same idle boundary rather than treating a
        # partially published count as a missing rendered control.
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            self.refresh()
            active=self.b('dos_host_active')
            if self.b('ui_composing') or (active and self.module()[0][35]):
                time.sleep(.015)
                continue
            hits=self.hit_records()
            self.refresh()
            if self.b('ui_composing') or len(hits)!=self.w('ui_hit_count'):
                continue
            if active and self.module()[0][35]:
                continue
            break
        else:raise AssertionError('No idle boundary for read-only hit-table observation')
        width, height = self.w('ui_width'), self.w('ui_height')
        for n in range(len(hits)-1, -1, -1):
            x,y,right,bottom,encoded = hits[n]
            if encoded&255 != action or (owner is not None and encoded>>8 != owner+1):
                continue
            for px,py in (((x+right)//2,(y+bottom)//2), (x+8,y+8), (right-8,bottom-8)):
                if not (0 <= px < width-16 and 0 <= py < height-16):
                    continue
                front = next((i for i in range(len(hits)-1,-1,-1)
                              if hits[i][0]<=px<hits[i][2] and hits[i][1]<=py<hits[i][3]), None)
                if front == n:
                    return px,py
        raise AssertionError(f'No exposed action {action}, owner {owner}: {hits}')

    def click(self, action, owner=None):
        x,y = self.point(action,owner)
        self.vm.position(x,y)
        self.vm.hmp('mouse_button 1');time.sleep(.18)
        self.vm.hmp('mouse_button 0');time.sleep(.25)
        self.event('mouse click', action=action,owner=owner,position=[x,y])

    def drag(self, owner, dx, dy):
        before = self.geometry(owner)
        x,y = self.point(62,owner)
        self.vm.position(x,y)
        self.vm.hmp('mouse_button 1');time.sleep(.2)
        self.vm.move(dx//2,dy//2);time.sleep(.3)
        self.vm.hmp('mouse_button 0');time.sleep(.3)
        after = self.geometry(owner)
        assert after != before, f'Window {owner} did not move: {before}'
        self.event('native window dragged',owner=owner,before=before,after=after)
        return before,after

    def geometry(self, owner):
        self.refresh()
        return [self.w('ui_window_x',owner), self.w('ui_window_y',owner)]

    def vectors(self):
        path=self.vm.output/'ivt-observed.bin'
        self.vm.hmp(f'pmemsave 0 1024 "{path}"')
        data=path.read_bytes()
        return {f'{number:02X}':struct.unpack_from('<I',data,number*4)[0]
                for number in (8,0x10,0x15,0x16,0x2f,0x33)}

    def unhooked(self):
        current=self.vectors()
        assert current==self.original_vectors,('interrupt vectors not restored',current,self.original_vectors)
        self.event('original IRQ0/video/keyboard/mouse vectors restored',vectors=current)

    def module(self):
        self.refresh()
        segment = self.w('dw_segment')
        assert segment, 'DOSWIN allocation is absent'
        path = self.vm.output/'module-observed.bin'
        self.vm.hmp(f'pmemsave {segment*16} 65536 "{path}"')
        data = path.read_bytes()
        assert data[4:12] == b'CWRT0001', 'DOSWIN header mismatch'
        cells = struct.unpack_from('<H',data,18)[0]
        text = '\n'.join(bytes(data[cells+r*160:cells+(r+1)*160:2]).decode('cp437').rstrip() for r in range(25))
        return data, dict(segment=segment,installed=data[34],live=data[37],errors=struct.unpack_from('<H',data,44)[0],
                         ticks=struct.unpack_from('<I',data,48)[0],callbacks=struct.unpack_from('<I',data,52)[0],
                         focused=data[36],text=text,host_active=self.b('dos_host_active'))

    def state(self, name):
        _, state = self.module()
        self.event(name, **state)
        return state

    def screen(self, name):
        path = self.vm.shot(name)
        image = Image.open(path)
        image.save(path.with_suffix('.png'))
        return np.array(image)

    def pixels(self, name, row=1, columns=range(2,30)):
        """Compare actual 8x16 pixels to firmware glyphs and active RGB masks."""
        # Read on both sides of the screen capture so dynamic counters are
        # compared only when their cells remained stable during observation.
        # QEMU keeps running: no stop, GDB injection or guest-memory write.
        columns=tuple(columns)
        for attempt in range(24):
            self.refresh()
            data,state=self.module()
            assert state['live'] and not state['errors'],state
            cells,font=(struct.unpack_from('<H',data,off)[0] for off in (18,56))
            dirty=struct.unpack_from('<H',data,20)[0]
            if data[35] or struct.unpack_from('<I',data,dirty)[0]&(1<<row):
                time.sleep(.015)
                continue
            frame=self.screen(name)
            after,_=self.module()
            if (not after[35] and not struct.unpack_from('<I',after,dirty)[0]&(1<<row)
                and data[36:42]==after[36:42]
                and all(data[cells+row*160+c*2:cells+row*160+c*2+2]
                   ==after[cells+row*160+c*2:cells+row*160+c*2+2] for c in columns)):
                break
        else:raise AssertionError(f'{name}: text changed throughout all bounded capture attempts')
        origin = self.geometry(11)
        info = self.ram[self.offset('vc_info'):self.offset('vc_info')+256]
        colors = struct.unpack_from('<16I',self.ram,self.offset('vc_colors'))
        def rgb(index):
            value=colors[index]
            return [(value>>info[p+1]&((1<<info[p])-1))*255//((1<<info[p])-1) for p in (31,33,35)]
        compared=0
        for column in columns:
            character,attribute=data[cells+row*160+column*2:cells+row*160+column*2+2]
            glyph=np.array([data[font+character*32+r*2+1] for r in range(16)],dtype=np.uint8)
            mask=(glyph[:,None]&(128>>np.arange(8)))!=0
            expected=np.where(mask[:,:,None],rgb(attribute&15),rgb((attribute>>4)&7)).astype(np.uint8)
            if (state['live'] and state['focused'] and not data[41]&0x20
                and struct.unpack_from('<H',data,38)[0]==row*256+column):
                expected[14:16,:]=rgb(15)
            x,y=origin[0]+10+column*8,origin[1]+34+row*16
            actual=frame[y:y+16,x:x+8,:3]
            assert np.array_equal(actual,expected), f'{name}: glyph mismatch row={row} column={column} char={character} attr={attribute:02x}'
            compared+=128
        self.event('firmware text pixels matched', screenshot=name,pixels=compared,row=row,columns=list(columns),
                   capture_attempts=attempt+1,text=''.join(chr(data[cells+row*160+c*2]) for c in columns))

    def type_only(self, text):
        for ch in text:
            if ch.isupper():key='shift-'+ch.lower()
            else:key={' ':'spc','.':'dot','\\':'backslash','-':'minus','/':'slash',':':'shift-semicolon'}.get(ch,ch)
            self.vm.key(key)

    def raw(self, offset):
        return self.vm.serial.read_bytes()[offset:].decode('ascii','replace')


def command_gate(ui):
    vm=ui.vm
    ui.click(3)
    ui.until(lambda:ui.b('dos_host_active')==1,'COMMAND window did not launch')
    vm.text('dir')
    time.sleep(1)
    state=ui.state('interactive COMMAND DIR')
    # A long real directory scrolls its heading out of the 25-row surface.
    assert all(value in state['text'] for value in ('DWBIOST.COM','DWBIOSCH.COM','CiukiOS SHELL C:\\APPS>')),state
    vm.text('echo DOS WINDOW COMMAND OK')
    ui.until(lambda:'\nDOS WINDOW COMMAND OK\n' in ui.module()[1]['text'],
             'COMMAND did not execute complete keyboard ECHO input',20)
    state=ui.state('interactive COMMAND ECHO')
    assert 'DOS WINDOW COMMAND OK' in state['text'] and state['live'] and not state['errors'],state
    ui.screen('command-echo')
    vm.text('exit')
    ui.until(lambda:ui.b('dos_host_active')==0,'COMMAND EXIT did not restore foreground')
    assert ui.module()[1]['live']==0
    ui.unhooked()
    ui.click(18,11)
    ui.until(lambda:ui.b('ui_window_flags',11)==0,'finished DOS window did not close')
    ui.click(3)
    ui.until(lambda:ui.b('dos_host_active')==1,'second COMMAND window did not launch')
    ui.click(18,11)
    ui.until(lambda:ui.b('dos_host_active')==0 and ui.b('ui_window_flags',11)==0
             and ui.w('dw_segment')==0,'COMMAND cooperative title close did not exit and release memory',20)
    ui.event('COMMAND cooperative title close released runtime allocation')
    ui.unhooked()


def probe_gate(ui, exe, kind, escape=False):
    vm=ui.vm;prefix=f'[DOSWIN:{kind}] '
    # About exists before EXEC; the interrupt-time host may manipulate already
    # open windows, but deliberately cannot invoke new dialogs/DOS services.
    ui.click(13);ui.click(6)
    ui.until(lambda:ui.b('ui_active_window')==2,'About did not open')
    position=ui.geometry(2)
    ui.drag(2,50-position[0],70-position[1])
    vm.key('f3')
    ui.until(lambda:ui.b('ui_active_window')==1,'Run did not open')
    # Existing Run text persists between sessions; edit through real keyboard.
    length=ui.b('ui_run_len');vm.key('home')
    for _ in range(length):vm.key('delete')
    ui.type_only('run '+exe)
    offset=vm.offset();ui.click(58,1)
    vm.wait(prefix+'START psp=',offset,20)
    vm.wait(prefix+'FILE OK',offset,10)
    vm.wait(prefix+'LIVE ticks=',offset,10)
    first=ui.state(kind+' live before host input')
    ui.pixels(kind+'-native-text')
    # This title remains exposed because About was placed above the DOS panel.
    before_frame=ui.screen(kind+'-before-about-drag')
    old_position,new_position=ui.drag(2,100,35)
    after_frame=ui.screen(kind+'-about-dragged')
    x,y=old_position
    changed=np.any(before_frame[y:y+30,x:x+80]!=after_frame[y:y+30,x:x+80],axis=2)
    assert np.count_nonzero(changed)>500,'native About geometry moved in RAM without the old title being repainted'
    x,y=new_position
    old_x,old_y=old_position
    assert np.array_equal(after_frame[y,x],before_frame[old_y,old_x]),'moved About top bevel is absent from actual display'
    unfocused=ui.module()[1]
    assert unfocused['live'] and not unfocused['focused'],unfocused
    key_offset=vm.offset();vm.key('b');time.sleep(.6)
    assert prefix+'KEY ax=3062' not in ui.raw(key_offset),'unfocused DOS child received a native-window key'
    ui.click(62,11)
    vm.key('a');vm.wait(prefix+'KEY ax=1E61',offset,10)
    ui.click(17,11)
    ui.until(lambda:ui.b('ui_window_flags',11)==2,'DOS window did not minimize')
    minimized=ui.state(kind+' minimized while child live')
    assert minimized['live'] and minimized['ticks']>first['ticks'],minimized
    ui.screen(kind+'-minimized')
    ui.click(211)
    ui.until(lambda:ui.b('ui_window_flags',11)==1 and ui.b('ui_active_window')==11,'DOS taskbar restore failed')
    ui.pixels(kind+'-restored-text')
    vm.wait(prefix+'EXEC BEGIN',offset,30)
    vm.wait('[DOSWIN:BIOS-CHILD] END status=005A',offset,15)
    vm.wait(prefix+'EXEC RETURN',offset,15)
    vm.wait(prefix+'FILE MIDRUN OK',offset,20)
    late=ui.state(kind+' after nested EXEC and midrun file verification')
    assert late['live'] and not late['errors'] and late['ticks']>first['ticks'],late
    ui.pixels(kind+'-late-text')
    ui.pixels(kind+'-bios-scroll-text',row=23,columns=range(2,29))
    ui.pixels(kind+'-live-counter-pixels',row=5,columns=range(9,13))
    if escape:vm.key('esc')
    vm.wait(prefix+'END ticks=',offset,30)
    ui.until(lambda:ui.b('dos_host_active')==0,'probe exit did not restore foreground',20)
    raw=ui.raw(offset)
    assert prefix+'FAIL' not in raw,raw
    assert prefix+'KEY ax=3062' not in raw,'unfocused key was replayed after restoring DOS focus'
    starts=re.findall(re.escape(prefix)+r'START psp=([0-9A-F]{4})',raw)
    nested=re.findall(r'\[DOSWIN:BIOS-CHILD\] START psp=([0-9A-F]{4})',raw)
    assert len(starts)==len(nested)==1 and starts[0]!=nested[0],(starts,nested)
    reason='ESC' if escape else 'TIMEOUT'
    end=re.search(re.escape(prefix)+r'END ticks=([0-9A-F]{4}) frames=([0-9A-F]{4}) reason='+reason,raw)
    assert end and int(end[1],16)>=364,(reason,raw)
    live=[int(x,16) for x in re.findall(re.escape(prefix)+r'LIVE ticks=([0-9A-F]{4})',raw)]
    assert len(live)>=15 and live==sorted(set(live)),live
    final=ui.state(kind+' finished')
    assert not final['live'] and not final['installed'] and not final['errors'],final
    ui.unhooked()
    ui.report['cases'].append(dict(program=exe,reason=reason,ticks=int(end[1],16),frames=int(end[2],16),
                                   live_samples=len(live),parent_psp=starts[0],nested_psp=nested[0]))
    ui.screen(kind+'-finished')
    ui.click(18,11)
    ui.until(lambda:ui.b('ui_window_flags',11)==0,'DOS window memory release failed')


def fullscreen_return_gate(ui):
    ui.refresh();native_dimensions=(ui.w('ui_width'),ui.w('ui_height'))
    vm=ui.vm;offset=vm.offset();vm.key('f4');vm.wait('[DESKTOP] DOS',offset,15)
    offset=vm.offset();vm.text('run DWBIOST.COM')
    vm.wait('[DOSWIN:BIOS-COM] LIVE ticks=',offset,20)
    frame=ui.screen('full-screen-after-unhook')
    assert (frame.shape[1],frame.shape[0])!=native_dimensions,'full-screen child is still displaying the native VBE desktop'
    vm.key('esc');vm.wait('[DOSWIN:BIOS-COM] END ticks=',offset,10)
    vm.wait('CiukiOS SHELL C:\\APPS>',offset,10)
    offset=vm.offset();vm.text('exit');vm.ready(offset)
    vm.key('f3');ui.until(lambda:ui.b('ui_active_window')==1,'desktop keyboard after full-screen return failed')
    ui.click(18,1)
    ui.until(lambda:ui.b('ui_window_flags',1)==0,'desktop mouse after full-screen return failed')
    ui.screen('desktop-after-all-children')
    ui.event('full-screen BIOS child and native input restored after uninstall')
    ui.unhooked()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image',type=Path,required=True)
    parser.add_argument('--listing',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--com-only',action='store_true',help='development scope: omit separate MZ/ESC gate')
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    assert not (args.output/'report.json').exists(),'preserve existing evidence; choose a fresh output directory'
    original=sha(args.image);disk=args.output/'private.img';shutil.copyfile(args.image,disk)
    source=FAT16(disk);shell=source.read('SYSTEM/SHELL.COM')
    report=dict(scope='BIOS text only; actual foreground child plus IRQ-driven native presentation',
                source_image_sha256=original,shell_sha256=hashlib.sha256(shell).hexdigest(),
                listing_sha256=sha(args.listing),memory_mib=128,guest_memory_writes=False,events=[],cases=[])
    vm=WindowVM(disk,args.output,'std',memory=128,palette='platinum')
    report['qemu']=vm.process.args
    try:
        vm.ready();ui=Session(vm,shell,args.listing,report)
        ui.screen('desktop-initial');command_gate(ui)
        probe_gate(ui,'DWBIOST.COM','BIOS-COM')
        if not args.com_only:probe_gate(ui,'DWBIOST.EXE','BIOS-MZ',escape=True)
        fullscreen_return_gate(ui)
        report['passed']=True
    except Exception as exc:
        report.update(passed=False,error=repr(exc))
        try:
            ui.screen('failure');ui.state('failure observation')
        except Exception as diagnostic_error:report['diagnostic_error']=repr(diagnostic_error)
        raise
    finally:
        vm.close()
        if sha(args.image)!=original:
            report.update(passed=False,error='source image changed')
        if report.get('passed'):
            try:FAT16(disk).read('APPS/DWCHECK.DAT')
            except (FileNotFoundError,KeyError):report['scratch_file_absent']=True
            else:report.update(passed=False,error='probe left its scratch file behind')
        (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    assert report['passed'],report.get('error')


if __name__=='__main__':main()
