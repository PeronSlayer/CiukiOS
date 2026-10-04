#!/usr/bin/env python3
"""Check that the full image exposes installed RAM through XMS 3.x."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess

from qemu_test_full_display_profile import VM
from read_memory_map import read_map


ROOT = Path(__file__).resolve().parents[1]
PATTERN = re.compile(r'\[MEMCAP\] XMS88 largest=([0-9A-F]{8}) free=([0-9A-F]{8})')
DOS_PATTERN = re.compile(r'\[MEMCAP\] DOS largest=([0-9A-F]{8}) KiB')


def measure(image, output, memory):
    output.mkdir(parents=True, exist_ok=True)
    disk = output / 'full.img'
    shutil.copyfile(image, disk)
    probe = output / 'MEMCAP.COM'
    subprocess.run(['nasm', '-f', 'bin', 'scripts/fixtures/memory_capacity_probe.asm',
                    '-o', str(probe)], cwd=ROOT, check=True)
    subprocess.run(['mcopy', '-o', '-i', str(disk), str(probe), '::APPS/MEMCAP.COM'], check=True)
    strategy_probe = output / 'MEMSTRAT.COM'
    subprocess.run(['nasm', '-f', 'bin', 'scripts/fixtures/memory_strategy_probe.asm',
                    '-o', str(strategy_probe)], cwd=ROOT, check=True)
    subprocess.run(['mcopy', '-o', '-i', str(disk), str(strategy_probe),
                    '::APPS/MEMSTRAT.COM'], check=True)
    vm = VM(disk, output, memory=memory)
    try:
        vm.wait('[DESKTOP] READY', timeout=50)
        vm.key('f4')
        vm.wait('CiukiOS SHELL C:\\APPS>', timeout=25)
        strategy_offset = vm.offset()
        vm.text('memstrat')
        vm.wait('[MEMSTRAT] PASS first/best/last MCB placement', strategy_offset, 20)
        vm.wait('CiukiOS SHELL C:\\APPS>', strategy_offset, 20)
        offset = vm.offset()
        vm.text('memcap')
        vm.wait('[MEMCAP] XMS88', offset, 20)
        vm.wait('[MEMCAP] XMS89 allocate+free PASS', offset, 20)
        vm.wait('[MEMCAP] DOS largest=', offset, 20)
        vm.wait('CiukiOS SHELL C:\\APPS>', offset, 20)
        serial = vm.serial.read_bytes()[offset:].decode(errors='replace')
        result = PATTERN.search(serial)
        assert result, 'XMS 3.x query did not report usable memory'
        largest, free = (int(value, 16) for value in result.groups())
        assert largest == free, (largest, free)
        assert free >= (memory - 8) * 1024, (memory, free)
        conventional = DOS_PATTERN.search(serial)
        assert conventional, 'DOS conventional memory query did not report a largest block'
        conventional_kib = int(conventional.group(1), 16)
        fork_offset = vm.offset()
        vm.text('\\vm\\vmfork.com \\apps\\memcap.com')
        vm.wait('[MEMCAP] XMS89 allocate+free PASS', fork_offset, 60)
        vm.wait('[MEMCAP] DOS largest=', fork_offset, 60)
        vm.wait('CiukiOS SHELL C:\\APPS>', fork_offset, 60)
        fork_serial = vm.serial.read_bytes()[fork_offset:].decode(errors='replace')
        fork_conventional = DOS_PATTERN.search(fork_serial)
        assert fork_conventional, 'forked DOS VM did not report conventional memory'
        fork_kib = int(fork_conventional.group(1), 16)
        assert fork_kib >= 540, f'forked DOS arena regressed to {fork_kib} KiB'
    finally:
        vm.close()
    captured = output / 'MEMMAP.BIN'
    subprocess.run(['mcopy', '-o', '-i', str(disk), '::SYSTEM/MEMMAP.BIN', str(captured)],
                   check=True)
    firmware = read_map(captured, require_complete=True)
    assert not firmware['overlapping_entries'], firmware['overlapping_entries']
    assert 512 < firmware['conventional_kib'] <= 640
    assert 0x8000 <= firmware['ebda_segment'] <= 0xA000
    assert firmware['usable_above_1m_bytes'] >= free * 1024
    print(f'[memory-capacity] PASS {memory} MiB: {free / 1024:.1f} MiB free XMS; '
          f'{conventional_kib} KiB system DOS block, {fork_kib} KiB forked DOS block; '
          f'{firmware["usable_above_1m_bytes"] / 1048576:.1f} MiB pre-Jemm usable RAM', flush=True)
    return free


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    parser.add_argument('--output', type=Path, default=Path('build/tests/memory-capacity'))
    args = parser.parse_args()
    image = args.image.resolve()
    output = args.output.resolve()
    first = measure(image, output / '128', 128)
    second = measure(image, output / '256', 256)
    assert second - first >= 120 * 1024, (first, second)


if __name__ == '__main__':
    main()
