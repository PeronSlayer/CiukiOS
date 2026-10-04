#!/usr/bin/env python3
"""Exercise overlapping native windows and inspect real rendered/input results."""
import argparse,hashlib,json,shutil,struct,subprocess,time,wave
from pathlib import Path
import numpy as np
from PIL import Image
from qemu_test_full_display_profile import VM
from qemu_test_native_desktop import DesktopVM
from qemu_test_installed_hdd import FAT16,listing_address
from analyze_audio_wav import pcm_payload

class WindowVM(DesktopVM):
    def __init__(self,disk,output,vga,boot_capture=False,memory=512,video=None,palette='legacy',extra_qemu_args=()):
        output.mkdir(parents=True,exist_ok=True)
        extra=['-vga',vga]
        if video:
            width,height=map(int,video.lower().split('x'))
            extra=['-vga','none','-device',f'VGA,vgamem_mb=32,xres={width},yres={height}']
        self.palette=palette
        self.control_latencies=[]
        if palette=='platinum':
            self.cursor_colors=((36,40,48),(246,246,242))
        if boot_capture:
            extra+=['-audiodev',f'wav,id=snd,path={output}/audio.wav','-device','AC97,audiodev=snd']
        extra.extend(extra_qemu_args)
        VM.__init__(self,disk,output,qemu_args=extra,memory=memory)

    def click(self):
        if getattr(self,'palette','legacy')!='platinum':return super().click()
        # A software pointer can be intact while an off-screen page is still
        # being composed. Synchronize controls through completed paints instead
        # of assuming every repaint finishes within SetupVM.click's 500ms sleep.
        time.sleep(.3)
        offset=self.offset();self.hmp('mouse_button 1')
        self.wait('[DESKTOP] PAINT',offset,15)
        offset=self.offset();started=time.monotonic();self.hmp('mouse_button 0')
        self.wait('[DESKTOP] PAINT',offset,15)
        self.control_latencies.append(time.monotonic()-started)
        self.pointer()

    def completed_control_click(self,x,y):
        """Wait for both press feedback and the release action's completed paint.

        A visible cursor does not imply completion of a large banked redraw.
        Return measured host time so a slow operation is not hidden by waiting.
        """
        self.position(x,y);time.sleep(.3)  # prior focus accent is bounded to 220ms
        offset=self.offset();self.hmp('mouse_button 1')
        self.wait('[DESKTOP] PAINT',offset,15)
        offset=self.offset();started=time.monotonic();self.hmp('mouse_button 0')
        self.wait('[DESKTOP] PAINT',offset,15)
        self.pointer()
        return time.monotonic()-started
    def active_rect(self):
        self.pointer()
        a=np.array(Image.open(self.shot('active-window')))
        palette=getattr(self,'palette','legacy')
        color=(52,72,121) if palette=='platinum' else (32,53,73)
        paper=(246,246,242) if palette=='platinum' else (247,247,239)
        ink=(np.abs(a.astype(int)-color)<=4).all(axis=2)
        for y in range(32,a.shape[0]-35):
            edges=np.diff(np.r_[False,ink[y],False].astype(np.int8))
            for x,end in zip(np.flatnonzero(edges==1),np.flatnonzero(edges==-1)):
                if end-x<160:continue
                # Locate the actual continuous top bevel. The active title can
                # contain stripes or tabs; its first solid tab anchors the frame.
                row=(np.abs(a[y-3].astype(int)-paper)<=4).all(axis=1)
                left=int(x-3)
                if left<0 or not row[left]:continue
                stops=np.flatnonzero(~row[left:])
                if len(stops) and stops[0]>=450:return left,y-3,int(stops[0]+1)
        raise AssertionError('no complete active window title')

def pixels(vm,name):return np.array(Image.open(vm.shot(name)))

def rendered_frame_height(vm,frame,x,y):
    paper=(246,246,242) if getattr(vm,'palette','legacy')=='platinum' else (247,247,239)
    column=(np.abs(frame[y:,x].astype(int)-paper)<=4).all(axis=1)
    stops=np.flatnonzero(~column)
    assert len(stops) and stops[0]>=180,'no continuous rendered left window bevel'
    return int(stops[0]+1)

def burst(vm,name,count=8):
    """Capture successive actual display frames without PNG encoding between them.

    These timings include host monitor/capture overhead. They are evidence of
    sampled display damage, never a Pentium III frame-rate benchmark.
    """
    paths=[];timestamps=[];start=time.monotonic()
    for n in range(count):
        paths.append(VM.shot(vm,f'{name}-{n:02d}'))
        timestamps.append(time.monotonic()-start)
    frames=[]
    for path in paths:
        with Image.open(path) as im:
            frames.append(np.array(im));im.save(path.with_suffix('.png'))
    return frames,timestamps

def check_damage(vm,report,name,before,frames,boxes,timestamps):
    """Every sampled pixel outside explicitly affected geometry must survive."""
    height,width=before.shape[:2]
    stable=np.ones((height,width),bool)
    for x,y,w,h in boxes:
        stable[max(0,y):min(height,y+h),max(0,x):min(width,x+w)]=False
    # The minute clock can change independently of window input.
    stable[height-32:,width-78:]=False
    counts=[]
    for index,frame in enumerate(frames):
        assert frame.shape==before.shape,f'{name}: resolution changed during interaction'
        damage=np.any(frame!=before,axis=2)&stable
        counts.append(int(damage.sum()))
        if damage.any():
            diagnostic=frame.copy();diagnostic[damage]=(255,0,255)
            Image.fromarray(diagnostic).save(vm.output/f'{name}-unexpected-damage-{index:02d}.png')
    entry={'frames':len(frames),'distinct_frames':len({hashlib.sha256(f.tobytes()).digest() for f in frames}),
           'outside_changed_pixels':counts,'capture_elapsed_seconds':timestamps,
           'allowed_rectangles':boxes,'checked_pixels_per_frame':int(stable.sum())}
    report.setdefault('temporal_damage',{})[name]=entry
    assert not any(counts),f'{name}: pixels outside affected rectangles changed: {counts}'

def capture_boot(v,disk,output):
    asset=FAT16(disk).read('SYSTEM/SPLASH.BIN')
    palette=np.frombuffer(asset[:768],dtype=np.uint8).reshape(256,3).astype(int)
    palette=(palette>>2)*255//63
    indices=np.frombuffer(asset[768:768+256*192],dtype=np.uint8).reshape(192,256)
    repeats=np.full(256,3);repeats[7::8]=4
    expected=np.repeat(np.repeat(palette[indices],repeats,axis=1),3,axis=0)
    photo_frames=[];bars=set();deadline=time.monotonic()+90
    while time.monotonic()<deadline:
        name=f'boot-{len(photo_frames):03d}'
        frame=pixels(v,'boot-observed')
        if frame.shape==(600,800,3):
            match=float((np.abs(frame[:576].astype(int)-expected)<=4).all(axis=2).mean())
            if match>.995:
                path=output/(name+'.png');Image.fromarray(frame).save(path)
                photo_frames.append({'path':path.name,'photo_matching_fraction':match})
                bars.add(hashlib.sha256(frame[576:].tobytes()).hexdigest())
        serial=subprocess.check_output(['scripts/serial_log_normalize.py',str(v.serial)]) if v.serial.exists() else b''
        if b'[DESKTOP] READY' in serial:break
        time.sleep(.04)
    v.ready()
    assert photo_frames,'boot did not display the original Ciuki photograph'
    assert len(bars)>=2,f'boot progress did not visibly change: {len(bars)} states'
    # Snapshot only the PCM produced before any keyboard/mouse/application action.
    raw=(output/'audio.wav').read_bytes();pcm,bits=pcm_payload(raw)
    assert bits==16 and len(pcm)>100000,'startup PCM missing or too short'
    samples=np.frombuffer(pcm,dtype='<i2').astype(float)
    metrics={'pcm_bytes':len(pcm),'ac_rms':float(np.std(samples)),
             'peak':int(np.max(np.abs(samples))),'nonzero_samples':int(np.count_nonzero(samples)),
             'unique_samples':len(np.unique(samples)),
             'photo_frames':photo_frames,'progress_states':len(bars)}
    assert metrics['ac_rms']>20 and metrics['peak']>100,'startup capture is silent'
    channels=struct.unpack_from('<H',raw,22)[0];rate=struct.unpack_from('<I',raw,24)[0]
    metrics['sample_rate']=rate;metrics['channels']=channels
    metrics['duration_seconds']=len(pcm)/(rate*channels*2)
    with wave.open(str(output/'startup-only.wav'),'wb') as wav:
        wav.setnchannels(channels);wav.setsampwidth(2);wav.setframerate(rate);wav.writeframes(pcm)
    return metrics

def main():
    p=argparse.ArgumentParser();p.add_argument('--disk',type=Path,required=True)
    p.add_argument('--shell',type=Path);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--kernel',type=Path)
    p.add_argument('--media',type=Path)
    p.add_argument('--boot-capture',action='store_true')
    p.add_argument('--memory',type=int,default=128)
    p.add_argument('--video',help='Expose this preferred VBE/EDID mode, e.g. 2560x1440')
    p.add_argument('--palette',choices=['legacy','platinum'],default='legacy')
    p.add_argument('--profile',default='0800',choices=['0800','AUTO','1024','0640','TEXT'])
    p.add_argument('--vga',default='std',choices=['std','cirrus']);a=p.parse_args()
    a.output.mkdir(parents=True,exist_ok=True)
    disk=a.output/'target.img';shutil.copyfile(a.disk,disk)
    volume=f'{disk}@@{FAT16(disk).start}'
    profile=a.output/'profile.cfg';profile.write_bytes(a.profile.encode())
    for source,dest in [(a.shell,'::SYSTEM/SHELL.COM'),(profile,'::SYSTEM/VIDEO/DISPLAY.CFG')]:
        if source:subprocess.run(['mcopy','-o','-i',volume,str(source),dest],check=True)
    if a.shell:shutil.copyfile(a.shell,a.output/'tested-shell.com')
    if a.kernel:
        subprocess.run(['mcopy','-o','-i',volume,str(a.kernel),'::SYSTEM/CIUKIDOS.SYS'],check=True)
    if a.media:
        subprocess.run(['mcopy','-o','-i',volume,str(a.media),'::APPS/MEDIA.COM'],check=True)
    source=FAT16(disk)
    report={'harness_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            'payload_sha256':{path:hashlib.sha256(source.read(path)).hexdigest()
                            for path in ('SYSTEM/CIUKIDOS.SYS','SYSTEM/SHELL.COM','APPS/MEDIA.COM')}}
    v=WindowVM(disk,a.output,a.vga,a.boot_capture,a.memory,a.video,a.palette)
    report['qemu_command']=v.process.args
    report['memory_mib']=a.memory
    try:
        if a.boot_capture:report['boot']=capture_boot(v,disk,a.output)
        v.ready();base=pixels(v,'desktop');h,w=base.shape[:2]
        assert w>=800 and h>=600,(w,h)
        if a.video:assert (w,h)==tuple(map(int,a.video.split('x'))),(w,h,a.video)
        report['mode']=[w,h]
        # Open the real native Display control panel from its desktop shortcut.
        # Cancel here: persistence and mode-setting have their own runtime test.
        v.click_at(52,286);assert v.active_rect()[2]==466,'Display panel did not open'
        v.shot('display-controls');v.repaint_key('esc')
        # Opening Run must preserve unrelated desktop pixels on every sampled frame.
        v.position(w-35,h-55);px,py,pw=v.active_rect();mx,my=v.pointer();base=pixels(v,'before-open')
        offset=v.offset();v.hmp('sendkey f3 30');frames,opening_times=burst(v,'open-frame')
        v.wait('[DESKTOP] PAINT',offset,30);rx,ry,rw=v.active_rect();assert rw==466
        stable=np.ones((h,w),bool)
        stable[ry:ry+232,rx:rx+472]=False
        stable[py+3:py+28,px+3:px+pw-3]=False # inactive title color
        stable[py+338:py+370,px+pw-95:px+pw]=False # old focused Open button
        stable[h-32:,:]=False
        # The software pointer is hidden only during its intersecting band's
        # VRAM copy; measure that footprint separately from desktop damage.
        stable[my:my+16,mx:mx+24]=False
        for frame in frames:assert np.array_equal(frame[stable],base[stable]),'unrelated desktop pixels flashed'
        report['opening_frames_checked']=len(frames)
        report['distinct_opening_frames']=len({frame.tobytes() for frame in frames})
        report['opening_capture_elapsed_seconds']=opening_times
        report['cursor_transient_pixels']=[int(np.any(frame[my:my+16,mx:mx+24]!=base[my:my+16,mx:mx+24],axis=2).sum()) for frame in frames]
        for ch in 'echo retained':v.key('spc' if ch==' ' else ch)
        v.pointer();before=pixels(v,'run-text')[ry+76:ry+104,rx+22:rx+442].copy()
        v.repaint_key('f1');ax,ay,aw=v.active_rect();assert aw==466
        v.shot('about')
        assert (ax,ay)!=(rx,ry),'About replaced the Run window geometry'
        v.repaint_key('alt-tab');assert v.active_rect()==(rx,ry,rw),'Alt+Tab failed to focus Run'
        v.repaint_key('alt-tab');assert v.active_rect()==(ax,ay,aw),'Alt+Tab failed to restore About focus'
        v.position(ax+170,ay+12);v.hmp('mouse_button 1');time.sleep(.15)
        positions=[];durations=[]
        for n in range(6):
            old_x,old_y,old_w=v.active_rect();before_drag=pixels(v,f'drag-before-{n:02d}')
            started=time.monotonic();v.hmp('mouse_move -5 -5 0')
            drag_frames,drag_times=burst(v,f'drag-transition-{n:02d}',4)
            new_x,new_y,new_w=v.active_rect()
            positions.append((new_x,new_y));durations.append(time.monotonic()-started)
            # A dialog and its shadow cover 471x232 pixels (ui_comp_damage_drag).
            # The former 470x230 box omitted the shadow's last row; that was
            # hidden while slow banked paints finished after the four samples.
            check_damage(v,report,f'drag-{n:02d}',before_drag,drag_frames,
                         [(old_x,old_y,old_w+5,232),(new_x,new_y,new_w+5,232)],drag_times)
            v.shot(f'drag-frame-{n:02d}')
        v.hmp('mouse_button 0');time.sleep(.3)
        assert len(set(positions))>=5,positions
        assert positions[-1][0]<ax and positions[-1][1]<ay,positions
        x,y,_=v.active_rect();v.repaint_key('alt-f4')
        assert v.active_rect()==(rx,ry,rw),'closing About did not reveal retained Run'
        v.position(w-35,h-55);after=pixels(v,'run-retained')[ry+76:ry+104,rx+22:rx+442]
        assert np.array_equal(before,after),'Run text changed while another window was active'
        report['drag_positions']=positions;report['drag_capture_seconds']=durations
        # Minimize a native window, then restore it using its own task button.
        v.click_at(rx+424,ry+14);assert v.active_rect()[2]!=466,'Run stayed visible after minimize'
        v.click_at(280,h-19);assert v.active_rect()==(rx,ry,rw),'task button failed to restore Run'
        v.position(w-35,h-55);after=pixels(v,'run-restored')[ry+76:ry+104,rx+22:rx+442]
        assert np.array_equal(before,after),'minimize discarded the edit buffer'
        # Starting at Run, Tab reaches Cancel, then the native minimize control.
        # Enter must perform that focused action and keep a restorable task.
        v.repaint_key('tab');v.shot('keyboard-focus-cancel')
        v.repaint_key('tab');v.shot('keyboard-focus-minimize');v.repaint_key('ret')
        assert v.active_rect()[2]!=466,'keyboard minimize did not hide Run'
        v.click_at(280,h-19);v.click_at(rx+448,ry+14)
        px,py,pw=v.active_rect();v.position(w-35,h-55)
        category=pixels(v,'applications-tab')[py+72:py+330,px+10:px+pw-10]
        v.repaint_key('ctrl-tab')
        games=pixels(v,'games-tab')[py+72:py+330,px+10:px+pw-10]
        assert np.any(category!=games,axis=2).sum()>1000,'Ctrl+Tab did not change the visible category'
        v.repaint_key('ctrl-tab');v.repaint_key('ctrl-tab')
        again=pixels(v,'applications-returned')[py+72:py+330,px+10:px+pw-10]
        assert np.array_equal(category,again),'category cycle did not restore Applications'
        v.edges((w,h))
        # Exercise the Programs frame controls too, including a resize whose
        # actual rendered right/bottom edges must remain inside the work area.
        px,py,pw=v.active_rect()
        fixed_bar=pixels(v,'before-frame-controls')[h-31:,:w-75].copy()
        v.click_at(px+pw-64,py+15)
        try:v.active_rect()
        except AssertionError:pass
        else:raise AssertionError('Programs did not minimize')
        v.click_at(160,h-19);assert v.active_rect()==(px,py,pw)
        if a.palette=='platinum':
            report['maximize_completed_seconds']=v.completed_control_click(px+pw-40,py+15)
        else:v.click_at(px+pw-40,py+15)
        assert v.active_rect()==(8,34,w-16)
        if a.palette=='platinum':
            report['restore_completed_seconds']=v.completed_control_click(w-48,49)
        else:v.click_at(w-48,49)
        assert v.active_rect()==(px,py,pw)
        v.position(px+170,py+13);v.hmp('mouse_button 1');time.sleep(.1)
        v.move(-8,-6);v.hmp('mouse_button 0');time.sleep(.3)
        nx,ny,nw=v.active_rect();assert nx<px and ny<py and nw==pw
        v.position(nx+nw-7,ny+363);v.hmp('mouse_button 1');time.sleep(.1)
        before_resize=pixels(v,'before-resize')
        v.hmp('mouse_move 8 8 0');resize_frames,resize_times=burst(v,'resize-transition')
        v.hmp('mouse_button 0');time.sleep(.3)
        fx,fy,fw=v.active_rect();assert fw>nw and fx+fw+4<=w
        grip_pointer=v.pointer()
        frame=pixels(v,'programs-resized')
        old_height=rendered_frame_height(v,before_resize,nx,ny)
        new_height=rendered_frame_height(v,frame,fx,fy)
        assert new_height>old_height,'resize did not increase rendered frame height'
        # The frame shadow is drawn 5 rows below the bevel (U_RECT wy+5), and
        # the pointer follows the grip. The former box stopped one shadow row
        # short and omitted the pointer; slow banked paints completed only
        # after the eight samples, which hid both omissions.
        bottom=max(ny+old_height+5,fy+new_height+5)
        assert bottom<=h-32,'resized frame or pointer crossed the fixed taskbar'
        check_damage(v,report,'resize',before_resize,resize_frames,
                     [(nx,ny,max(nw,fw)+4,bottom-ny),(*grip_pointer,24,16)],resize_times)
        # Window frame/shadow cannot overwrite the desktop's fixed taskbar.
        assert np.array_equal(frame[h-31:,:w-75],fixed_bar),'window frame overwrote fixed taskbar pixels'
        report['programs_frame']=[fx,fy,fw]
        v.hmp(f'pmemsave 0x11800 65536 "{a.output}/shell-before-dos.bin"')
        # DOS must continue executing commands after opening/moving native windows.
        off=v.offset();v.key('f4');v.wait('[DESKTOP] DOS',off,30)
        v.wait('CiukiOS SHELL C:\\APPS>',off,60)
        off=v.offset();v.text('echo WINDOW RETURN OK');v.wait('WINDOW RETURN OK\r\nCiukiOS SHELL',off,60)
        v.key('caps_lock');v.key('caps_lock')
        off=v.offset();v.text('comdemo');v.wait('COM demo via INT21h',off,30)
        v.wait('CiukiOS SHELL C:\\APPS>',off,30)
        v.shot('dos-after-windows');off=v.offset();v.text('exit');v.ready(off);v.pointer()
        if a.kernel and a.kernel.with_suffix('.lst').is_file():
            listing=a.kernel.with_suffix('.lst').read_text().splitlines()
            end=listing_address(listing,'boot_drive');xms=listing_address(listing,'xms_entrypoint')
            reference=source.read('SYSTEM/CIUKIDOS.SYS')[:end]
            dump=a.output/'kernel-code-final.bin';v.hmp(f'pmemsave 0x3000 {end} "{dump}"')
            actual=dump.read_bytes();assert len(actual)==len(reference)
            changes=[i for i,(x,y) in enumerate(zip(reference,actual)) if x!=y and not xms<=i<xms+5]
            assert not changes,f'kernel code changed at {[hex(i) for i in changes[:32]]}'
            report['kernel_code']={'bytes':end,'unexpected_changes':0}
        report['checks']=['retained overlap','focus and close','six live drag steps',
                          'sampled drag and resize damage containment','minimize/task restore',
                          'input retained','keyboard minimize','Ctrl+Tab categories',
                          'Programs maximize/drag/resize','cursor four corners',
                          'DOS echo, Caps Lock, COM execution and desktop return']
        report['mouse_control_release_to_paint_seconds']=v.control_latencies
        (a.output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps(report),flush=True)
    except Exception:
        (a.output/'partial-result.json').write_text(json.dumps(report,indent=2)+'\n')
        v.hmp(f'pmemsave 0 1048576 "{a.output}/failure-ram.bin"')
        v.shot('failure');(a.output/'registers.log').write_bytes(v.hmp('info registers'));raise
    finally:v.close()
if __name__=='__main__':main()
