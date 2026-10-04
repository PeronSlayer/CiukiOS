#!/usr/bin/env python3
"""Save one read-only E820 capture from the serial-pinned Transcend SSD."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import tempfile
from datetime import datetime, timezone

from read_memory_map import read_map


DISK = Path('/dev/disk/by-id/ata-TS64GMSA230S_H551120730')
PARTITION_SECTORS = 262144
DISK_SECTORS = 125045424
OUTPUT = Path('docs/validation/2026-10-02-physical-memory')


def identified_partition() -> Path:
    if os.geteuid() != 0:
        raise RuntimeError('root is required to read the block device')
    disk = DISK.resolve(strict=True)
    if not stat.S_ISBLK(disk.stat().st_mode):
        raise RuntimeError('Transcend link is not a block device')
    sys = Path('/sys/class/block') / disk.name
    if int((sys / 'size').read_text()) != DISK_SECTORS:
        raise RuntimeError('disk size changed')
    if int((sys / 'queue/logical_block_size').read_text()) != 512:
        raise RuntimeError('sector size changed')
    props = subprocess.check_output(
        ['udevadm', 'info', '--query=property', '--name', str(disk)], text=True)
    if 'ID_SERIAL=TS64GMSA230S_H551120730\n' not in props:
        raise RuntimeError('Transcend serial changed')
    part = Path('/sys/class/block') / (disk.name + '1')
    if int((part / 'start').read_text()) != 63 or \
            int((part / 'size').read_text()) != PARTITION_SECTORS:
        raise RuntimeError('CiukiOS partition layout changed')
    return Path('/dev') / (disk.name + '1')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--machine', choices=('ibm-t23', 'compaq-e500'), required=True)
    args = parser.parse_args()
    part = identified_partition()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    raw = OUTPUT / (args.machine + '-MEMMAP.BIN')
    report = OUTPUT / (args.machine + '.json')
    if raw.exists() or report.exists():
        raise RuntimeError('capture already exists; refusing to overwrite it')
    with tempfile.TemporaryDirectory(prefix='ciuki-e820-') as folder:
        temp = Path(folder) / 'MEMMAP.BIN'
        subprocess.run(['mcopy', '-o', '-i', str(part),
                        '::SYSTEM/MEMMAP.BIN', str(temp)], check=True)
        memory = read_map(temp, require_complete=True)
        if memory['overlapping_entries']:
            raise RuntimeError('firmware E820 entries overlap')
        if not 512 <= memory['conventional_kib'] <= 640:
            raise RuntimeError('unexpected conventional-memory boundary')
        if not 0x8000 <= memory['ebda_segment'] <= 0xA000:
            raise RuntimeError('unexpected EBDA segment')
        data = temp.read_bytes()
    with raw.open('xb') as stream:
        stream.write(data)
    summary = {
        'machine_reported_by_operator': args.machine,
        'captured_utc': datetime.now(timezone.utc).isoformat(),
        'disk_serial': 'TS64GMSA230S_H551120730',
        'raw_sha256': hashlib.sha256(data).hexdigest(),
        **memory,
    }
    with report.open('x') as stream:
        stream.write(json.dumps(summary, indent=2) + '\n')
    if os.environ.get('SUDO_UID') and os.environ.get('SUDO_GID'):
        owner = int(os.environ['SUDO_UID'])
        group = int(os.environ['SUDO_GID'])
        os.chown(raw, owner, group)
        os.chown(report, owner, group)
    print(f'Saved {raw} and {report}: '
          f'{memory["usable_above_1m_bytes"] / 1048576:.1f} MiB usable above 1 MiB')


if __name__ == '__main__':
    main()
