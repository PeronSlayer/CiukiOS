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

PATHS = (
    'DESKTOP/TestGames/DOOM.COM', 'DESKTOP/TestGames/DOOMVAN.COM',
    'DESKTOP/TestGames/WOLF3D.COM', 'DRIVERS/ACTIVE.CFG', 'DRIVERS/DRIVERS.CFG',
    'DRIVERS/LOADDRV.LOG', 'SYSTEM/AUDIO.LOG', 'SYSTEM/BOOT.LOG',
    'SYSTEM/DISPLAY.LOG', 'SYSTEM/DOSVM.LOG', 'SYSTEM/DOSVM1.BIN',
    'SYSTEM/DOSVM2.BIN', 'SYSTEM/DOSVM3.BIN', 'SYSTEM/DRIVERS/SFX.DRV',
    'SYSTEM/MEMMAP.BIN', 'SYSTEM/SHELL.COM', 'SYSTEM/STARTUP.CFG',
    'SYSTEM/UI/WALL.CFG', 'SYSTEM/VIDEO/DISPLAY.CFG',
    'SYSTEM/VIDEO/DISPLAY.LOG', 'SYSTEM/VIDEO/GPU.LOG',
    'SYSTEM/VIDEO/GPU3D.LOG', 'SYSTEM/VIDEO/VBE.TRC',
    'SYSTEM/VIDEO/CACHE.LOG', 'VM/CVSESS.DLL',
)


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
                'SYSTEM/VIDEO/CACHE.LOG': parse_cache_diagnostics}
    decoders.update({f'SYSTEM/DOSVM{i}.BIN': parse_snapshot for i in range(1, 4)})
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
                                     sha256=hashlib.sha256(data).hexdigest()))
        if name in decoders:
            try:
                manifest['decoded'][name] = decoders[name](data)
            except (ValueError, IndexError) as error:
                manifest['decoded'][name] = dict(error=str(error))
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Copied {len(manifest["files"])} files; missing {len(manifest["missing"])} optional records.')


if __name__ == '__main__':
    main()
