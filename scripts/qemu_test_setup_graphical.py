#!/usr/bin/env python3
"""Test the graphical installer against disposable disks, including data effects."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import shutil
import struct
import subprocess
import tempfile
import time
import uuid
from PIL import Image
from qemu_test_full_display_profile import VM


class SetupVM(VM):
    def __init__(self, disks, iso, output, fault=False, qemu_args=(), allow_reboot=False):
        self.output = output
        output.mkdir(parents=True, exist_ok=True)
        self.serial = output / 'serial.log'
        self.sock = Path(tempfile.gettempdir()) / f'ciukios-setup-{uuid.uuid4().hex}.sock'
        self.err = (output / 'qemu.stderr.log').open('w')
        drives = []
        for index, disk in disks:
            if fault:
                config = output / 'fault.conf'
                # Surface/data read failure, well past the source/MBR preflight.
                config.write_text('[inject-error]\nevent = "read_aio"\nerrno = "5"\nsector = "1000"\nonce = "off"\nimmediately = "on"\n')
                backend = f'blkdebug:{config}:{disk}'
            else:
                backend = str(disk)
            drives += ['-drive', f'file={backend},format=raw,if=ide,index={index}']
        self.process = subprocess.Popen([
            'qemu-system-i386', '-accel', os.environ.get('CIUKIOS_SETUP_QEMU_ACCEL','tcg'), '-machine', 'pc,vmport=off,i8042=on',
            '-cpu', 'pentium3', '-m', '512', *drives,
            '-drive', f'file={iso},format=raw,if=ide,index=2,media=cdrom,readonly=on',
            '-boot', 'd', '-display', 'none', '-serial', f'file:{self.serial}',
            '-monitor', f'unix:{self.sock},server,nowait', '-no-shutdown',
            *([] if allow_reboot else ['-no-reboot']),
            '-qmp', f'unix:{output}/qmp.sock,server=on,wait=off',
            *qemu_args,
        ], stdout=subprocess.DEVNULL, stderr=self.err)
        deadline = time.monotonic() + 10
        while not self.sock.exists():
            assert self.process.poll() is None, (output / 'qemu.stderr.log').read_text()
            assert time.monotonic() < deadline, 'monitor did not start'
            time.sleep(.05)

    def page(self, n, offset=0, timeout=60):
        self.wait(f'[SETUP-GUI] PAGE {n:02X}', offset, timeout)
        time.sleep(.7)

    def boot_menu(self, index):
        deadline=time.monotonic()+15
        dump=self.output/'boot-menu.bin'
        while time.monotonic()<deadline:
            self.hmp(f'pmemsave 0xb8000 4000 "{dump}"')
            if dump.exists():
                text=dump.read_bytes()[::2].decode('cp437',errors='replace')
                if 'CiukiOS - Setup' in text and 'Booting' not in text:
                    self.key('up')  # stop the timeout, then explicitly select
                    self.key('home')
                    for _ in range(index):self.key('down')
                    self.shot('boot-menu')
                    self.key('ret')
                    return
            time.sleep(.08)
        raise AssertionError('GRUB Live/Setup menu was not offered')

    def shot(self, name, size=None):
        path = super().shot(name, size)
        Image.open(path).save(path.with_suffix('.png'))
        return path

    def next(self, n):
        if n in (1,2,3):
            check_navigation_frames(self, self.output, n)
        else:
            offset = self.offset()
            self.key('ret')
            self.page(n, offset)

    def move(self, dx, dy):
        # QEMU relative PS/2 deltas, bypass any tablet/absolute pointer.
        self.hmp(f'mouse_move {dx} {dy} 0')
        time.sleep(.3)

    # Arrow ink/fill for the legacy palette and for the 2026-09-25 platinum
    # theme, which Setup shares with the desktop (ui_theme.inc). Matching only
    # the legacy pair failed on the unchanged pre-LFB ISO too, with an intact
    # platinum arrow on screen.
    CURSOR_PALETTES = (((48,61,73),(247,247,239)), ((36,40,48),(246,246,242)))

    def pointer(self):
        import numpy as np
        pixels = np.array(Image.open(self.shot('pointer')))
        for ink, fill in self.CURSOR_PALETTES:
            coords = self._pointer_coords(pixels, ink, fill)
            if len(coords) == 1:
                break
        assert len(coords) == 1, f'expected one cursor, found {len(coords)}'
        y,x = coords[0]
        return int(x),int(y)

    @staticmethod
    def _pointer_coords(pixels, ink, fill):
        import numpy as np
        black = (np.abs(pixels.astype(int) - ink) <= 4).all(axis=2)
        white = (np.abs(pixels.astype(int) - fill) <= 4).all(axis=2)
        # Match opaque pixels of the private arrow; ignore the background.
        bm = (0x8000,0xC000,0xE000,0xF000,0xF800,0xFC00,0xFE00,0xFF00,
              0xFF80,0xFFC0,0xFE00,0xEF00,0xCF00,0x8780,0x0780,0x0300)
        wm = (0,0,0x4000,0x6000,0x7000,0x7800,0x7C00,0x7E00,
              0x7F00,0x7C00,0x6C00,0x4600,0x0600,0x0300,0x0300,0)
        height,width = pixels.shape[:2]
        rows,cols = height-15,width-15
        valid = np.ones((rows,cols), dtype=bool)
        for y in range(16):
            for x in range(16):
                bit = 1 << (15-x)
                if wm[y] & bit:
                    valid &= white[y:y+rows,x:x+cols]
                elif bm[y] & bit:
                    valid &= black[y:y+rows,x:x+cols]
        return np.argwhere(valid)

    def click_at(self, x, y):
        for _ in range(24):
            px,py = self.pointer()
            if abs(px-x) <= 5 and abs(py-y) <= 5:
                self.click()
                return
            dx,dy = x-px,y-py
            self.move(max(-30,min(30,round(dx/2))),max(-30,min(30,round(dy/2))))
        raise AssertionError(f'could not position pointer at {(x,y)}; got {self.pointer()}')

    def click(self):
        self.hmp('mouse_button 1')
        time.sleep(.12)
        self.hmp('mouse_button 0')
        time.sleep(.5)


def check_focus_frames(vm, out):
    import numpy as np
    base=np.array(Image.open(vm.shot('before-focus',(800,600))))
    px,py=vm.pointer()
    off=vm.offset();vm.hmp('sendkey tab 30')
    frames=[np.array(Image.open(vm.shot(f'focus-frame-{n:02d}',(800,600)))) for n in range(16)]
    vm.wait('[SETUP-GUI] PAGE 00',off,30)
    # Only Next/Cancel focus borders and the temporarily hidden cursor can change.
    stable=np.ones((600,800),bool)
    stable[456:486,370:660]=False
    stable[py:py+16,px:px+24]=False
    for frame in frames:
        assert np.array_equal(base[stable],frame[stable]), 'setup flashed outside the focus controls'
    after=np.array(Image.open(vm.shot('after-focus',(800,600))))
    assert np.any(base[456:486,370:660]!=after[456:486,370:660]), 'focus never visibly changed'
    (out/'focus-frames.json').write_text(json.dumps({'frames':len(frames), 'distinct_frames':len({f.tobytes() for f in frames}), 'unchanged_background':True},indent=2)+'\n')
    # Welcome has two enabled controls: Next and Cancel. Back is disabled
    # and no longer participates in focus traversal. A second Tab here would
    # select Cancel again, so the following Enter would correctly exit Setup.
    vm.key('tab');time.sleep(.5)
    restored=np.array(Image.open(vm.shot('focus-restored',(800,600))))
    assert np.array_equal(restored[456:486,370:660],base[456:486,370:660]), 'Tab did not restore Next focus'
    check_radio_frames(vm,out)


def check_radio_frames(vm,out):
    import numpy as np
    base=np.array(Image.open(vm.shot('before-radio',(800,600))));px,py=vm.pointer()
    off=vm.offset();vm.hmp('sendkey down 30')
    frames=[np.array(Image.open(vm.shot(f'radio-frame-{n:02d}',(800,600)))) for n in range(16)]
    vm.page(0,off)
    stable=np.ones((600,800),bool)
    stable[253:265,293:305]=False;stable[315:327,293:305]=False
    stable[py:py+16,px:px+24]=False
    for f in frames:assert np.array_equal(f[stable],base[stable]), 'radio selection flashed outside its controls'
    after=np.array(Image.open(vm.shot('after-radio',(800,600))))
    assert np.any(base[315:327,293:305]!=after[315:327,293:305]), 'radio selection did not change'
    (out/'radio-frames.json').write_text(json.dumps({'frames':16,'distinct_frames':len({f.tobytes() for f in frames}), 'unchanged_background':True},indent=2)+'\n')
    off=vm.offset();vm.key('up');vm.page(0,off)


def check_navigation_frames(vm,out,page):
    import numpy as np
    base=np.array(Image.open(vm.shot(f'before-page-{page}',(800,600))));px,py=vm.pointer()
    off=vm.offset();vm.hmp('sendkey ret 30')
    frames=[np.array(Image.open(vm.shot(f'page-{page}-frame-{n:02d}',(800,600)))) for n in range(16)]
    vm.page(page,off)
    stable=np.ones((600,800),bool)
    stable[137:438,280:670]=False # wizard content
    stable[280:405,132:145]=False # current step marker
    stable[456:486,370:660]=False # navigation enabled/focus state
    stable[py:py+16,px:px+24]=False
    for f in frames:assert np.array_equal(f[stable],base[stable]), 'page navigation flashed fixed setup chrome'
    (out/f'page-{page}-frames.json').write_text(json.dumps({'frames':16,'distinct_frames':len({f.tobytes() for f in frames}), 'unchanged_chrome':True},indent=2)+'\n')


def check_guest_restart(vm, out):
    # The guest button is the sole reset trigger. Ejecting the virtual CD is
    # equivalent to the removal explicitly requested by the final setup page.
    with socket.socket(socket.AF_UNIX) as conn:
        conn.settimeout(30);conn.connect(str(out/'qmp.sock'))
        stream=conn.makefile('rwb',buffering=0)
        assert 'QMP' in json.loads(stream.readline())
        stream.write(b'{"execute":"qmp_capabilities"}\n')
        while 'return' not in json.loads(stream.readline()): pass
        vm.hmp('eject -f ide1-cd0')
        vm.hmp('boot_set c')
        off=vm.offset()
        vm.click_at(600,469)
        events=[];deadline=time.monotonic()+30
        while time.monotonic()<deadline:
            event=json.loads(stream.readline());events.append(event)
            if event.get('event')=='RESET' and event.get('data',{}).get('guest') is True:break
        else:raise AssertionError('Restart did not cause a guest hardware RESET')
        (out/'reset-events.json').write_text(json.dumps(events,indent=2)+'\n')
    vm.wait('[SETUP] HARDWARE RESET',off,30)
    vm.wait('[BOOT0-FULL] CiukiOS full stage0 ready',off,90)
    vm.wait('[DESKTOP] READY',off,90)
    vm.shot('desktop-after-hardware-reset')
    vm._entered_dos=False
    vm.wait('CiukiOS SHELL C:\\APPS>',off,90)
    vm.command('dir \\sbemu','VSBHDA.EXE')
    vm.command('comdemo.com','COM demo via INT21h')
    print('[setup-gui] PASS Restart caused guest hardware RESET; same VM booted HDD without CD and executed DOS',flush=True)


def digest(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def fill_disk(path, mib):
    # Distinct nonzero old data proves the difference between quick and full.
    with path.open('wb') as f:
        for _ in range(mib):
            f.write(bytes([0xA5]) * 1024 * 1024)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--iso', type=Path, default=Path('build/full/CiukiOS_full_cd_0-7-1.iso'))
    ap.add_argument('--source', type=Path, default=Path('build/full/ciukios-full-cd-disk.img'))
    ap.add_argument('--case', choices=('preview','cancel','quick','full','install','small','no-disk','io-error','interrupt','multiple','mouse'), required=True)
    ap.add_argument('--full-install', action='store_true')
    ap.add_argument('--reboot', action='store_true', help='Click Restart, observe a guest RESET event and boot the installed disk in the same VM.')
    ap.add_argument('--expect-grub4dos', action='store_true')
    ap.add_argument('--boot-setup', action='store_true')
    ap.add_argument('--reinstall-from', type=Path, help='Copy an existing 128 MiB qualification HDD as the disposable installation target.')
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    if args.reboot and args.case != 'install': ap.error('--reboot requires --case install')
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    disk = out / 'target.img'
    if args.reinstall_from:
        assert args.case == 'install', '--reinstall-from requires --case install'
        assert args.reinstall_from.stat().st_size == 128*1024*1024
        shutil.copyfile(args.reinstall_from, disk)
    else:
        fill_disk(disk, 32 if args.case == 'small' else 128)
    before = digest(disk)
    disks = [] if args.case == 'no-disk' else [(0, disk)]
    second = None
    if args.case == 'multiple':
        second = out / 'other.img'
        fill_disk(second, 144)
        disks += [(1, second)]
        other_before = digest(second)
    vm = SetupVM(disks, args.iso.resolve(), out, fault=args.case == 'io-error', allow_reboot=args.reboot)
    try:
        if args.boot_setup:
            vm.boot_menu(1)
            vm.wait('[BOOT-SESSION] SETUP',timeout=90)
            assert '[DESKTOP] READY' not in vm.serial.read_text(errors='replace')
        else:
            vm.wait('CiukiOS SHELL D:\\APPS>', timeout=90)
            vm.shot('shell')
            vm.text('setup')
        vm.page(0)
        vm.shot('welcome',(800,600))
        check_focus_frames(vm, out)
        if args.case == 'mouse':
            from qemu_test_native_desktop import DesktopVM
            DesktopVM.edges(vm,(800,600))
            # Real PS/2 movement and clicks through every pre-write page.
            initial=vm.pointer()
            vm.move(-12,-8)
            assert vm.pointer()!=initial, 'cursor did not move'
            vm.click_at(500,469);vm.page(1)
            vm.click_at(500,469);vm.page(2)
            vm.click_at(320,319);time.sleep(.7)
            vm.shot('mouse-full')
            vm.click_at(500,469);vm.page(3)
            vm.click_at(298,386);time.sleep(.7)
            vm.shot('mouse-confirmed')
            vm.click_at(610,469)
            vm.wait('CiukiOS SHELL D:\\APPS>',vm.offset()-100,20)
            offset=vm.offset();vm.text('mouse info');vm.wait('info ver=',offset,20)
            vm.wait('CiukiOS SHELL D:\\APPS>',offset,30)
            offset=vm.offset();vm.text('echo mouse returned');vm.wait('mouse returned\r\nCiukiOS SHELL',offset,30)
            vm.shot('mouse-returned')
            vm.text('costa');time.sleep(8)
            a=vm.shot('costa-after-setup',(640,350)).read_bytes().split(maxsplit=4)[4]
            vm.move(80,40);time.sleep(1)
            b=vm.shot('costa-mouse',(640,350)).read_bytes().split(maxsplit=4)[4]
            changed=sum(a[i:i+3]!=b[i:i+3] for i in range(0,len(a),3))
            assert 20 <= changed <= 128, f'Costa pointer did not move cleanly: {changed}'
            vm.key('tab');vm.key('tab');vm.key('ret');time.sleep(2);vm.key('ret');time.sleep(6)
            c=vm.shot('costa-calculator',(640,350)).read_bytes().split(maxsplit=4)[4]
            assert sum(b[i:i+3]!=c[i:i+3] for i in range(0,len(b),3)) > 10000
            print('[setup-gui] PASS PS/2 cursor, clicks, radio, checkbox, cancel, Costa mouse and Calculator',flush=True)
            return
        if args.case in ('quick','full','io-error','interrupt','multiple'):
            vm.key('down')
            time.sleep(.6)
        vm.next(1)
        vm.shot('destination',(800,600))
        if args.case == 'no-disk':
            vm.key('ret');time.sleep(1)
            assert '[SETUP-GUI] PAGE 02' not in vm.serial.read_text(errors='replace')
            vm.key('esc')
            vm.wait('CiukiOS SHELL D:\\APPS>', vm.offset()-100, 20)
            print('[setup-gui] PASS no target: no writing path',flush=True)
            return
        if args.case == 'multiple':
            vm.key('down');time.sleep(.7)
        if args.case == 'small':
            vm.next(6)
            vm.shot('too-small',(800,600))
            vm.key('esc')
            vm.wait('CiukiOS SHELL D:\\APPS>', vm.offset()-100, 20)
            print('[setup-gui] PASS small target rejected before writes',flush=True)
            return
        vm.next(2)
        vm.shot('format',(800,600))
        if args.case in ('full','io-error','interrupt') or args.full_install:
            vm.key('down');time.sleep(.7)
            vm.shot('full-format',(800,600))
        vm.next(3)
        vm.shot('review',(800,600))
        # Enter must not start before the destructive checkbox is selected.
        offset = vm.offset()
        vm.key('ret');time.sleep(.5)
        assert '[SETUP-GUI] PAGE 04' not in vm.serial.read_text(errors='replace')
        if args.case in ('preview','cancel'):
            # Back navigation invalidates the prior confirmation.
            vm.key('spc');vm.key('b');vm.page(2,offset)
            vm.next(3)
            vm.key('ret');time.sleep(.5)
            assert '[SETUP-GUI] PAGE 04' not in vm.serial.read_text(errors='replace')
            vm.key('esc');vm.wait('CiukiOS SHELL D:\\APPS>',vm.offset()-100,20)
            vm.text('echo setup returned')
            vm.wait('setup returned\r\nCiukiOS SHELL',offset,20)
            vm.shot('returned')
            print('[setup-gui] PASS navigation, confirmation, cancel and shell return',flush=True)
            return
        vm.key('spc');time.sleep(.6)
        vm.next(4)
        vm.shot('progress',(800,600))
        if args.case == 'interrupt':
            vm.key('esc');vm.page(6,timeout=120)
            vm.shot('cancelled',(800,600))
            vm.wait('[SETUP-GUI] CANCELLED')
            return
        if args.case == 'io-error':
            vm.page(6,timeout=180)
            vm.shot('io-error',(800,600))
            return
        vm.page(5,timeout=2400 if args.full_install else 1800)
        vm.shot('complete',(800,600))
        print('[setup-gui] PASS completed operation',flush=True)
        if args.reboot:
            shutil.copyfile(disk, out/'installed-before-reboot.img')
            check_guest_restart(vm, out)
    finally:
        vm.shot('final')
        vm.close()
        if args.case in ('preview','cancel','small','no-disk','mouse'):
            assert digest(disk) == before, 'target changed before confirmation'
        if args.case in ('io-error','interrupt'):
            with disk.open('rb') as f:
                assert f.read(512) == bytes(512), 'target MBR was not invalidated'
            assert digest(disk) != before, 'error test never reached a disk write'
            print('[setup-gui] PASS incomplete target has no valid MBR',flush=True)
    if args.case in ('preview','cancel','small','no-disk','io-error','interrupt'):
        return
    target = second if args.case == 'multiple' else disk
    if second:
        assert digest(disk) == before, 'installer wrote the unselected disk'
        assert digest(second) != other_before, 'selected disk was not formatted'
    data = (out/'installed-before-reboot.img' if args.reboot else target).read_bytes()
    mbr=data[:512]
    assert mbr[510:] == b'\x55\xaa'
    start,count=struct.unpack_from('<II',mbr,454)
    source_bytes=args.source.stat().st_size
    expected_count=source_bytes//512-63
    assert (start,count)==(63,expected_count),(start,count)
    assert data[source_bytes:]==bytes([0xA5])*(len(data)-source_bytes),'wrote past displayed partition'
    vbr=data[start*512:(start+1)*512]
    assert struct.unpack_from('<H',vbr,11)[0]==512
    assert vbr[13]==8 and vbr[16]==2
    reserved=struct.unpack_from('<H',vbr,14)[0]
    fat_size=struct.unpack_from('<H',vbr,22)[0]
    fat1=(start+reserved)*512
    assert data[fat1:fat1+fat_size*512]==data[fat1+fat_size*512:fat1+2*fat_size*512]
    data_start=(start+reserved+2*fat_size+32)*512
    if args.case in ('quick','multiple'):
        assert data[data_start:source_bytes]==bytes([0xA5])*(source_bytes-data_start),'quick format cleared file data'
    if args.case == 'full':
        assert data[data_start:source_bytes]==bytes(source_bytes-data_start),'full format left old data'
    if args.case in ('quick','full','multiple'):
        assert mbr[446]==0, 'empty format advertised an OS as bootable'
        subprocess.run(['mdir','-i',f'{target}@@32256','::'],check=True)
        print('[setup-gui] PASS FAT16 geometry, FAT copies, data coverage and write bounds',flush=True)
    if args.case == 'install':
        assert '[SETUP-VERIFY] ALL SECTORS CRC MATCH' in (out/'serial.log').read_text(errors='replace'), 'post-flush BIOS readback did not complete'
        assert mbr[446]==0x80
        source=args.source.read_bytes()
        diffs=[i for i,(a,b) in enumerate(zip(source,data)) if a!=b]
        # GRUB4DOS assigns the RAM disk an MBR disk signature of 0x80 when
        # the source signature is zero (upstream builtins.c map_func). It is
        # an identity field, not filesystem data; validate it byte-for-byte.
        grub_identity = (source[440:444] == bytes(4)
                         and data[440:444] == b'\x80\0\0\0')
        if args.expect_grub4dos:
            assert grub_identity, 'expected the GRUB4DOS RAM image, not fallback'
        actual_diffs = [i for i in diffs if not (grub_identity and i == 440)]
        assert len(actual_diffs)==1 and source[actual_diffs[0]]==3 and data[actual_diffs[0]]==2, f'unexpected installed bytes: {diffs[:20]}'
        print(f'[setup-gui] PASS every installed sector matches source; D: -> C: patch; GRUB4DOS identity={grub_identity}',flush=True)
        boot=out/'boot';boot.mkdir(exist_ok=True)
        hdd=VM(target,boot)
        try:
            if args.expect_grub4dos:
                hdd.wait('[DESKTOP] READY',timeout=60)
                hdd.shot('installed-desktop',(1280,800))
            hdd.wait('CiukiOS SHELL C:\\APPS>',timeout=60)
            hdd.command('echo installed keyboard ready','installed keyboard ready\r\nCiukiOS SHELL')
            hdd.command('help files','FILES AND FOLDERS')
            hdd.shot('installed')
            print('[setup-gui] PASS installed HDD boots alone and accepts shell commands',flush=True)
        finally:
            hdd.close()


if __name__=='__main__':
    main()
