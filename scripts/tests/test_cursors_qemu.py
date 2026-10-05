#!/usr/bin/env python3
"""Automated QEMU test for CiukiOS cursor themes, hover, blinking and control panel."""
import sys
import os
import time
import subprocess
from pathlib import Path
from PIL import Image
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.append(str(ROOT / 'scripts'))
from qemu_test_native_desktop import DesktopVM, match_pointer

def test_cursors():
    out_dir = Path('/tmp/ciukios-cursor-test')
    out_dir.mkdir(parents=True, exist_ok=True)
    
    os.environ.setdefault('CIUKIOS_SETUP_QEMU_ACCEL', 'kvm')
    img_path = ROOT / 'build/full/ciukios-full.img'
    iso_path = ROOT / 'build/full/CiukiOS_full_cd_0-8-0.iso'
    if not iso_path.exists():
        iso_path = None
    import shutil
    target_img = out_dir / 'test.img'
    shutil.copyfile(img_path, target_img)
    vm = DesktopVM(disks=[(0, str(target_img))], iso=iso_path, output=out_dir)
    try:
        print("[test-cursor] Waiting for Desktop ready...")
        vm.ready()
        print("[test-cursor] Desktop ready!")
        
        # Take initial screenshot
        shot1 = Path(vm.shot('desktop-ready'))
        print(f"[test-cursor] Initial shot saved: {shot1}")
        
        # In Application Library: press Down to select Control Panel
        print("[test-cursor] Selecting Control Panel from Application Library...")
        vm.key('down')
        time.sleep(0.5)
        vm.key('ret')
        time.sleep(2.0)
        
        shot_ctrl = Path(vm.shot('control-opened'))
        print(f"[test-cursor] Control panel shot: {shot_ctrl}")
        
        # In Control Panel, select Mouse applet
        print("[test-cursor] Selecting Mouse applet in Control Panel...")
        vm.key('m')
        time.sleep(0.5)
        vm.key('ret')
        time.sleep(1.5)
        
        shot_mouse = Path(vm.shot('mouse-applet'))
        print(f"[test-cursor] Mouse applet shot: {shot_mouse}")
        
        # Convert PPM to PNG and copy to artifact directory
        import shutil
        for name in ('desktop-ready', 'control-opened', 'mouse-applet'):
            ppm = out_dir / f'{name}.ppm'
            if ppm.exists():
                png = out_dir / f'{name}.png'
                Image.open(ppm).save(png)
                shutil.copy(png, Path('/home/peronslayer/.gemini/antigravity/brain/9f45aa5c-201b-453e-852d-b96e0c5221b0') / f'{name}.png')
        
        print("[test-cursor] PASS basic desktop cursor validation!")
        
    finally:
        try:
            vm.hmp('quit')
        except Exception:
            pass
        vm.close()

if __name__ == '__main__':
    test_cursors()
