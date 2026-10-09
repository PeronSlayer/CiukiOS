#!/usr/bin/env python3
"""Copy bounded CiukiOS diagnostics from an already mounted physical volume.

The source is only read. Mount the disk read-only before running this tool;
the manifest records hashes and missing optional records, including old builds.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

from inspect_boot_hardware import parse_cache_diagnostics, parse_log, parse_gpu_log
from inspect_dosvm_snapshot import parse_snapshot
from inspect_vmfork_launch import parse_launch
from inspect_disk_ata import parse_disk_status, parse_disk_binding
from read_dos_memory import parse_record as parse_dos_memory
from inspect_dos_audio_timing import parse_audio_timing
from inspect_input_snapshot import parse_input_snapshot

PATHS = (
    'DESKTOP/TestGames/DOOM.COM', 'DESKTOP/TestGames/DOOMVAN.COM',
    'DESKTOP/TestGames/WOLF3D.COM', 'DRIVERS/ACTIVE.CFG', 'DRIVERS/DRIVERS.CFG',
    'DRIVERS/LOADDRV.LOG', 'SYSTEM/AUDIO.LOG', 'SYSTEM/BOOT.LOG',
    'SYSTEM/DISPLAY.LOG', 'SYSTEM/DPMIRUN.LOG', 'SYSTEM/DOSVM.LOG', 'SYSTEM/DOSVM1.BIN',
    'SYSTEM/DOSVM2.BIN', 'SYSTEM/DOSVM3.BIN', 'SYSTEM/DRIVERS/SFX.DRV',
    'SYSTEM/VMFORK1.BIN', 'SYSTEM/VMFORK2.BIN', 'SYSTEM/VMFORK3.BIN',
    'SYSTEM/INPUT0.BIN', 'SYSTEM/INPUT1.BIN', 'SYSTEM/INPUT2.BIN',
    'SYSTEM/DPMIRUN1.BIN', 'SYSTEM/DPMIRUN2.BIN', 'SYSTEM/DPMIRUN3.BIN',
    'SYSTEM/MEMMAP.BIN', 'SYSTEM/DOSMEM.BIN', 'SYSTEM/SHELL.COM', 'SYSTEM/STARTUP.CFG',
    'SYSTEM/APPS/DOSVM.APP',
    'SYSTEM/VOLUME.CFG',
    'SYSTEM/DISKATA.BIN',
    'SYSTEM/DISKBIND.BIN',
    'SYSTEM/UI/WALL.CFG', 'SYSTEM/VIDEO/DISPLAY.CFG',
    'SYSTEM/VIDEO/DISPLAY.LOG', 'SYSTEM/VIDEO/GPU.LOG',
    'SYSTEM/VIDEO/GPU3D.LOG', 'SYSTEM/VIDEO/S3QUAL.LOG', 'SYSTEM/VIDEO/ATIQUAL.LOG', 'SYSTEM/VIDEO/VBE.TRC',
    'SYSTEM/VIDEO/CACHE.LOG', 'VM/CVSESS.DLL', 'VM/DPMIRUN.COM', 'VM/VMFORK.COM',
    'VM/VMSTART.COM',
    'VM/VMFORK.EXE',
) + tuple(f'SYSTEM/DOSR{vm}{generation}.BIN'
          for vm in range(1, 4) for generation in range(8)) + tuple(
          f'SYSTEM/DOSA{vm}{generation}.BIN' for vm in range(1, 4) for generation in range(8))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mount', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    source = args.mount.resolve(strict=True)
    if not (source / 'SYSTEM/SHELL.COM').is_file():
        parser.error('--mount must be the root of a mounted CiukiOS volume')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    manifest = dict(acquired_utc=datetime.now(timezone.utc).isoformat(),
                    source=str(source), source_writes=False, files=[], missing=[], decoded={})
    mount = subprocess.run(['findmnt', '-T', str(source), '-n', '-o',
                            'SOURCE,OPTIONS'], capture_output=True, text=True, check=False)
    manifest['mount_info'] = mount.stdout.strip()
    decoders = {'SYSTEM/VIDEO/DISPLAY.LOG': parse_log,
                'SYSTEM/VIDEO/GPU.LOG': parse_gpu_log,
                'SYSTEM/VIDEO/GPU3D.LOG': parse_gpu_log,
                'SYSTEM/VIDEO/S3QUAL.LOG': parse_gpu_log,
                'SYSTEM/VIDEO/ATIQUAL.LOG': parse_gpu_log,
                'SYSTEM/VIDEO/CACHE.LOG': parse_cache_diagnostics,
                'SYSTEM/DISKATA.BIN': parse_disk_status,
                'SYSTEM/DISKBIND.BIN': parse_disk_binding,
                'SYSTEM/DOSMEM.BIN': parse_dos_memory}
    decoders.update({f'SYSTEM/DOSVM{i}.BIN': parse_snapshot for i in range(1, 4)})
    decoders.update({f'SYSTEM/INPUT{i}.BIN': parse_input_snapshot for i in range(3)})
    decoders.update({f'SYSTEM/DOSR{vm}{generation}.BIN': parse_snapshot
                     for vm in range(1, 4) for generation in range(8)})
    decoders.update({f'SYSTEM/DOSA{vm}{generation}.BIN': parse_audio_timing
                     for vm in range(1, 4) for generation in range(8)})
    decoders.update({f'SYSTEM/VMFORK{i}.BIN': parse_launch for i in range(1, 4)})
    decoders.update({f'SYSTEM/DPMIRUN{i}.BIN': parse_launch for i in range(1, 4)})
    for name in PATHS:
        origin = source / name
        if not origin.exists():
            manifest['missing'].append(name)
            continue
        if not origin.is_file() or not origin.resolve().is_relative_to(source):
            raise ValueError(f'Unexpected source path: {name}')
        limit = 1048576 if name.endswith('.DLL') else 131072
        if origin.stat().st_size > limit:
            raise ValueError(f'Diagnostic exceeds its bounded size: {name}')
        target = output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(origin, target)
        data = target.read_bytes()
        manifest['files'].append(dict(path=name, bytes=len(data),
                                     sha256=hashlib.sha256(data).hexdigest(),
                                     source_mtime_utc=datetime.fromtimestamp(
                                         origin.stat().st_mtime, timezone.utc).isoformat()))
        if name in decoders:
            try:
                manifest['decoded'][name] = decoders[name](data)
            except (ValueError, IndexError) as error:
                manifest['decoded'][name] = dict(error=str(error))
    for index in range(1, 4):
        launch = manifest['decoded'].get(f'SYSTEM/VMFORK{index}.BIN', {})
        session = manifest['decoded'].get(f'SYSTEM/DOSVM{index}.BIN', {})
        if launch.get('format') == 'CVFL' and session.get('format') == 'CDVS':
            launch['matches_dosvm_generation'] = (
                launch['vm'] == session['vm'] and launch['generation'] == session['generation'])
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Copied {len(manifest["files"])} files; missing {len(manifest["missing"])} optional records.')


if __name__ == '__main__':
    main()
