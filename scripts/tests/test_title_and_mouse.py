#!/usr/bin/env python3
"""Targeted test for window title dynamic tab width and mouse cursor applet."""
import sys
import os
import time
import shutil
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.append(str(ROOT / 'scripts'))
from qemu_test_native_windows import WindowVM

def test():
    out_dir = Path('/tmp/ciukios-cursor-test')
    out_dir.mkdir(parents=True, exist_ok=True)

    img_path = ROOT / 'build/full/ciukios-full.img'
    target_img = out_dir / 'test.img'
    shutil.copyfile(img_path, target_img)

    vm = WindowVM(target_img, out_dir, 'std', memory=128, palette='platinum')
    try:
        print("[test] Waiting for Desktop ready...")
        vm.ready()
        print("[test] Desktop ready!")

        # Open CiukNote with long document name to verify dynamic title bar
        print("[test] Launching CiukNote with long title...")
        vm.key('meta_l-r')
        time.sleep(0.5)
        cmd1 = 'ciuknote c:\\LongDocumentTitleTest.txt'
        for ch in cmd1:
            if ch == ' ':
                vm.key('spc')
            elif ch == '\\':
                vm.key('backslash')
            elif ch == ':':
                vm.key('shift-semicolon')
            elif ch == '.':
                vm.key('dot')
            else:
                vm.key(ch)
            time.sleep(0.04)
        vm.key('ret')
        time.sleep(2.0)

        shot_note = Path(vm.shot('ciuknote-title-long'))
        print(f"[test] CiukNote title shot: {shot_note}")

        # Open Control Panel Mouse applet
        print("[test] Launching Control Panel Mouse applet...")
        vm.key('meta_l-r')
        time.sleep(0.5)
        cmd2 = 'control mouse'
        for ch in cmd2:
            if ch == ' ':
                vm.key('spc')
            else:
                vm.key(ch)
            time.sleep(0.04)
        vm.key('ret')
        time.sleep(2.0)

        shot_mouse = Path(vm.shot('mouse-applet-scheme'))
        print(f"[test] Mouse applet shot: {shot_mouse}")

        # Select Classic 95 (Alt-C) and save (Enter)
        print("[test] Selecting Classic 95 and saving...")
        vm.key('alt-c')
        time.sleep(0.5)
        shot_sel = Path(vm.shot('mouse-selected-classic95'))
        vm.key('ret')
        time.sleep(1.0)

        # Convert to PNG and save to brain directory
        brain_dir = Path('/home/peronslayer/.gemini/antigravity/brain/9f45aa5c-201b-453e-852d-b96e0c5221b0')
        for name in ('ciuknote-title-long', 'mouse-applet-scheme', 'mouse-selected-classic95'):
            ppm = out_dir / f'{name}.ppm'
            if ppm.exists():
                png = out_dir / f'{name}.png'
                Image.open(ppm).save(png)
                shutil.copy(png, brain_dir / f'{name}.png')
                print(f"[test] Saved {name}.png to brain directory")

        print("[test] Completed successfully!")

    finally:
        try:
            vm.hmp('quit')
        except Exception:
            pass
        vm.close()

if __name__ == '__main__':
    test()
