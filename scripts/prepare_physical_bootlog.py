#!/usr/bin/env python3
"""Prepare a CiukiOS HDD image with persistent, best-effort boot diagnostics."""

import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile


PARTITION_LBA = 63
PARTITION_SECTORS = 262144
PARTITION_BYTES = PARTITION_SECTORS * 512
PREFIX_BYTES = (PARTITION_LBA + PARTITION_SECTORS) * 512


def run(*args: str) -> None:
    subprocess.run(args, check=True)


def nasm(source: str, output: Path, *defines: str) -> bytes:
    run("nasm", "-f", "bin", source, *defines, "-o", str(output))
    return output.read_bytes()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True, type=Path,
                        help="previous known-good HARD-mode whole-disk prefix")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--desktop-app", type=Path,
                        help="matching DESKTOP.APP when shell events changed")
    args = parser.parse_args()
    if args.base.stat().st_size != PREFIX_BYTES:
        raise SystemExit("base prefix has the wrong size")
    if args.output.exists():
        raise SystemExit("output already exists; refusing to overwrite it")
    with args.base.open("rb") as source:
        old_mbr = source.read(512)
        source.seek(PARTITION_LBA * 512)
        old_vbr = source.read(512)
    if old_mbr[446:462] != bytes.fromhex("8001010006feffff3f00000000000400"):
        raise SystemExit("base partition table is not the expected active FAT16 layout")
    if old_mbr[510:] != b"\x55\xaa" or old_vbr[510:] != b"\x55\xaa":
        raise SystemExit("base boot signature missing")

    with tempfile.TemporaryDirectory(prefix="ciuki-bootlog-") as temp:
        temp = Path(temp)
        mbr = nasm("src/boot/full_cd_mbr.asm", temp / "mbr.bin",
                   "-D", "BOOT_DISK_LOG=1", "-D", "PARTITION_LBA=63",
                   "-D", "PARTITION_SECTORS=262144")
        vbr = nasm("src/boot/full_boot.asm", temp / "vbr.bin",
                   "-D", "BOOT_DISK_LOG=1", "-D", "BOOT_LBA_OFFSET=63",
                   "-D", "FAT_TOTAL_SECTORS=262144")
        stage1 = nasm("src/boot/full_stage1_loader.asm", temp / "stage1.bin",
                      "-D", "BOOT_DISK_LOG=1", "-D", "FAT_LBA_OFFSET=63",
                      "-D", "DOS_DEFAULT_DRIVE_INDEX=2")
        shell = nasm("src/com/shell.asm", temp / "shell.com",
                     "-D", "BOOT_DIAG_PALETTE=1", "-D", "BOOT_DISK_LOG=1")
        if len(mbr) != 512 or len(vbr) != 512 or len(stage1) > 4096:
            raise SystemExit("boot component exceeds its fixed disk slot")
        if mbr[0xE0] != 0x9C or mbr[446:] != old_mbr[446:]:
            raise SystemExit("MBR logger entry or partition table changed")
        if len(shell) + 0x100 > 0xEF00:
            raise SystemExit("SHELL.COM and stack exceed the module limit")

        part = temp / "partition.img"
        with args.base.open("rb") as source, part.open("wb") as target:
            source.seek(PARTITION_LBA * 512)
            shutil.copyfileobj(source, target, length=1024 * 1024)
        with part.open("r+b") as target:
            target.write(vbr)
            target.write(stage1.ljust(4096, b"\0"))
        (temp / "BOOT.LOG").write_bytes(b"")
        run("mcopy", "-o", "-i", str(part), str(temp / "shell.com"),
            "::SYSTEM/SHELL.COM")
        if args.desktop_app:
            run("mcopy", "-o", "-i", str(part), str(args.desktop_app),
                "::SYSTEM/APPS/DESKTOP.APP")
        run("mcopy", "-o", "-i", str(part), str(temp / "BOOT.LOG"),
            "::SYSTEM/BOOT.LOG")
        startup = subprocess.check_output(
            ["mtype", "-i", str(part), "::SYSTEM/STARTUP.CFG"])
        if startup != b"HARD":
            raise SystemExit(f"physical recovery profile changed: {startup!r}")
        run("fsck.fat", "-n", str(part))
        with args.output.open("xb") as output, part.open("rb") as contents:
            output.write(mbr)
            output.write(b"\0" * ((PARTITION_LBA - 1) * 512))
            shutil.copyfileobj(contents, output, length=1024 * 1024)

    if args.output.stat().st_size != PREFIX_BYTES:
        raise SystemExit("prepared prefix has the wrong size")
    h = hashlib.sha256()
    with args.output.open("rb") as image:
        for block in iter(lambda: image.read(1024 * 1024), b""):
            h.update(block)
    print(f"Prepared {args.output}\nSHA-256 {h.hexdigest()}")


if __name__ == "__main__":
    main()
