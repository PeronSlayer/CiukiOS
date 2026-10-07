#!/usr/bin/env python3
"""Exercise the shipping native desktop using real BIOS keys and PS/2 input."""
import argparse
import hashlib
import os
from pathlib import Path
import struct
import time
import numpy as np
from PIL import Image
from qemu_test_setup_graphical import SetupVM


def match_pointer(pixels,cursor_black,cursor_white):
    height,width = pixels.shape[:2]
    signed=pixels.astype(np.int16)
    black = (np.abs(signed - np.asarray(cursor_black,dtype=np.int16)) <= 4).all(axis=2)
    white = (np.abs(signed - np.asarray(cursor_white,dtype=np.int16)) <= 4).all(axis=2)
    bm = (0x8000,0xC000,0xE000,0xF000,0xF800,0xFC00,0xFE00,0xFF00,
          0xFF80,0xFFC0,0xFE00,0xEF00,0xCF00,0x8780,0x0780,0x0300)
    wm = (0,0,0x4000,0x6000,0x7000,0x7800,0x7C00,0x7E00,
          0x7F00,0x7C00,0x6C00,0x4600,0x0600,0x0300,0x0300,0)
    h,w = height-15,width-15
    # Seed with three opaque arrow pixels, then examine only matching
    # coordinates. Scanning the entire 2K frame for each of 100+ sprite bits
    # made the host test unnecessarily slow and memory hungry.
    ys,xs=np.nonzero(black[:h,:w]&black[1:1+h,1:1+w]&white[2:2+h,1:1+w])
    for y in range(16):
        for x in range(16):
            bit = 1 << (15-x)
            if wm[y] & bit:
                valid=white[ys+y,xs+x]
            elif bm[y] & bit:
                valid=black[ys+y,xs+x]
            else:continue
            ys,xs=ys[valid],xs[valid]
    assert len(xs)==1, f'expected one intact desktop cursor; got {len(xs)}'
    y,x = ys[0],xs[0]
    return int(x),int(y)


class DesktopVM(SetupVM):
    auto_enter_dos = False

    def __init__(self,disks,iso,output):
        super().__init__(disks,iso,output,qemu_args=[
            '-audiodev',f'wav,id=snd,path={output}/audio.wav',
            '-device','AC97,audiodev=snd'])

    def ready(self, offset=0):
        self.wait('[DESKTOP] READY', offset, 90)
        time.sleep(1)

    def repaint_key(self, key):
        offset=self.offset()
        self.key(key)
        self.wait('[DESKTOP] PAINT',offset,30)

    def pointer(self):
        deadline=time.monotonic()+12
        while True:
            try:return self._pointer()
            except AssertionError:
                if time.monotonic()>=deadline:raise
                time.sleep(.1)

    def click(self):
        super().click()
        self.pointer()  # A banked full repaint must complete before the next action.

    def _pointer(self):
        for _attempt in range(5):
            try:
                pixels = np.array(Image.open(self.shot('pointer')))
                break
            except Exception:
                if _attempt == 4: raise
                time.sleep(.05)
        base_palettes=[getattr(self,'cursor_colors',((48,61,73),(247,247,239))),
                       ((48,61,73),(247,247,239)),
                       ((36,40,48),(246,246,242)),
                       ((56,32,40),(255,247,247))]
        palettes=[]
        for pair in base_palettes:
            for colors in (pair,
                           tuple(self._quantize_rgb(color, (5,5,5)) for color in pair),
                           tuple(self._quantize_rgb(color, (5,6,5)) for color in pair)):
                if colors not in palettes:
                    palettes.append(colors)
        # Theme settings can change the software cursor colours even when a
        # gate boots a fresh copy of the image. VBE 15/16-bit scanout rounds
        # those palette colors to RGB555/RGB565; match those exact expansions
        # without widening the per-pixel tolerance or changing the 16x16 mask.
        last_error = None
        for colors in palettes:
            try:return match_pointer(pixels,*colors)
            except AssertionError as exc:
                last_error = exc
        raise last_error

    @staticmethod
    def _quantize_rgb(color, bits):
        out=[]
        for channel,depth in zip(color,bits):
            value=channel >> (8-depth)
            out.append((value << (8-depth)) | (value >> (2*depth-8)))
        return tuple(out)


    def position(self, x, y):
        for _ in range(35):
            px,py = self.pointer()
            if abs(px-x)<=3 and abs(py-y)<=3:
                return
            # A 2K desktop can require more than the old 35*60 pixel travel
            # budget. Use the full signed PS/2 packet range for distant targets.
            self.move(max(-127,min(127,round((x-px)/2))),
                      max(-127,min(127,round((y-py)/2))))
        raise AssertionError(f'pointer did not reach {(x,y)}: {self.pointer()}')

    def click_at(self,x,y):
        self.position(x,y)
        self.click()

    def edges(self, size):
        w,h=size
        for dx,dy,expected in ((-127,-127,(0,0)),(127,-127,(w-24,0)),
                               (127,127,(w-24,h-16)),(-127,127,(0,h-16))):
            for _ in range(24):
                self.hmp(f'mouse_move {dx} {dy} 0');time.sleep(.035)
            time.sleep(.3)
            assert self.pointer()==expected, f'cursor escaped or failed to saturate: {self.pointer()} != {expected}'
            self.move(-16 if dx>0 else 16,-16 if dy>0 else 16)
            x,y=self.pointer()
            assert 0<x<w-24 and 0<y<h-16,'cursor retained hidden travel outside screen'
        print(f'[native-desktop] PASS all four cursor limits and immediate reverse at {w}x{h}',flush=True)

    def dos(self):
        offset=self.offset()
        self.key('f4')
        self.wait('[DESKTOP] DOS',offset)
        self.wait('CiukiOS SHELL D:\\APPS>',offset)

    def desktop(self):
        offset=self.offset()
        self.text('exit')
        self.ready(offset)

    def command(self, command, marker=None, timeout=30):
        offset=self.offset()
        self.text(command)
        if marker:self.wait(marker,offset,timeout)
        self.wait('CiukiOS SHELL D:\\APPS>',offset,timeout)
        return offset

    def library_rect(self):
        self.pointer()
        a=np.array(Image.open(self.shot('window-geometry')))
        # The platinum title is a 180px tab on a striped rail. Its width is
        # not the frame width. Anchor on the tab, then measure the continuous
        # top bevel, as the native-window suite does. Keep the legacy palette
        # accepted so the same test still checks the pre-redesign baseline.
        palettes=(((52,72,121),(246,246,242)),((32,53,73),(247,247,239)))
        signed=a.astype(np.int16)
        for color,paper in palettes:
            ink=(np.abs(signed-color)<=4).all(axis=2)
            for y in range(32,a.shape[0]-40):
                edges=np.diff(np.r_[False,ink[y],False].astype(np.int8))
                for x,end in zip(np.flatnonzero(edges==1),np.flatnonzero(edges==-1)):
                    if end-x<160:continue
                    row=(np.abs(signed[y-3]-paper)<=4).all(axis=1)
                    left=int(x-3)
                    if left<0 or not row[left]:continue
                    stops=np.flatnonzero(~row[left:])
                    if len(stops) and stops[0]>=450:
                        return left,y-3,int(stops[0]+1)
        raise AssertionError('library window is missing')

    def select_recovery(self):
        # Read VGA text while GRUB's menu is visible; avoid a timing-only key.
        deadline=time.monotonic()+12
        dump=self.output/'boot-menu.bin'
        while time.monotonic()<deadline:
            result=self.hmp(f'pmemsave 0xb8000 4000 \"{dump}\"')
            if not dump.exists():
                time.sleep(.1)
                continue
            text=dump.read_bytes()[::2].decode('cp437',errors='replace')
            if 'CiukiOS - recovery' in text and 'Booting' not in text:
                self.shot('recovery-menu')
                self.key('up');self.key('home')
                for _ in range(5):self.key('down')
                self.key('ret')
                return
            time.sleep(.08)
        raise AssertionError(f'GRUB recovery menu was not offered: {result!r}')


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--iso',type=Path,default=Path('build/full/CiukiOS_full_cd_0-8-0.iso'))
    ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--case',choices=('desktop','safe','recovery','doom','costa','files'),default='desktop')
    args=ap.parse_args()
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    target=out/'untouched.img'
    with target.open('wb') as f:f.truncate(128*1024*1024)
    initial=hashlib.sha256(target.read_bytes()).digest()
    os.environ.setdefault('CIUKIOS_SETUP_QEMU_ACCEL','kvm')
    vm=DesktopVM([(0,target)],args.iso.resolve(),out)
    try:
        if args.case=='recovery':vm.select_recovery()
        if args.case=='safe':vm.boot_menu(2)
        vm.ready();boot_size=(640,480) if args.case=='safe' else (1280,800);vm.shot('desktop',boot_size)
        if args.case=='safe':
            vm.wait('[BOOT-SESSION] SAFE')
            vm.edges(boot_size)
            vm.dos();vm.command('echo safe ready','safe ready\r\nCiukiOS SHELL')
            vm.desktop();vm.shot('safe-return',boot_size);vm.pointer()
            vm.close()  # flush QEMU's empty WAV header before inspecting it
            from analyze_audio_wav import pcm_payload
            pcm,_=pcm_payload((out/'audio.wav').read_bytes())
            assert not any(pcm),'safe boot emitted startup audio'
            print('[native-desktop] PASS safe menu, VGA, silent boot, PS/2 and DOS return',flush=True)
            return
        if args.case=='recovery':
            vm.dos();vm.command('echo recovery ready','recovery ready\r\nCiukiOS SHELL')
            vm.desktop();vm.shot('recovery-desktop',boot_size)
            print('[native-desktop] PASS recovery menu, uncompressed RAM boot, DOS, desktop',flush=True)
            return
        if args.case in ('doom','costa','files'):
            # Select and open a real application from the graphical library.
            if args.case=='files':
                vm.key('right');offset=vm.offset();vm.key('ret')
                vm.wait('[DESKTOP] RUN DOSNAV',offset)
                time.sleep(10);vm.shot('files')
                vm.key('tab');time.sleep(.5);vm.shot('files-other-panel')
                vm.key('alt-x');time.sleep(.5);vm.key('ret')
                vm.ready(offset);vm.pointer();vm.shot('files-return',boot_size)
            elif args.case=='costa':
                vm.key('down');offset=vm.offset();vm.key('ret')
                vm.wait('[DESKTOP] RUN COSTA',offset)
                time.sleep(10)
                a=vm.shot('costa',(640,350)).read_bytes().split(maxsplit=4)[4]
                vm.move(40,20);time.sleep(1)
                b=vm.shot('costa-mouse',(640,350)).read_bytes().split(maxsplit=4)[4]
                changed=sum(a[i:i+3]!=b[i:i+3] for i in range(0,len(a),3))
                assert 20<=changed<=128,f'Costa pointer redraw changed {changed} pixels'
                # Costa buttons use their underlined letter without Alt.
                vm.key('x');time.sleep(1);vm.key('y');time.sleep(1)
                vm.shot('costa-exit')
                vm.ready(offset);vm.pointer()
            else:
                wx,wy,_=vm.library_rect()
                vm.click_at(wx+175,wy+50)
                offset=vm.offset();vm.key('ret')
                vm.wait('[DESKTOP] RUN DOOM',offset)
                vm.wait('ST_Init: Init status bar.',offset,90)
                deadline=time.monotonic()+90
                while True:
                    title=Image.open(vm.shot('doom-title'))
                    sample=list(title.get_flattened_data())[::16]
                    if title.size in ((320,200),(640,400)) and len(set(sample))>32:
                        break
                    assert time.monotonic()<deadline,'Doom title did not finish loading'
                    time.sleep(1)
                def game_key(key):
                    vm.hmp(f'sendkey {key} 150');time.sleep(.4)
                def menu_visible():
                    a=np.array(Image.open(vm.shot('doom-menu-probe')).resize((640,400)))
                    red=(a[:,:,0]>110)&(a[:,:,1]<50)&(a[:,:,2]<50)
                    return all(red[y:y+32,185:500].sum()>1100 for y in (120,152,184,216,248,280))
                for attempt in range(6):
                    if menu_visible():break
                    game_key('esc');time.sleep(2)
                    vm.shot(f'doom-menu-attempt-{attempt}')
                assert menu_visible(),'Doom did not accept Escape at the title'
                a=np.array(Image.open(vm.shot('doom-menu')).resize((640,400)))
                time.sleep(12);game_key('down');time.sleep(1)
                b=np.array(Image.open(vm.shot('doom-menu-moved')).resize((640,400)))
                assert np.any(a[120:330,125:185]!=b[120:330,125:185]),'Doom menu cursor did not move'
                game_key('q');game_key('ret');time.sleep(2)
                vm.shot('doom-quit');game_key('y')
                vm.ready(offset);vm.shot('doom-return',boot_size);vm.pointer()
            print(f'[native-desktop] PASS {args.case} launch, interaction, return and desktop mouse',flush=True)
            return
        # Cursor movement changes only the old and new pointer footprints.
        a=np.array(Image.open(vm.shot('before-mouse')))
        p=vm.pointer();vm.move(16,-11);assert vm.pointer()!=p
        b=np.array(Image.open(vm.shot('after-mouse')))
        assert 20<=np.any(a!=b,axis=2).sum()<600,'mouse damaged desktop pixels'
        wx,wy,ww=vm.library_rect()
        # Minimize and restore, then maximize and restore through real clicks.
        vm.click_at(wx+ww-64,wy+15)
        vm.shot('minimized',boot_size)
        try:vm.library_rect()
        except AssertionError:pass
        else:raise AssertionError('minimize did not hide the library')
        vm.click_at(75,boot_size[1]-19)
        assert vm.library_rect()==(wx,wy,ww)
        vm.click_at(wx+ww-40,wy+15)
        assert vm.library_rect()==(8,34,boot_size[0]-16),'maximize did not resize'
        vm.shot('maximized',boot_size)
        vm.click_at(boot_size[0]-48,34+15)
        assert vm.library_rect()==(wx,wy,ww),'restore lost geometry'
        # Drag the title. Validate actual geometry rather than a debug marker.
        vm.position(wx+170,wy+13);vm.hmp('mouse_button 1');time.sleep(.2)
        vm.move(-10,-6);vm.hmp('mouse_button 0');time.sleep(.5)
        nx,ny,nw=vm.library_rect()
        assert nx<wx and ny<wy and nw==ww,'title drag did not move the window'
        vm.shot('dragged')
        vm.position(nx+nw-7,ny+370-7);vm.hmp('mouse_button 1');time.sleep(.2)
        vm.move(8,8);vm.hmp('mouse_button 0');time.sleep(.5)
        assert vm.library_rect()[2]>nw,'resize grip did not change width'
        vm.shot('resized')
        vm.edges(boot_size)
        print('[native-desktop] PASS cursor save/restore, minimize, maximize, drag, resize',flush=True)
        vm.repaint_key('f1');vm.shot('about');vm.repaint_key('esc')
        vm.repaint_key('f3')
        # Insertion, Home/End and Delete in Run must affect the executed command.
        for ch in 'echo RUM READY':
            vm.key('spc' if ch==' ' else 'shift-'+ch.lower() if ch.isupper() else ch)
        vm.key('home')
        for _ in range(7):vm.key('right')
        vm.key('delete');vm.key('shift-n');vm.key('end');vm.key('ret')
        vm.wait('\r\nRUN READY\r\n',timeout=30)
        vm.wait('Press any key to return to the desktop.')
        vm.shot('run-output');offset=vm.offset();vm.key('spc');vm.ready(offset)
        vm.dos();vm.command('echo DOS READY','DOS READY\r\nCiukiOS SHELL')
        vm.command('mem','Largest free DOS block:')
        vm.command('mkdir UIGUI')
        vm.desktop()
        assert vm.library_rect()[2]>nw,'DOS return lost window size'
        vm.dos();vm.command('dir UIGUI')
        vm.command('rmdir UIGUI')
        vm.command('nonexist')
        vm.desktop();vm.pointer()
        # A previous DOS error must not reopen a graphical error dialog.
        vm.repaint_key('f3');vm.shot('run-dialog')
        vm.repaint_key('esc');vm.repaint_key('f10');vm.shot('power');vm.repaint_key('esc')
        print('[native-desktop] PASS Run output, DOS commands, directory persistence, EXIT',flush=True)
        # DOS keeps the requested low/text profile, while the normal desktop
        # has required at least 800x600 since the redesign. SET SAFE saves TEXT;
        # the separate --case safe tests the explicitly forced VGA boot path.
        profiles=(('640',(640,480),boot_size,'0640'),
                  ('800',(800,600),(800,600),'0800'),
                  ('1024',(1024,768),(1024,768),'1024'),
                  ('safe',(720,400),boot_size,'TEXT'))
        for profile,dos_size,size,saved in profiles:
            vm.dos();vm.command(f'vgasetup set {profile}','Shared resolution saved',60)
            vm.shot(f'dos-{profile}',dos_size)
            vm.command(r'type \SYSTEM\VIDEO\DISPLAY.CFG',saved+'\r\nCiukiOS SHELL')
            vm.desktop();vm.shot(f'desktop-{profile}',size);vm.pointer();vm.edges(size)
            vm.repaint_key('f1');vm.shot(f'about-{profile}',size);vm.repaint_key('esc')
        print('[native-desktop] PASS DOS 640/text profiles, desktop SVGA minimum, shared 800/1024 and mouse',flush=True)
    except Exception:
        vm.shot('failure');(out/'registers.log').write_bytes(vm.hmp('info registers'))
        (out/'pic.log').write_bytes(vm.hmp('info pic'))
        vm.hmp(f'memsave 0x2d0000 0x10000 "{out}/doom-code.bin"')
        raise
    finally:
        vm.close()
        assert hashlib.sha256(target.read_bytes()).digest()==initial,'desktop changed the host disk'


if __name__=='__main__':main()
