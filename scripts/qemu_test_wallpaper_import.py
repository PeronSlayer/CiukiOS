#!/usr/bin/env python3
"""Test wallpaper import: the tiles are copied with Files, then applied.

With N wallpapers installed, the input image must contain
QAIMPORT/WALL<N+1>.CWP (valid) and WALL<N+2>.CWP (damaged); --make-fixtures
adds them to the copy. This test copies the image; guest RAM is observed only, never written.
"""
import argparse
import hashlib
import json
import shutil
import time
from pathlib import Path

import numpy as np
from PIL import Image

from qemu_test_installed_hdd import FAT16
from qemu_test_native_windows import WindowVM
from qemu_test_native_utilities import Utilities
from qemu_test_wallpaper import cover_frame, palette


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--listing', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--make-fixtures', action='store_true')
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    disk = a.output/'target.img'
    shutil.copyfile(a.image, disk)
    fs = FAT16(disk)
    installed = sorted(n for n in fs.entries(fs.entry('SYSTEM/UI')[1]) if n.startswith('WALL') and n.endswith('.CWP'))
    base = len(installed)
    valid, damaged = base + 1, base + 2
    import subprocess
    volume = f'{disk}@@{fs.start}'
    # The helper clicks within 800x600 (its control geometry).
    (a.output/'DISPLAY.CFG').write_bytes(b'0800')
    subprocess.run(['mcopy', '-o', '-i', volume, str(a.output/'DISPLAY.CFG'), '::SYSTEM/VIDEO/DISPLAY.CFG'], check=True)
    if a.make_fixtures:
        good = fs.read('SYSTEM/UI/' + installed[-1])
        (a.output/'valid.cwp').write_bytes(good)
        (a.output/'damaged.cwp').write_bytes(good[:100])
        subprocess.run(['mmd', '-i', volume, '::QAIMPORT'], check=True)
        subprocess.run(['mcopy', '-i', volume, str(a.output/'valid.cwp'), f'::QAIMPORT/WALL{valid:02d}.CWP'], check=True)
        subprocess.run(['mcopy', '-i', volume, str(a.output/'damaged.cwp'), f'::QAIMPORT/WALL{damaged:02d}.CWP'], check=True)
        fs = FAT16(disk)
    shell = fs.read('SYSTEM/SHELL.COM')
    catalog_before = fs.read('SYSTEM/UI/WALLS.DAT')
    report = {'status': 'running', 'scope': 'QEMU real native Files copy and GUI input; no physical hardware claim',
              'source_sha256': hashlib.sha256(a.image.read_bytes()).hexdigest(),
              'ram_writes': False, 'events': []}
    vm = None
    def save():
        (a.output/'result.json').write_text(json.dumps(report, indent=2)+'\n')
    def wallpaper():
        ui.click(1)
        ui.click(22, 0)
        ui.click(34, 0)
        vm.key('ret')
        ui.until(lambda: ui.b('ui_active_window') == 10, 'Wallpaper opens')
    def selected(number):
        while ui.w('wp_page') // 5 < (number-1)//5:
            ui.click(182, 10)
        ui.click(184+(number-1)%5, 10)
        ui.click(183, 10)
    def hide_all():
        for _ in range(12):
            ui.refresh()
            if ui.b('ui_window_visible') != 1 and not any(ui.b('ui_window_flags', i) & 1 for i in range(1,13)):
                return
            vm.repaint_key('alt-f4')
        raise AssertionError('Visible windows did not close')
    def compare(name):
        vm.position(70, 32)
        time.sleep(.3)
        frame = np.asarray(Image.open(vm.shot(name)).convert('RGB'))
        raw = FAT16(disk).read(f'SYSTEM/UI/WALL{valid:02d}.CWP')
        w, h = int.from_bytes(raw[4:6], 'little'), int.from_bytes(raw[6:8], 'little')
        pal = np.frombuffer(raw[16:784], np.uint8).reshape(256,3).astype(np.int32)
        indices = np.frombuffer(raw[784:], np.uint8).reshape(h,w)
        ui_palette = palette(a.output).astype(np.int32)
        distances = ((pal[:,None,:]//4-ui_palette[None,:,:])**2).sum(2)
        mapped = ui_palette[distances.argmin(1)]
        mapped = ((mapped << 2)|((mapped&1)*3)).astype(np.uint8)
        tile = mapped[indices]
        expected = frame.copy()
        expected[29:frame.shape[0]-32] = cover_frame(tile, frame.shape[1], frame.shape[0])
        mask = np.zeros(frame.shape[:2],bool)
        mask[40:frame.shape[0]-80,150:frame.shape[1]-15] = True
        mismatches = int(((frame != expected).any(2)&mask).sum())
        report['events'].append({'name':name,'position':'Fill',
                                 'compared_pixels':int(mask.sum()),'mismatched_pixels':mismatches})
        save()
        assert mismatches == 0
    try:
        vm = WindowVM(disk, a.output/'first', 'std', memory=128, palette='platinum')
        vm.auto_enter_dos = False
        vm.ready()
        ui = Utilities(vm, shell, a.listing)
        assert ui.w('wp_count') == base
        wallpaper()
        ui.click(17,10)  # Minimize Wallpaper before copying, leaving its old catalog resident.
        # Files (\SYSTEM\APPS\FILES.APP): select both tiles, copy, paste.
        def keys(*names, marker=None, timeout=60):
            offset = vm.offset()
            for name in names:
                vm.key(name)
            if marker:
                vm.wait(marker, offset, timeout)
        keys('meta_l-r')
        keys(*'explorer', 'spc', 'c', 'shift-semicolon', 'backslash', *'qaimport', 'ret',
             marker='[FILES] list C:\\QAIMPORT 2')
        keys('ctrl-a', 'ctrl-c', 'f4')
        keys(*'c', 'shift-semicolon', 'backslash', *'system', 'backslash', *'ui', 'ret',
             marker='[FILES] list C:\\SYSTEM\\UI')
        keys('ctrl-v', marker='[FILES] job 2 item(s) copied.')
        keys('alt-f4', marker='[DESKTOP] WINDOW 08 CLOSE')
        assert FAT16(disk).read('SYSTEM/UI/WALLS.DAT') == catalog_before
        assert ui.w('wp_count') == base, 'Catalog changed without user Refresh'
        wallpaper()
        ui.click(189,10)
        ui.until(lambda: ui.w('wp_count') == damaged,'Refresh discovers copied files')
        selected(valid)
        ui.until(lambda: ui.b('wp_selected') == valid,'Imported wallpaper applied')
        assert FAT16(disk).read('SYSTEM/UI/WALL.CFG') == bytes([valid, 0])
        selected(damaged)
        ui.until(lambda: 'could not be loaded' in ui.z_at(ui.w('wp_status')-256), 'Damaged tile has an English error')
        assert ui.b('wp_selected') == valid
        assert FAT16(disk).read('SYSTEM/UI/WALL.CFG') == bytes([valid, 0])
        vm.shot('damaged-file-error')
        hide_all()
        compare('imported-tile-after-failed-apply')
        vm.close(); vm = None
        vm = WindowVM(disk,a.output/'restart','std',memory=128,palette='platinum')
        vm.auto_enter_dos = False
        vm.ready()
        ui = Utilities(vm,shell,a.listing)
        assert ui.w('wp_count') == damaged and ui.b('wp_selected') == valid
        hide_all()
        compare('imported-tile-after-cold-boot')
        report['status'] = 'pass'
        report['catalog_unmodified'] = FAT16(disk).read('SYSTEM/UI/WALLS.DAT') == catalog_before
        save()
        print('[wallpaper-import] PASS')
    except Exception as e:
        report['status']='failed';report['error']=str(e);save()
        if vm: vm.shot('failure')
        raise
    finally:
        if vm: vm.close()


if __name__ == '__main__':
    main()
