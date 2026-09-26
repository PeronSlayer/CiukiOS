#!/usr/bin/env python3
"""Test wallpaper import using native Files and real GUI clicks.

The input image must contain QAIMPORT/WALL12.CWP (valid) and WALL13.CWP
(damaged). This test copies the image; guest RAM is observed only, never written.
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
from qemu_test_wallpaper import palette


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--listing', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    disk = a.output/'target.img'
    shutil.copyfile(a.image, disk)
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
            if ui.b('ui_window_visible') != 1 and not any(ui.b('ui_window_flags', i) & 1 for i in range(1,12)):
                return
            vm.repaint_key('alt-f4')
        raise AssertionError('Visible windows did not close')
    def compare(name):
        vm.position(70, 32)
        time.sleep(.3)
        frame = np.asarray(Image.open(vm.shot(name)).convert('RGB'))
        raw = FAT16(disk).read('SYSTEM/UI/WALL12.CWP')
        w, h = int.from_bytes(raw[4:6], 'little'), int.from_bytes(raw[6:8], 'little')
        pal = np.frombuffer(raw[16:784], np.uint8).reshape(256,3).astype(np.int32)
        indices = np.frombuffer(raw[784:], np.uint8).reshape(h,w)
        ui_palette = palette(a.output).astype(np.int32)
        distances = ((pal[:,None,:]//4-ui_palette[None,:,:])**2).sum(2)
        mapped = ui_palette[distances.argmin(1)]
        mapped = ((mapped << 2)|((mapped&1)*3)).astype(np.uint8)
        tile = mapped[indices]
        yy,xx = np.indices(frame.shape[:2])
        expected = tile[yy%h,xx%w]
        mask = np.zeros(frame.shape[:2],bool)
        mask[40:frame.shape[0]-80,150:frame.shape[1]-15] = True
        mismatches = int(((frame != expected).any(2)&mask).sum())
        report['events'].append({'name':name,'compared_pixels':int(mask.sum()),'mismatched_pixels':mismatches})
        save()
        assert mismatches == 0
    try:
        vm = WindowVM(disk, a.output/'first', 'std', memory=128, palette='platinum')
        vm.auto_enter_dos = False
        vm.ready()
        ui = Utilities(vm, shell, a.listing)
        assert ui.w('wp_count') == 11
        wallpaper()
        ui.click(17,10)  # Minimize Wallpaper before copying, leaving its old catalog resident.
        ui.click(2)
        ui.until(lambda: ui.b('ui_active_window') == 8,'Files opens')
        ui.location('C:\\QAIMPORT')
        for number in (12,13):
            ui.select(f'WALL{number}.CWP')
            ui.operation(105,f'C:\\SYSTEM\\UI\\WALL{number}.CWP','File copied.')
        assert FAT16(disk).read('SYSTEM/UI/WALLS.DAT') == catalog_before
        assert ui.w('wp_count') == 11, 'Catalog changed without user Refresh'
        wallpaper()
        ui.click(189,10)
        ui.until(lambda: ui.w('wp_count') == 13,'Refresh discovers copied files')
        selected(12)
        ui.until(lambda: ui.b('wp_selected') == 12,'Imported wallpaper applied')
        assert FAT16(disk).read('SYSTEM/UI/WALL.CFG') == bytes([12])
        selected(13)
        ui.until(lambda: 'could not be loaded' in ui.z_at(ui.w('wp_status')-256), 'Damaged tile has an English error')
        assert ui.b('wp_selected') == 12
        assert FAT16(disk).read('SYSTEM/UI/WALL.CFG') == bytes([12])
        vm.shot('damaged-file-error')
        hide_all()
        compare('imported-tile-after-failed-apply')
        vm.close(); vm = None
        vm = WindowVM(disk,a.output/'restart','std',memory=128,palette='platinum')
        vm.auto_enter_dos = False
        vm.ready()
        ui = Utilities(vm,shell,a.listing)
        assert ui.w('wp_count') == 13 and ui.b('wp_selected') == 12
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
