#!/usr/bin/env python3
"""Validate the actual installed FAT16 HDD without a CD or resident GRUB mapper."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import time
import threading

import numpy as np
from PIL import Image

from analyze_audio_wav import pcm_payload
from doom_screen import menu_skulls, selected_row
from qemu_test_full_display_profile import VM
from qemu_test_native_desktop import DesktopVM


class FAT16:
    """Independent read-only image parser; no guest implementation is imported."""
    def __init__(self, path):
        self.data = path.read_bytes()
        self.start = (0 if self.data[54:62] == b'FAT16   '
                      and struct.unpack_from('<H', self.data, 11)[0] == 512
                      else struct.unpack_from('<I', self.data, 454)[0] * 512)
        b = self.data[self.start:self.start + 512]
        self.bps = struct.unpack_from('<H', b, 11)[0]
        assert self.bps == 512
        self.spc = b[13]
        reserved, = struct.unpack_from('<H', b, 14)
        roots, = struct.unpack_from('<H', b, 17)
        spf, = struct.unpack_from('<H', b, 22)
        self.fat = self.start + reserved * self.bps
        self.root = self.fat + b[16] * spf * self.bps
        self.root_size = roots * 32
        self.clusters = self.root + ((self.root_size + 511) // 512) * 512

    def chain(self, cluster):
        seen = set()
        while 2 <= cluster < 0xfff8:
            assert cluster not in seen, 'FAT cycle'
            seen.add(cluster)
            offset = self.clusters + (cluster - 2) * self.spc * self.bps
            yield self.data[offset:offset + self.spc * self.bps]
            cluster, = struct.unpack_from('<H', self.data, self.fat + cluster * 2)

    def entries(self, cluster=0):
        raw = (self.data[self.root:self.root + self.root_size] if not cluster
               else b''.join(self.chain(cluster)))
        result = {}
        for offset in range(0, len(raw), 32):
            e = raw[offset:offset + 32]
            if e[0] == 0:
                break
            if e[0] == 0xe5 or e[11] == 0x0f or e[11] & 8:
                continue
            name = e[:8].decode('cp437').rstrip()
            ext = e[8:11].decode('cp437').rstrip()
            if ext:
                name += '.' + ext
            result[name] = (e[11], struct.unpack_from('<H', e, 26)[0],
                            struct.unpack_from('<I', e, 28)[0])
        return result

    def entry(self, path):
        entry = (16, 0, 0)
        for part in path.replace('\\', '/').strip('/').upper().split('/'):
            if part:
                entry = self.entries(entry[1])[part]
        return entry

    def read(self, path):
        _, cluster, size = self.entry(path)
        return b''.join(self.chain(cluster))[:size]


class InstalledVM(VM):
    auto_enter_dos = False
    _pointer = DesktopVM._pointer
    pointer = DesktopVM.pointer

    def __init__(self, disk, output, vga='std', audio='ac97', memory=512):
        devices = ['-device', 'AC97,audiodev=snd'] if audio == 'ac97' else [
            '-device', 'sb16,iobase=0x220,irq=7,dma=1,dma16=5,audiodev=snd',
            '-device', 'adlib,audiodev=snd']
        super().__init__(disk, output, qemu_args=[
            '-m', str(memory),
            '-vga', vga,
            '-audiodev', f'wav,id=snd,path={output}/audio.wav',
            *devices])

    def shot(self, name, size=None):
        path = super().shot(name, size)
        Image.open(path).save(path.with_suffix('.png'))
        return path

    def result(self, command, *expected, timeout=60, prompt='CiukiOS SHELL C:\\APPS>'):
        offset = self.offset()
        self.text(command)
        self.wait(prompt, offset, timeout)
        text = subprocess.check_output([
            'scripts/serial_log_normalize.py', '--offset', str(offset), str(self.serial)])
        body = text.partition(b'\r\n')[2].decode('cp437')
        for expected_text in expected:
            assert expected_text in body, (command, expected_text, body)
        for error in ('cannot execute', 'not found', 'EXEC FAIL', 'WOOF', '[FILECHECK] ERROR='):
            assert error not in body, (command, body)
        return body


def fnv(data):
    value = 0x811c9dc5
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def listing_address(lines, name):
    for index, line in enumerate(lines):
        if re.search(r'\b' + re.escape(name) + r'(?=:|\s+d[bwd]\b)', line):
            for following in lines[index:]:
                match = re.match(r'\s*\d+\s+([0-9A-F]{8})\s', following)
                if match:
                    return int(match[1], 16)
    raise AssertionError(f'Kernel listing has no {name}')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--image', type=Path, default=Path(
        'build/full/t23-runtime-repair-2026-09-06/final-install/target.img'))
    ap.add_argument('--output', required=True, type=Path)
    ap.add_argument('--case', choices=('filesystem', 'paths', 'video', 'video-menu', 'games', 'classic-doom', 'legacy-apps', 'bios-fallback'), default='filesystem')
    ap.add_argument('--kernel', type=Path, help='Candidate CIUKIDOS.SYS; installed stage1 supplies drive C.')
    ap.add_argument('--kernel-listing', type=Path,
                    help='Matching NASM listing for RAM code verification without replacing the installed kernel.')
    ap.add_argument('--shell', type=Path, help='Candidate native SHELL.COM for isolated video qualification.')
    ap.add_argument('--doom-launcher', type=Path, help='Candidate DOOM.COM for isolated original-engine qualification.')
    ap.add_argument('--doom-launcher-mz', type=Path, help='Candidate DOOM.EXE, used by the shell Doom shortcut.')
    ap.add_argument('--expect-pvi', action='store_true', help='Require CR4.PVI during classic Doom and its restoration after exit.')
    ap.add_argument('--keep-display-profile', action='store_true', help='Exercise directory and EXEC paths through the installed graphical console.')
    ap.add_argument('--vga', choices=('std', 'cirrus'), default='std', help='Emulated VGA BIOS/device for hardware-path comparison.')
    ap.add_argument('--audio', choices=('ac97','sb16'), default='ac97')
    ap.add_argument('--ui-palette', choices=('classic', 'platinum'), default='classic',
                    help='Pointer colours of the actual shell under test; preserves legacy baseline defaults.')
    ap.add_argument('--memory', type=int, default=512)
    ap.add_argument('--finish-windows', action='store_true', help='After the games, start Windows and return in the same VM.')
    ap.add_argument('--direct-dos', action='store_true', help='Control fixture: supplied SHELL boots directly to DOS, skipping the UI startup.')
    ap.add_argument('--pointer-activity', action='store_true', help='Deliver real PS/2 motion concurrently with guest file and EXEC operations.')
    ap.add_argument('--disk-fixture', type=Path, help='Preserved earlier INT13 fault fixture for allocation isolation.')
    ap.add_argument('--disk-padding-paras', type=int, default=0,
                    help='Extra resident paragraphs in the BIOS fixture; varies subsequent MZ alignment.')
    args = ap.parse_args()
    if not 0 <= args.disk_padding_paras <= 256:
        ap.error('--disk-padding-paras must be between 0 and 256')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    disk = out / 'installed.img'
    shutil.copyfile(args.image, disk)
    before_hash = hashlib.sha256(args.image.read_bytes()).hexdigest()
    source = FAT16(disk)
    volume = str(disk) + '@@' + str(source.start)
    if args.kernel:
        subprocess.run(['mcopy', '-o', '-i', volume, str(args.kernel),
                        '::SYSTEM/CIUKIDOS.SYS'], check=True)
    if args.shell:
        subprocess.run(['mcopy', '-o', '-i', volume, str(args.shell),
                        '::SYSTEM/SHELL.COM'], check=True)
    if args.doom_launcher:
        subprocess.run(['mcopy', '-o', '-i', volume, str(args.doom_launcher),
                        '::APPS/DOOM/DOOM.COM'], check=True)
    if args.doom_launcher_mz:
        subprocess.run(['mcopy', '-o', '-i', volume, str(args.doom_launcher_mz),
                        '::APPS/DOOM/DOOM.EXE'], check=True)
    probe = out / 'FILECHK.COM'
    subprocess.run(['nasm', '-f', 'bin', 'scripts/fixtures/filecheck.asm', '-o', str(probe)], check=True)
    subprocess.run(['mcopy', '-o', '-i', volume, str(probe), '::APPS/FILECHK.COM'], check=True)
    # Bulk directory/checksum qualification isolates storage from the existing
    # expensive true-colour console renderer. The video case uses shipping AUTO.
    if not args.keep_display_profile and args.case in ('filesystem', 'paths', 'bios-fallback', 'video-menu'):
        profile = out / 'DISPLAY.CFG'
        profile.write_bytes(b'TEXT')
        subprocess.run(['mcopy', '-o', '-i', volume, str(profile),
                        '::SYSTEM/VIDEO/DISPLAY.CFG'], check=True)
    source = FAT16(disk)
    if args.case == 'bios-fallback':
        fault = out/'DISKFAIL.COM'
        if args.disk_fixture:
            shutil.copyfile(args.disk_fixture, fault)
        else:
            subprocess.run(['nasm', '-f', 'bin', 'scripts/fixtures/disk_bios_fault.asm',
                            f'-DRESIDENT_PADDING_PARAS={args.disk_padding_paras}',
                            '-o', str(fault)], check=True)
        subprocess.run(['mcopy', '-o', '-i', volume, str(fault),
                        '::APPS/DISKFAIL.COM'], check=True)
    results = {'source_sha256': before_hash, 'case': args.case, 'completed': False,
               'directories': [], 'files': []}
    results['disk_padding_paras'] = args.disk_padding_paras
    results['keep_display_profile'] = args.keep_display_profile
    results['vga'] = args.vga
    results['direct_dos'] = args.direct_dos
    results['pointer_activity'] = args.pointer_activity
    results['ui_palette'] = args.ui_palette
    if args.case == 'bios-fallback':
        fixture = fault.read_bytes()
        results['disk_fixture'] = {'sha256': hashlib.sha256(fixture).hexdigest(),
                                   'resident_paragraphs': (256 + len(fixture) + 15) // 16}
    results['payload_sha256'] = {
        path: hashlib.sha256(source.read(path)).hexdigest()
        for path in ('SYSTEM/CIUKIDOS.SYS', 'SYSTEM/SHELL.COM', 'SYSTEM/BOOTSND.COM',
                     'SYSTEM/DRIVERS/SOUND.COM', 'APPS/DOOM/DOOM.COM', 'APPS/DOOM/DOOM.EXE',
                     'APPS/DOOM/DOOMCORE.EXE', 'SBEMU/VSBHDA.EXE', 'SBEMU/HDPMI32I.EXE')}
    vm = InstalledVM(disk, out, vga=args.vga, audio=args.audio, memory=args.memory)
    if args.ui_palette == 'platinum':
        vm.cursor_colors = ((36, 40, 48), (247, 247, 243))
    motion_stop = threading.Event()
    motion_thread = None
    motion_errors = []
    motion_count = [0]
    try:
        if args.direct_dos:
            assert args.shell, '--direct-dos requires an explicit test shell'
            vm.wait('CiukiOS SHELL C:\\APPS>', timeout=90)
            vm.shot('direct-dos')
        else:
            vm.wait('[DESKTOP] READY', timeout=90)
            vm.shot('desktop')
            vm.key('f4')
            vm.wait('CiukiOS SHELL C:\\APPS>')
        results['boot_audio_end'] = (out/'audio.wav').stat().st_size
        if args.pointer_activity:
            monitor_lock = threading.Lock()
            original_hmp = vm.hmp
            def locked_hmp(command):
                with monitor_lock:
                    return original_hmp(command)
            vm.hmp = locked_hmp
            def move_pointer():
                try:
                    while not motion_stop.wait(.05):
                        delta = 17 if motion_count[0] % 2 else -17
                        vm.hmp(f'mouse_move {delta} {delta} 0')
                        motion_count[0] += 1
                except Exception as error:
                    motion_errors.append(str(error))
            motion_thread = threading.Thread(target=move_pointer, daemon=True)
            motion_thread.start()
        if args.case == 'bios-fallback':
            vm.result('diskfail', '[DISK-BIOS-FAULT] installed')
            vm.result('dir \\windows', 'WINCORE.COM', 'WIN.COM', 'SYSTEM.INI', timeout=90)
            vm.result('dir \\apps\\doomvan', 'PCDMCORE.EXE', 'DOOM.WAD', timeout=90)
            vm.result('copy mzdemo.exe CHSCOPY.EXE', 'File copied', timeout=90)
            vm.result('CHSCOPY.EXE', 'MZ demo via INT21h', timeout=90)
            vm.result('copy \\system\\boot.pcm CHSCOPY.PCM', 'File copied', timeout=180)
            vm.result('filechk CHSCOPY.PCM',
                      f'[FILECHECK] FNV={fnv(source.read("SYSTEM/BOOT.PCM")):08X}', timeout=90)
            vm.result('comdemo', 'COM demo via INT21h')
            dump = out/'bios-fault-ram.bin'
            vm.hmp(f'pmemsave 0 0x100000 "{dump}"')
            ram = dump.read_bytes()
            counters = []
            for match in re.finditer(b'DBIOSFIX', ram):
                values = struct.unpack_from('<6H' if args.disk_fixture else '<7H', ram, match.end())
                if all(values[:5]):
                    if not args.disk_fixture:
                        assert values[6] == 0, 'kernel reused BIOS-modified DAP count'
                    counters.append(values)
            assert counters, 'INT13 fixture did not exercise EDD read/write, geometry and CHS read/write'
            results['fault_counters'] = counters
            print(f'[installed-hdd] PASS actual BIOS fallback counters {counters}', flush=True)
        elif args.case == 'paths':
            for path in ('sbemu', 'windows', 'windows\\system', 'apps\\doom', 'apps\\doomvan',
                         'apps\\wolf3d', 'apps\\costa', 'apps\\dosnav'):
                prompt = 'CiukiOS SHELL C:\\' + path + '>'
                vm.result('cd \\' + path, 'Current directory: C:\\' + path, prompt=prompt)
                entries = source.entries(source.entry(path)[1])
                expected = [name + (' <DIR>' if entry[0] & 16 else '')
                            for name, entry in entries.items()]
                body = vm.result('dir', prompt=prompt)
                listing = body.split('Directory listing\r\n', 1)[1].split('CiukiOS SHELL', 1)[0]
                assert [line for line in listing.splitlines() if line] == expected, path
                if path == 'sbemu':
                    vm.shot('sbemu-directory')
                filename = next(name for name, entry in entries.items() if not entry[0] & 16)
                data = source.read(path + '\\' + filename)
                vm.result('\\APPS\\FILECHK ' + filename.lower(),
                          f'[FILECHECK] FNV={fnv(data):08X} BYTES={len(data):08X}', prompt=prompt)
                results['directories'].append({'path': path, 'entries': len(expected),
                                                'relative_file': filename})
                if path == 'sbemu':
                    for component in ('HDPMI32I.EXE', 'HDPMI16I.EXE',
                                      'VSBHDA.EXE', 'VSBHDA16.EXE'):
                        payload = source.read(path + '\\' + component)
                        vm.result('\\APPS\\FILECHK ' + component,
                                  f'[FILECHECK] FNV={fnv(payload):08X} BYTES={len(payload):08X}',
                                  prompt=prompt)
                        results['files'].append({'path': path + '/' + component,
                                                 'bytes': len(payload),
                                                 'fnv': f'{fnv(payload):08X}'})
                    vm.shot('sbemu-directory-and-file-reads')
            vm.result('cd..', 'Current directory: C:\\apps', prompt='CiukiOS SHELL C:\\apps>')
            vm.result('cd \\APPS', 'Current directory: C:\\APPS')
            vm.result('mzdemo', 'MZ demo via INT21h')
            vm.result('comdemo', 'COM demo via INT21h')
            print('[installed-hdd] PASS lowercase CD, relative DIR/file reads, CD.. and subsequent COM/MZ', flush=True)
        elif args.case == 'filesystem':
            for path in ('SBEMU', 'APPS', 'WINDOWS', 'WINDOWS/SYSTEM', 'SYSTEM/DRIVERS',
                         'APPS/DOOM', 'APPS/DOOMVAN', 'APPS/WOLF3D', 'APPS/COSTA', 'APPS/DOSNAV'):
                entries = source.entries(source.entry(path)[1])
                expected = [name + (' <DIR>' if entry[0] & 16 else '')
                            for name, entry in entries.items()]
                body = vm.result('dir \\' + path.replace('/', '\\'), timeout=90)
                listing = body.split('Directory listing\r\n', 1)[1].split('CiukiOS SHELL', 1)[0]
                actual = [line for line in listing.splitlines() if line]
                assert actual == expected, (path, actual, expected)
                results['directories'].append({'path': path, 'entries': len(actual)})
                print(f'[installed-hdd] PASS {path}: all {len(actual)} short-name entries match image', flush=True)
            for path in ('SYSTEM/CIUKIDOS.SYS', 'SYSTEM/SHELL.COM', 'SYSTEM/BOOT.PCM',
                         'SYSTEM/DRIVERS/VGASETUP.COM', 'WINDOWS/WIN.COM', 'WINDOWS/WINCORE.COM',
                         'WINDOWS/SYSTEM/KRNL286.EXE', 'APPS/DOOM/DOOMCORE.EXE', 'APPS/DOOM/DOOM.WAD',
                         'APPS/DOOMVAN/PCDMCORE.EXE', 'APPS/DOOMVAN/DOOM.WAD', 'APPS/WOLF3D/VSWAP.WL6'):
                data = source.read(path)
                expected = f'[FILECHECK] FNV={fnv(data):08X} BYTES={len(data):08X}'
                vm.result('filechk \\' + path.replace('/', '\\'), expected, timeout=180)
                results['files'].append({'path': path, 'bytes': len(data), 'fnv': f'{fnv(data):08X}'})
                print(f'[installed-hdd] PASS complete DOS read {path}: {len(data)} bytes', flush=True)
            vm.result('comdemo', 'COM demo via INT21h')
            vm.result('mzdemo', 'MZ demo via INT21h')
            vm.result('vgasetup set 800', 'Shared resolution saved')
            vm.shot('dos-800', (800, 600))
            vm.result('echo HDD VIDEO READY', 'HDD VIDEO READY')
            vm.result('vgasetup set safe', 'Shared resolution saved')
            vm.result('dir \\windows', 'WINCORE.COM', 'WIN.COM', timeout=90)
            vm.shot('directory-after-video')
        elif args.case == 'video-menu':
            offset = vm.offset()
            vm.text('vgasetup')
            vm.wait('[VGASETUP] MENU READY', offset, 30)
            vm.shot('vgasetup-menu')
            vm.key('2'); vm.key('ret')
            vm.wait('[VGASETUP] PREVIEW', offset, 30)
            time.sleep(.5)
            vm.shot('vgasetup-preview', (800, 600))
            offset = vm.offset()
            vm.key('esc')
            vm.wait('[VGASETUP] MENU READY', offset, 30)
            vm.shot('vgasetup-cancelled')
            offset = vm.offset()
            vm.key('esc')
            vm.wait('CiukiOS SHELL C:\\APPS>', offset, 30)
            vm.result('type \\system\\video\\display.cfg', 'TEXT')
            old_ini = source.read('WINDOWS/SYSTEM.INI')
            vm.result('filechk \\windows\\system.ini',
                      f'[FILECHECK] FNV={fnv(old_ini):08X} BYTES={len(old_ini):08X}')
            offset = vm.offset()
            vm.text('vgasetup')
            vm.wait('[VGASETUP] MENU READY', offset, 30)
            vm.key('2'); vm.key('ret')
            vm.wait('[VGASETUP] PREVIEW', offset, 30)
            time.sleep(.5)
            offset = vm.offset()
            vm.key('ret')
            vm.wait('Shared resolution saved', offset, 30)
            vm.wait('CiukiOS SHELL C:\\APPS>', offset, 30)
            vm.result('echo VIDEO MENU RETURNED', 'VIDEO MENU RETURNED')
            vm.shot('vgasetup-applied', (800, 600))
            print('[installed-hdd] PASS interactive VGASETUP, 800 preview/cancel, confirm, DOS input', flush=True)
        elif args.case == 'video':
            start = time.monotonic()
            vm.result('dir \\windows', 'WINCORE.COM', 'WIN.COM', timeout=30)
            results['directory_seconds'] = time.monotonic() - start
            vm.result('echo VIDEO STILL RUNNING', 'VIDEO STILL RUNNING', timeout=30)
            vm.shot('directory-auto')
        elif args.case == 'legacy-apps':
            offset = vm.offset()
            vm.text('dosnav')
            time.sleep(12)
            a = np.array(Image.open(vm.shot('dosnav')))
            dump = out/'dosnav-text.bin'
            vm.hmp(f'pmemsave 0xb8000 4000 "{dump}"')
            text = dump.read_bytes()[::2].decode('cp437')
            assert 'C:\\APPS\\DOSNAV' in text, text
            assert 'dn       com' in text and 'readme   txt' in text and 'SSAVERS' in text, text
            vm.key('tab'); time.sleep(.5)
            b = np.array(Image.open(vm.shot('dosnav-other-panel')))
            assert a.shape == b.shape and np.any(a != b), 'DOS Navigator did not change active panel'
            vm.key('alt-x'); time.sleep(.5); vm.key('ret')
            vm.wait('CiukiOS SHELL C:\\APPS>', offset, 60)
            vm.result('comdemo', 'COM demo via INT21h')
            offset = vm.offset()
            vm.text('costa')
            time.sleep(10)
            a = np.array(Image.open(vm.shot('costa', (640, 350))))
            vm.hmp('mouse_move 40 20 0'); time.sleep(1)
            b = np.array(Image.open(vm.shot('costa-mouse', (640, 350))))
            changed = np.any(a != b, axis=2).sum()
            assert 20 <= changed <= 128, f'Costa pointer redraw changed {changed} pixels'
            vm.key('x'); time.sleep(1); vm.key('y')
            vm.wait('CiukiOS SHELL C:\\APPS>', offset, 60)
            vm.result('comdemo', 'COM demo via INT21h')
            print('[installed-hdd] PASS DOS Navigator directory panel, Tab and quit; Costa mouse, quit and subsequent COM', flush=True)
        elif args.case == 'classic-doom':
            def cr4(name):
                registers = vm.hmp('info registers')
                (out/(name+'-registers.log')).write_bytes(registers)
                return int(re.search(rb'CR4=([0-9a-fA-F]+)', registers)[1], 16)

            results['cr4_before_doom'] = cr4('before-doom')
            sprites = menu_skulls(source.read('APPS/DOOM/DOOM.WAD'))
            results['doom_menu_rows'] = {}

            def expect_row(name, expected, origin=(97, 64), rows=6):
                deadline = time.monotonic()+5
                while True:
                    row = selected_row(Image.open(vm.shot(name)), sprites, origin, rows)
                    if row == expected:
                        results['doom_menu_rows'][name] = row
                        return
                    assert time.monotonic() < deadline, f'{name}: selected row {row}, expected {expected}'
                    time.sleep(.25)

            offset = vm.offset()
            start = (out/'audio.wav').stat().st_size
            vm.text('doom')
            vm.wait('ST_Init: Init status bar.', offset, 90)
            deadline = time.monotonic() + 90
            while True:
                title = Image.open(vm.shot('doom-title'))
                if title.size in ((320, 200), (640, 400)) and len(set(list(title.get_flattened_data())[::16])) > 32:
                    break
                assert time.monotonic() < deadline, 'Doom title did not finish loading'
                time.sleep(1)

            def game_key(key):
                vm.hmp(f'sendkey {key} 150')
                time.sleep(.4)

            def menu_visible():
                a = np.array(Image.open(vm.shot('doom-menu-probe')).resize((640, 400)))
                red = (a[:, :, 0] > 110) & (a[:, :, 1] < 50) & (a[:, :, 2] < 50)
                return all(red[y:y+32, 185:500].sum() > 1100 for y in (120, 152, 184, 216, 248, 280))

            for attempt in range(6):
                if menu_visible():
                    break
                game_key('esc')
                time.sleep(2)
                vm.shot(f'doom-menu-attempt-{attempt}')
            assert menu_visible(), 'Doom did not accept Escape at the title'
            expect_row('doom-menu', 0)
            time.sleep(12)
            game_key('down'); time.sleep(1)
            expect_row('doom-menu-moved', 1)
            game_key('up')
            expect_row('doom-menu-return', 0)
            game_key('ret')  # New Game.
            expect_row('doom-episode', 0, (48, 63), 3)
            game_key('ret')  # Knee-Deep in the Dead.
            expect_row('doom-skill', 2, (48, 63), 5)
            game_key('ret')  # Default skill.
            time.sleep(10)
            results['cr4_during_doom'] = cr4('during-doom')
            if args.expect_pvi:
                assert results['cr4_during_doom'] & 2, 'Doom did not retain CR4.PVI'
            game_frame = Image.open(vm.shot('doom-game'))
            assert selected_row(game_frame, sprites, (48, 63), 5) is None, 'Doom stayed in the skill menu'
            a = np.array(game_frame)
            assert a.shape[:2] in ((200, 320), (400, 640))
            vm.hmp('sendkey up 1200'); time.sleep(2)
            b = np.array(Image.open(vm.shot('doom-walk')))
            changed = np.any(a[:int(a.shape[0]*.75)] != b[:int(b.shape[0]*.75)], axis=2).sum()
            assert changed > a.shape[0]*a.shape[1]*.01, 'Doom gameplay did not respond to movement'
            game_key('ctrl'); game_key('f10'); time.sleep(1)
            vm.shot('doom-quit')
            game_key('y')
            vm.wait('[DOOM] AUDIO CLEANUP COMPLETE', offset, 60)
            vm.wait('CiukiOS SHELL C:\\APPS>', offset, 60)
            results['cr4_after_doom'] = cr4('after-doom')
            assert (results['cr4_before_doom'] ^ results['cr4_after_doom']) & 2 == 0, 'Doom did not restore CR4.PVI'
            vm.result('comdemo', 'COM demo via INT21h')
            end = (out/'audio.wav').stat().st_size
            results['audio_windows'] = [('classic-doom', start, end)]
            offset = vm.offset()
            vm.text('exit')
            vm.wait('[DESKTOP] READY', offset, 60)
            before_pointer = vm.pointer()
            vm.hmp('mouse_move 30 20 0'); time.sleep(.3)
            assert vm.pointer() != before_pointer, 'Desktop mouse failed after Doom'
            vm.shot('doom-desktop-return')
            offset = vm.offset()
            vm.key('f4'); vm.wait('CiukiOS SHELL C:\\APPS>', offset, 30)
            vm.result('echo DOOM RETURN READY', 'DOOM RETURN READY')
            print('[installed-hdd] PASS classic Doom title, live menu, episode/skill, gameplay, movement, quit, subsequent COM', flush=True)
        else:
            audio_windows = []
            for name, cmd, cleanup in (
                    ('wolf3d', 'wolf3d nowait tedlevel 0 normal', '[WOLF3D] AUDIO CLEANUP COMPLETE'),
                    ('doomvan', 'doomvan -devparm -warp 1 1', '[DOOMVAN] AUDIO CLEANUP COMPLETE')):
                offset = vm.offset()
                start = (out/'audio.wav').stat().st_size
                vm.text(cmd)
                vm.wait('VSBHDA transient child:' if args.audio == 'ac97' else
                        '[AUDIO] Native Sound Blaster - starting application', offset, 90)
                time.sleep(15)
                deadline = time.monotonic() + 60
                while True:
                    a = np.array(Image.open(vm.shot(name+'-game')))
                    if a.shape[:2] in ((200,320),(400,640)) and len(np.unique(a.reshape(-1,3),axis=0)) > 32:
                        break
                    assert time.monotonic() < deadline, 'gameplay screen missing'
                    time.sleep(1)
                vm.hmp('sendkey up 1200'); time.sleep(2)
                b = np.array(Image.open(vm.shot(name+'-walk')))
                assert a.shape == b.shape
                region = slice(0, int(a.shape[0] * .75))
                changed = np.any(a[region] != b[region], axis=2).sum()
                assert changed > a.shape[0] * a.shape[1] * .01, f'{name}: movement unresponsive'
                vm.hmp('sendkey ctrl 700'); time.sleep(2)
                vm.hmp('sendkey f10 120'); time.sleep(1)
                vm.shot(name+'-quit')
                vm.hmp('sendkey y 120')
                vm.wait(cleanup, offset, 60)
                vm.wait('CiukiOS SHELL C:\\APPS>', offset, 60)
                vm.result('comdemo', 'COM demo via INT21h')
                end = (out/'audio.wav').stat().st_size
                audio_windows.append((name, start, end))
                print(f'[installed-hdd] PASS {name}: gameplay, movement, quit, subsequent COM', flush=True)
            results['audio_windows'] = audio_windows
        if args.finish_windows:
            offset = vm.offset()
            vm.text('win')
            vm.wait('VSBHDA transient child:' if args.audio == 'ac97' else
                    '[AUDIO] Native Sound Blaster - starting application',offset,90)
            time.sleep(20)
            shot = vm.shot('windows-after-games')
            pixels = np.array(Image.open(shot))
            ocr_path = out/'windows-after-games-ocr.png'
            Image.fromarray(np.where(pixels.min(axis=2)<60,0,255).astype('uint8')).resize(
                (pixels.shape[1]*3,pixels.shape[0]*3)).save(ocr_path)
            env = os.environ.copy()
            tessdata = Path('build/tools/tessdata').resolve()
            if (tessdata/'eng.traineddata').exists():
                env['TESSDATA_PREFIX'] = str(tessdata)
            text = subprocess.check_output(['tesseract',str(ocr_path),'stdout','--psm','11'],
                                           env=env,stderr=subprocess.DEVNULL).decode()
            (out/'windows-after-games.txt').write_text(text)
            assert 'programmanager' in ''.join(text.lower().split()), text
            offset = vm.offset()
            vm.key('alt-f4');time.sleep(1);vm.key('ret')
            vm.wait('CiukiOS SHELL C:\\APPS>',offset,90)
            vm.key('caps_lock');vm.key('caps_lock')
            vm.result('comdemo','COM demo via INT21h')
            results['windows_after_games'] = True
            print('[installed-hdd] PASS same-VM games -> Windows -> DOS, Caps Lock and COM',flush=True)
        vm.shot('final')
        kernel_listing = args.kernel_listing or (args.kernel.with_suffix('.lst') if args.kernel else None)
        if kernel_listing and kernel_listing.is_file():
            listing = kernel_listing.read_text().splitlines()
            code_end = listing_address(listing, 'boot_drive')
            reference = source.read('SYSTEM/CIUKIDOS.SYS')[:code_end]
            dump = out/'kernel-code-final.bin'
            vm.hmp(f'pmemsave 0x3000 {code_end} "{dump}"')
            actual = dump.read_bytes()
            assert len(actual) == len(reference), 'incomplete kernel RAM capture'
            changes = [i for i, (a, b) in enumerate(zip(reference, actual)) if a != b]
            # HDPMI owns the documented five-byte patchable XMS entry stub.
            # Every other byte, including all INT21 and MCB code, must agree.
            xms_entry = listing_address(listing, 'xms_entrypoint')
            unexpected = [i for i in changes if not xms_entry <= i < xms_entry + 5]
            assert not unexpected, f'Kernel code overwritten at {[hex(i) for i in unexpected[:32]]}'
            if changes:
                assert actual[xms_entry] == 0xea, 'unrecognized XMS entry patch'
            results['kernel_code'] = {'bytes': code_end, 'unexpected_changes': len(unexpected),
                                      'xms_entry_patch': actual[xms_entry:xms_entry+5].hex()}
            print(f'[installed-hdd] PASS {code_end} kernel code bytes: no unexpected writes', flush=True)
    except Exception:
        vm.shot('failure')
        (out/'registers.log').write_bytes(vm.hmp('info registers'))
        (out/'pic.log').write_bytes(vm.hmp('info pic'))
        if args.case == 'classic-doom':
            vm.hmp(f'memsave 0x2d0000 0x20000 "{out}/doom-code.bin"')
        vm.hmp(f'pmemsave 0 0x100000 "{out}/failure-ram.bin"')
        raise
    finally:
        motion_stop.set()
        if motion_thread:
            motion_thread.join(timeout=10)
        results['pointer_commands'] = motion_count[0]
        results['pointer_errors'] = motion_errors
        vm.close()
        assert hashlib.sha256(args.image.read_bytes()).hexdigest() == before_hash, 'source HDD changed'
        (out/'results.json').write_text(json.dumps(results, indent=2) + '\n')
    raw = (out/'audio.wav').read_bytes()
    if args.case == 'bios-fallback':
        after = FAT16(disk)
        for source_path, copied_path in (('APPS/MZDEMO.EXE', 'APPS/CHSCOPY.EXE'),
                                         ('SYSTEM/BOOT.PCM', 'APPS/CHSCOPY.PCM')):
            original = source.read(source_path)
            copied = after.read(copied_path)
            assert copied == original, f'Guest COPY corrupted {copied_path}'
            print(f'[installed-hdd] PASS byte-exact CHS read/write copy {copied_path}: {len(copied)} bytes', flush=True)
    if args.case == 'video-menu':
        after = FAT16(disk)
        assert after.read('SYSTEM/VIDEO/DISPLAY.CFG') == b'0800'
        assert b'Width=800' in after.read('WINDOWS/SYSTEM.INI')
    pcm, bits = pcm_payload(raw if args.direct_dos else raw[:results['boot_audio_end']])
    samples = np.frombuffer(pcm, dtype='<i2').astype(np.int32)
    if not args.direct_dos:
        assert bits == 16 and len(samples) > 1000 and np.max(np.abs(samples)) > 3000, 'boot PCM missing'
    else:
        assert not np.any(samples), 'direct DOS control unexpectedly ran startup audio'
    if args.pointer_activity:
        assert motion_count[0] > 100 and not motion_errors, 'concurrent PS/2 input was not exercised'
    for name, start, end in results.get('audio_windows', []):
        data = raw[max(44,start+4096):end-4096]
        data = data[:len(data)//4*4]
        assert len(data) > 20000
        rms = float(np.std(np.frombuffer(data, dtype='<i2').astype(float)))
        assert rms > 20, (name, rms)
        print(f'[installed-hdd] PASS {name}: PCM RMS {rms:.1f}', flush=True)
    print('[installed-hdd] PASS HDD-only case completed; '+('silent direct DOS control' if args.direct_dos else 'digital startup PCM present'), flush=True)
    results['completed'] = True
    (out/'results.json').write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()
