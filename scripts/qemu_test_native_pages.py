#!/usr/bin/env python3
"""Exercise the trusted Jemm-backed page allocator in the full HDD profile."""
import argparse
from pathlib import Path
import shutil

from qemu_test_full_display_profile import VM


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    parser.add_argument('--output', type=Path, default=Path('build/tests/native-pages'))
    parser.add_argument('--memory', type=int, default=256)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    disk = output / 'disk.img'
    shutil.copyfile(args.image, disk)
    vm = VM(disk, output, memory=args.memory)
    try:
        vm.wait('[DESKTOP] READY', timeout=60)
        vm.key('f4')
        vm.wait('CiukiOS SHELL C:\\APPS>', timeout=30)
        offset = vm.offset()
        vm.text('\\vm\\natpage.com')
        vm.wait('[NATIVE-PAGES] PASS 8 owner/zero/release probes', offset, 45)
        vm.wait('CiukiOS SHELL C:\\APPS>', offset, 25)
    finally:
        vm.close()
    print('[native-pages] PASS Jemm-owned page blocks and shell return', flush=True)


if __name__ == '__main__':
    main()
