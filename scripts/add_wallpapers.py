#!/usr/bin/env python3
"""Prepare extra CiukiOS wallpaper tiles without rebuilding or modifying the OS.

Copy the generated WALLnn.CWP files to \\SYSTEM\\UI using Files, then select
Wallpaper > Refresh. --image only reads the catalog to choose unused names.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
from pathlib import Path

from build_wallpapers import MAX_WALLPAPERS, convert, read_catalog


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sources", nargs="+", type=Path, help="PNG/BMP native tiles, at most 256x256 and 256 colors")
    parser.add_argument("--output", required=True, type=Path)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--image", type=Path, help="read-only FAT disk image used to find the next available number")
    group.add_argument("--after", type=int, help="last installed number: 11 means new tiles begin at WALL12.CWP")
    args = parser.parse_args()
    try:
        count = args.after
        if args.image is not None:
            if not args.image.is_file():
                raise ValueError("--image must name a regular FAT image file")
            result = subprocess.run(["mtype", "-i", str(args.image), "::SYSTEM/UI/WALLS.DAT"], capture_output=True)
            if result.returncode:
                raise ValueError("Could not read the wallpaper catalog: " + result.stderr.decode(errors="replace").strip())
            count = len(read_catalog(result.stdout))
            # Imported tiles are intentionally discovered without rewriting the
            # catalog. Include these consecutive names when finding a new slot.
            while count < MAX_WALLPAPERS:
                result = subprocess.run(["mtype", "-i", str(args.image), f"::SYSTEM/UI/WALL{count+1:02}.CWP"], capture_output=True)
                if result.returncode:
                    break
                count += 1
        if count is None or not 0 <= count <= MAX_WALLPAPERS:
            raise ValueError("--after must be between 0 and 99")
        if count + len(args.sources) > MAX_WALLPAPERS:
            raise ValueError(f"Only {MAX_WALLPAPERS-count} wallpaper slots remain")
        converted = [(source, *convert(source)) for source in args.sources]
        outputs = [(args.output / f"WALL{count+index:02}.CWP", source, payload, dimensions)
                   for index, (source, payload, dimensions) in enumerate(converted, 1)]
        for target, *_ in outputs:
            if target.exists():
                raise ValueError(f"Refusing to overwrite {target}; choose an empty output directory")
        args.output.mkdir(parents=True, exist_ok=True)
        manifest = {"format": "CiukiOS append-only wallpaper pack", "files": [],
                    "image_modified": False, "license": "Input artwork rights remain with its owner"}
        for target, source, payload, dimensions in outputs:
            target.write_bytes(payload)
            manifest["files"].append({"filename": target.name, "source": str(source),
                                     "dimensions": dimensions, "sha256": hashlib.sha256(payload).hexdigest()})
        (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        (args.output / "README.TXT").write_text(
            "Copy these WALLnn.CWP files to \\SYSTEM\\UI using Files.\n"
            "Open Programs > System > Wallpaper, then click Refresh.\n"
            "Select the imported tile and click Apply. No reboot is needed.\n"
            "Keep file numbers consecutive; do not replace WALLS.DAT or existing tiles.\n"
            "PNG/BMP originals are not directly decoded by the DOS renderer.\n")
        print(f"Prepared {len(outputs)} tile(s) starting at WALL{count+1:02}.CWP in {args.output}; no disk image was modified")
    except (ValueError, OSError) as error:
        raise SystemExit(str(error)) from error


if __name__ == "__main__":
    main()
