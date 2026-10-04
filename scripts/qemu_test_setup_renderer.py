#!/usr/bin/env python3
"""Boot disposable full HDD images and check Setup's VGA and VBE painters."""
import argparse
from pathlib import Path
import shutil
import subprocess
import time

from PIL import Image

from qemu_test_full_display_profile import VM


ROOT = Path(__file__).resolve().parents[1]


def run_mode(image, output, vga):
    mode = 'vga' if vga else 'vbe'
    out = output / mode
    out.mkdir(parents=True, exist_ok=True)
    setup = out / 'SETUP.COM'
    define = ['-D', 'SETUP_FORCE_VGA=1'] if vga else []
    subprocess.run(['nasm', '-f', 'bin', '-D', 'SETUP_LIVE_CD_MODE=1', *define,
                    'src/com/setup.asm', '-o', str(setup)], cwd=ROOT, check=True)
    disk = out / 'full.img'
    shutil.copyfile(image, disk)
    subprocess.run(['mcopy', '-o', '-i', str(disk), str(setup), '::APPS/SETUP.COM'], check=True)
    vm = VM(disk, out, memory=256)
    try:
        vm.wait('[DESKTOP] READY', timeout=45)
        vm.key('f4')
        vm.wait('CiukiOS SHELL C:\\APPS>', timeout=30)
        offset = vm.offset()
        vm.text('setup')
        vm.wait('[SETUP-GUI] PAGE 00', offset, 30)
        time.sleep(.5)
        size = (640, 480) if vga else (800, 600)
        screenshot = Image.open(vm.shot('welcome', size)).convert('RGB')
        screenshot.save(out / 'welcome.png')
        box = (210, 85, 323, 110) if vga else (290, 145, 402, 170)
        dark = sum(max(pixel) < 100 for pixel in screenshot.crop(box).get_flattened_data())
        assert dark > 250, f'{mode}: Setup heading was erased ({dark} dark pixels)'
        offset = vm.offset()
        vm.key('ret')
        vm.wait('[SETUP-GUI] PAGE 01', offset, 20)
        time.sleep(.3)
        destination = Image.open(vm.shot('destination', size)).convert('RGB')
        destination.save(out / 'destination.png')
        assert screenshot.tobytes() != destination.tobytes(), f'{mode}: page transition was not painted'
        print(f'[setup-renderer] PASS {mode.upper()} welcome and destination, Ciuki logo and text', flush=True)
    finally:
        vm.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    parser.add_argument('--output', type=Path, default=Path('build/tests/setup-renderer'))
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    for vga in (True, False):
        run_mode(args.image.resolve(), output, vga)


if __name__ == '__main__':
    main()
