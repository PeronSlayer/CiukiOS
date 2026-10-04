#!/usr/bin/env python3
"""Boot the full HDD in HARD/Mode 12h and check composed scene text."""
import argparse
from pathlib import Path
import shutil
import subprocess
import time

from PIL import Image

from qemu_test_full_display_profile import VM


def dark_pixels(image, box):
    return sum(max(pixel) < 100 for pixel in image.crop(box).get_flattened_data())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    parser.add_argument('--output', type=Path, default=Path('build/tests/ui-vga-composed'))
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    disk = out / 'hard.img'
    shutil.copyfile(args.image, disk)
    subprocess.run(['mcopy', '-o', '-i', str(disk), '-', '::SYSTEM/STARTUP.CFG'],
                   input=b'HARD\r\n', check=True)
    vm = VM(disk, out, memory=256)
    try:
        vm.wait('[DESKTOP] READY', timeout=45)
        time.sleep(3)
        desktop = Image.open(vm.shot('desktop', (640, 480))).convert('RGB')
        desktop.save(out / 'desktop.png')
        # This catches direct VGA glyph writes that the completed band erases.
        assert dark_pixels(desktop, (33, 5, 95, 23)) > 80, 'CiukiOS brand text was erased'
        offset = vm.offset()
        vm.key('f1')
        vm.wait('[DESKTOP] PAINT', offset, 30)
        time.sleep(1)
        about = Image.open(vm.shot('about', (640, 480))).convert('RGB')
        about.save(out / 'about.png')
        assert dark_pixels(about, (123, 175, 380, 207)) > 250, 'About text was erased'
        print('[ui-vga] PASS Mode 12h desktop, CiukiOS brand and About text', flush=True)
    finally:
        vm.close()


if __name__ == '__main__':
    main()
