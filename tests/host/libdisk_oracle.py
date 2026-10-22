#!/usr/bin/env python3
"""f9-02 oracles for tests/host/libdisk_test.c, built from scripts/build_image.py.

For each disk size of the directive (1, 4, 40 and 200 GiB, the last capped at
the 28-bit partition limit) this writes, under --out:
  mbr-<name>.bin     the build_image.py MBR for a disk of that size: mbr.asm
                     code, the "CIUK" signature, entry 1 with the saturated
                     CHS markers and count = min(capacity, 2^28) - 2048;
  oracle-<name>.bin  sectors 0-8, both FATs' first sectors and the root
                     directory's first sector of `mkfs.fat` run with the
                     exact build_image.py arguments (plus --invariant) on a
                     file of the partition's size;
and, for 1 GiB only, volume-1g.img: the complete fresh volume after
build_image.initialize_blackbox(). loader.bin is ciukldr.asm assembled and
prepared by build_image.prepare_loader(). Large mkfs outputs are deleted as
soon as their sectors are extracted (the fs suite's 200 MB artifact rule).
"""
# SPDX-License-Identifier: GPL-2.0-only
import argparse
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
SIZES = (("1g", 2097152), ("4g", 8388608), ("40g", 83886080), ("200g", 419430400))
LBA28 = 1 << 28


def load_build_image():
    spec = importlib.util.spec_from_file_location("build_image", ROOT / "scripts/build_image.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def guard(source):
    """Fail when build_image.py stops writing what this oracle reproduces."""
    for needle in ('struct.pack_into("<I", mbr, 440, 0x4B554943)',
                   'struct.pack_into("<B3sB3sII", mbr, 446, 0x80, b"\\x20\\x21\\0", 0x0C,',
                   'b"\\xfe\\xff\\xff", PART_LBA,',
                   'run("mkfs.fat", "--invariant", "-a", "-F", "32", "-S", "512", "-s", "8",',
                   '"-f", "2", "-R", str(FAT_RESERVED), "-b", "6", "-h", str(PART_LBA),',
                   '"-n", "CIUKIOS", volume)',
                   'initialize_blackbox(volume)'):
        if needle not in source:
            raise SystemExit(f"build_image.py drifted from the libdisk oracle: {needle}")


def mkfs(bi, volume):
    subprocess.run(["mkfs.fat", "--invariant", "-a", "-F", "32", "-S", "512", "-s", "8",
                    "-f", "2", "-R", str(bi.FAT_RESERVED), "-b", "6", "-h", str(bi.PART_LBA),
                    "-n", "CIUKIOS", str(volume)], check=True, stdout=subprocess.DEVNULL)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    bi = load_build_image()
    guard((ROOT / "scripts/build_image.py").read_text())
    sector = bi.SECTOR
    mbr_path, loader_path = out / "mbr-code.bin", out / "ciukldr.bin"
    subprocess.run(["nasm", "-f", "bin", str(ROOT / "src/boot/mbr.asm"), "-o", str(mbr_path)], check=True)
    subprocess.run(["nasm", "-f", "bin", str(ROOT / "src/boot/ciukldr.asm"), "-o", str(loader_path)], check=True)
    (out / "loader.bin").write_bytes(bi.prepare_loader(loader_path.read_bytes()))
    loader_path.unlink()
    code = mbr_path.read_bytes()
    mbr_path.unlink()
    for name, capacity in SIZES:
        count = min(capacity, LBA28) - bi.PART_LBA
        mbr = bytearray(code)
        struct.pack_into("<I", mbr, 440, 0x4B554943)
        struct.pack_into("<B3sB3sII", mbr, 446, 0x80, b"\x20\x21\0", 0x0C, b"\xfe\xff\xff", bi.PART_LBA, count)
        (out / f"mbr-{name}.bin").write_bytes(mbr)
        volume = out / f"volume-{name}.img"
        volume.unlink(missing_ok=True)
        with volume.open("wb") as stream:
            stream.truncate(count * sector)
        mkfs(bi, volume)
        bi.initialize_blackbox(volume)
        with volume.open("rb") as stream:
            head = stream.read(9 * sector)
            fat = struct.unpack_from("<I", head, 36)[0]
            parts = [head]
            for lba in (bi.FAT_RESERVED, bi.FAT_RESERVED + fat, bi.FAT_RESERVED + 2 * fat):
                stream.seek(lba * sector)
                parts.append(stream.read(sector))
        (out / f"oracle-{name}.bin").write_bytes(b"".join(parts))
        if name != "1g":
            volume.unlink()
        print(f"ORACLE {name} capacity={capacity} part={count} fat={fat} "
              f"clusters={(count - bi.FAT_RESERVED - 2 * fat) // 8}")
    sys.stdout.flush()


if __name__ == "__main__":
    main()
