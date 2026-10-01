#!/usr/bin/env python3
"""Validate the bundled CiukiOS TinyGL software OpenGL prototype in QEMU."""
import argparse
import json
import shutil
import sys
import time
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qemu_test_native_windows import WindowVM  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    disk = out / 'disk.img'
    shutil.copyfile(args.image, disk)
    result = {'passed': False, 'host': 'QEMU', 'physical_hardware_qualified': False}
    vm = WindowVM(disk, out, 'std', memory=128, palette='platinum')
    try:
        vm.ready()
        vm.key('meta_l-r')
        vm.text('c:\\programs\\ciukgl\\gldemo.exe')
        vm.key('ret')
        vm.wait('[CIUKGL] FRAME READY', 0, 45)
        time.sleep(0.4)
        pixels = Image.open(vm.shot('opengl-triangle')).convert('RGB')
        counts = [0, 0, 0]
        for red, green, blue in pixels.crop((390, 290, 1050, 718)).getdata():
            if red > 140 and red > green * 3 // 2 and red > blue * 3 // 2:
                counts[0] += 1
            if green > 140 and green > red * 3 // 2 and green > blue * 3 // 2:
                counts[1] += 1
            if blue > 140 and blue > red * 3 // 2 and blue > green * 3 // 2:
                counts[2] += 1
        result['red_green_blue_pixels'] = counts
        if min(counts) < 300:
            raise AssertionError('TinyGL triangle was not visible in all three colours')
        offset = vm.offset()
        vm.key('esc')
        vm.wait('[DOSVM] ended', offset, 20)
        result['passed'] = True
    except Exception as error:
        result['error'] = repr(error)
        try:
            vm.shot('failure')
        except Exception:
            pass
    finally:
        vm.close()
    (out / 'report.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
