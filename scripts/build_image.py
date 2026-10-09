#!/usr/bin/env python3
"""Build and statically verify the canonical Ciuki F0 MBR/FAT32 disk."""
# SPDX-License-Identifier: GPL-2.0-only
import argparse
import hashlib
import errno
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
SECTOR = 512
PART_LBA = 2048
IMAGE_BYTES = 512 * 1024 * 1024
HEADER = struct.Struct("<4sHHIHH")
DEFAULT_CONFIG = b"safe=0 serial=1\n"


def run(*args):
    subprocess.run([str(a) for a in args], cwd=ROOT, check=True)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def prepare_loader(raw):
    loader = bytearray(raw)
    require(16 < len(loader) <= 1023 * SECTOR and len(loader) % SECTOR == 0,
            "loader size")
    magic, version, _, _, entry, length = HEADER.unpack_from(loader)
    require(magic == b"CLDR" and version == 1 and length == 16, "loader header")
    require(16 <= entry < len(loader), "loader entry")
    struct.pack_into("<H", loader, 6, len(loader) // SECTOR)
    struct.pack_into("<I", loader, 8, zlib.crc32(loader[16:]))
    return loader


def check_image(image, volume):
    with image.open("rb") as stream:
        mbr = stream.read(SECTOR)
        require(len(mbr) == SECTOR and mbr[510:] == b"\x55\xaa", "MBR signature")
        parts = [mbr[446+i*16:462+i*16] for i in range(4)]
        require(parts[0][0] == 0x80 and parts[0][4] == 0x0C, "active FAT32 LBA")
        start, count = struct.unpack_from("<II", parts[0], 8)
        require(start == PART_LBA and count == IMAGE_BYTES // SECTOR - start, "FAT32 BPB")
        require(all(p == bytes(16) for p in parts[1:]), "single partition")
        magic, version, sectors, crc, entry, length = HEADER.unpack(stream.read(16))
        require(magic == b"CLDR" and version == 1 and length == 16, "loader header")
        require(1 <= sectors <= 1023 and 16 <= entry < sectors * SECTOR, "loader extent")
        payload = stream.read(sectors * SECTOR - length)
        require(len(payload) == sectors * SECTOR - length, "FAT32 BPB")
        require(zlib.crc32(payload) == crc, "loader CRC32")
        gap = stream.read((PART_LBA - 1 - sectors) * SECTOR)
        require(gap == bytes(len(gap)), "reserved disk gap")
        bpb = stream.read(SECTOR)
    bps, spc, reserved, fats = struct.unpack_from("<HBHB", bpb, 11)
    require((bps, spc, reserved, fats) == (512, 8, 32, 2), "FAT32 geometry")
    require(struct.unpack_from("<H", bpb, 17)[0] == 0, "FAT32 BPB")
    require(struct.unpack_from("<H", bpb, 22)[0] == 0, "FAT32 BPB")
    total = struct.unpack_from("<I", bpb, 32)[0]
    fat_size = struct.unpack_from("<I", bpb, 36)[0]
    require(total == count, "partition/BPB size")
    require((total - reserved - fats * fat_size) // spc >= 65525, "FAT32 cluster count")
    require(struct.unpack_from("<I", bpb, 44)[0] == 2, "root cluster")
    require(struct.unpack_from("<HH", bpb, 48) == (1, 6), "FSInfo/backup locations")
    require(bpb[510:] == b"\x55\xaa", "FAT32 BPB")
    with volume.open("rb") as stream:
        stream.seek(SECTOR)
        fsinfo = stream.read(SECTOR)
        require(struct.unpack_from("<I", fsinfo)[0] == 0x41615252, "FAT32 BPB")
        require(struct.unpack_from("<I", fsinfo, 484)[0] == 0x61417272, "FAT32 BPB")
        require(fsinfo[508:] == b"\0\0\x55\xaa", "FAT32 BPB")
        stream.seek(6 * SECTOR)
        require(stream.read(SECTOR) == bpb, "backup BPB")
        require(stream.read(SECTOR) == fsinfo, "backup FSInfo")
    run("fsck.fat", "-n", volume)
    print("T1 PASS: MBR, FAT32 geometry, fsck, loader size and CRC32")


def build(kernel, out, boot_cfg):
    for tool in ("nasm", "mkfs.fat", "mmd", "mcopy", "fsck.fat"):
        if not shutil.which(tool):
            raise SystemExit(f"missing host tool: {tool}")
    if not kernel.is_file():
        raise SystemExit(f"kernel not found: {kernel}")
    options = boot_cfg.read_bytes() if boot_cfg else DEFAULT_CONFIG
    if len(options) > 127 or b"\0" in options or any(c > 127 for c in options):
        raise SystemExit("BOOT.CFG must be <=127 ASCII bytes without NUL")
    out.parent.mkdir(parents=True, exist_ok=True)
    # Large scratch files stay on disk beside the output, never in /tmp.
    with tempfile.TemporaryDirectory(prefix=".image-", dir=out.parent) as directory:
        scratch = Path(directory)
        mbr_path = scratch / "mbr.bin"
        loader_path = scratch / "ciukldr.bin"
        volume = scratch / "system.fat"
        cfg = scratch / "BOOT.CFG"
        cfg.write_bytes(options)
        run("nasm", "-f", "bin", ROOT / "src/boot/mbr.asm", "-o", mbr_path)
        run("nasm", "-f", "bin", ROOT / "src/boot/ciukldr.asm", "-o", loader_path)
        loader = prepare_loader(loader_path.read_bytes())
        loader_path.write_bytes(loader)
        mbr = bytearray(mbr_path.read_bytes())
        require(len(mbr) == 512 and mbr[510:] == b"\x55\xaa", "MBR signature")
        struct.pack_into("<I", mbr, 440, 0x4B554943)  # Stable "CIUK" disk signature.
        # CHS values are conventional saturated markers; boot uses LBA.
        struct.pack_into("<B3sB3sII", mbr, 446, 0x80, b"\x20\x21\0", 0x0C,
                         b"\xfe\xff\xff", PART_LBA,
                         IMAGE_BYTES // SECTOR - PART_LBA)
        partition_bytes = IMAGE_BYTES - PART_LBA * SECTOR
        with volume.open("wb") as stream:
            stream.truncate(partition_bytes)
        run("mkfs.fat", "--invariant", "-a", "-F", "32", "-S", "512", "-s", "8",
            "-f", "2", "-R", "32", "-b", "6", "-h", str(PART_LBA),
            "-n", "CIUKIOS", volume)
        run("mmd", "-i", volume, "::/SYSTEM")
        run("mcopy", "-i", volume, kernel, "::/SYSTEM/VMM.ELF")
        run("mcopy", "-i", volume, cfg, "::/SYSTEM/BOOT.CFG")
        # mtools updates the primary allocation hint but not its backup.
        with volume.open("r+b") as stream:
            stream.seek(SECTOR)
            fsinfo = stream.read(SECTOR)
            stream.seek(7 * SECTOR)
            stream.write(fsinfo)
        # Copy allocated extents, retaining sparsity of the FAT volume.
        candidate = scratch / "ciukios.img"
        with candidate.open("wb") as dest, volume.open("rb") as source:
            dest.truncate(IMAGE_BYTES)
            dest.write(mbr)
            dest.write(loader)
            cursor = 0
            while cursor < partition_bytes:
                try:
                    data = os.lseek(source.fileno(), cursor, os.SEEK_DATA)
                except OSError as exc:
                    if exc.errno == errno.ENXIO:  # ENXIO: no further allocated extent.
                        break
                    raise
                hole = min(os.lseek(source.fileno(), data, os.SEEK_HOLE), partition_bytes)
                source.seek(data)
                dest.seek(PART_LBA * SECTOR + data)
                while data < hole:
                    block = source.read(min(1024 * 1024, hole - data))
                    if not block:
                        raise RuntimeError("short volume read")
                    dest.write(block)
                    data += len(block)
                cursor = hole
        check_image(candidate, volume)
        os.replace(candidate, out)
    image_sha = sha256(out)
    write_manifest(out, kernel, image_sha)
    print(f"SHA256 {image_sha}  {out}")


def git_identity():
    """Commit and dirty state of the sources that produce the image."""
    try:
        rev = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"],
                             capture_output=True, text=True, check=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", str(ROOT), "status", "--porcelain", "--untracked-files=all",
                                "--", "src", "scripts", "config", "tests", "Makefile"],
                               capture_output=True, text=True, check=True).stdout.strip()
        return rev, bool(dirty)
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "unknown", "unknown"


def write_manifest(image, kernel, image_sha):
    """build-manifest.json beside the image: read by scripts/test/run.py."""
    rev, dirty = git_identity()
    manifest = {
        "schema_version": 1,
        "git_revision": rev,
        "git_dirty": dirty,
        "image": image.name,
        "image_sha256": image_sha,
        "kernel": str(kernel),
        "kernel_sha256": sha256(kernel),
    }
    (image.parent / "build-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=ROOT / "build/f0/ciukios.img")
    parser.add_argument("--boot-cfg", type=Path)
    args = parser.parse_args()
    build(args.kernel.resolve(), args.out.resolve(),
          args.boot_cfg.resolve() if args.boot_cfg else None)


if __name__ == "__main__":
    main()
