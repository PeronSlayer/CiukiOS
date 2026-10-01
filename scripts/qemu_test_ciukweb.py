#!/usr/bin/env python3
"""QEMU gate for CiukWeb's native window, HTTP fetch and HTML text rendering."""
import argparse
import json
import shutil
import subprocess
import time
from pathlib import Path

from PIL import Image
from qemu_test_full_display_profile import VM


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--browse-wait', type=float, default=0.5,
                        help='Optional pause before the framebuffer capture')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    disk = out / 'disk.img'
    shutil.copyfile(args.image, disk)
    result = {'passed': False, 'host': 'QEMU', 'physical_hardware_qualified': False}
    vm = VM(disk, out, qemu_args=[
        '-vga', 'std', '-netdev', 'user,id=ciuknet0',
        '-device', 'ne2k_isa,netdev=ciuknet0,irq=3,iobase=0x300,mac=52:54:00:12:34:56',
    ], memory=256)
    try:
        vm.wait('CiukiOS SHELL C:\\APPS>', timeout=90)
        vm.command('netstart', 'NETSTART: ICMP resident active', 45)
        offset = vm.offset()
        vm.text('exit')
        vm.wait('[DESKTOP] READY', offset, 40)
        offset = vm.offset()
        vm.key('f2')
        vm.key('right')
        vm.key('right')
        vm.key('ret')
        vm.wait('[DESKTOP] WINDOW 17 OPEN', offset, 30)
        vm.shot('ciukweb-open')
        offset = vm.offset()
        vm.key('ret')
        vm.wait('[CIUKWEB] fetch http://example.com/', offset, 20)
        vm.wait('[CIUKWEB] rendered http://example.com/', offset, 90)
        time.sleep(args.browse_wait)
        page = subprocess.check_output(['mtype', '-i', str(disk), '::NET/CIUKWEB.HTM'])
        result['http_bytes'] = len(page)
        result['example_domain_in_download'] = b'Example Domain' in page
        screenshot = Image.open(vm.shot('ciukweb-page')).convert('RGB')
        result['screen_size'] = list(screenshot.size)
        content = screenshot.crop((360, 300, 1040, 680))
        result['light_pixels'] = sum(1 for r, g, b in content.getdata() if min(r, g, b) > 220)
        result['dark_pixels'] = sum(1 for r, g, b in content.getdata() if max(r, g, b) < 100)
        result['passed'] = (result['example_domain_in_download'] and
                            result['light_pixels'] > 10000 and result['dark_pixels'] > 100)
        if not result['passed']:
            raise AssertionError('native browser did not render the downloaded page')
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
