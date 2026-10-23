"""Compile FAT/VFS/FDC integration; optional sibling FAT12 source, no copy."""
# SPDX-License-Identifier: GPL-2.0-only
from pathlib import Path
import hashlib
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
out = root / "build/host/floppy"
out.mkdir(parents=True, exist_ok=True)
fat_root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root
san = sys.argv[2] if len(sys.argv) > 2 else "address,undefined"
sources = ["src/kernel/fs/" + name + ".c" for name in
           ("fs_port", "blkreq", "cache", "partition", "path", "vfs", "fs_ro", "iso9660", "ntfs_fs", "ntfs_ro", "mount")]
sources += ["src/kernel/drivers/blkpart.c", "src/kernel/drivers/floppy.c",
            "src/kernel/drivers/floppy_media.c", "src/kernel/lib/fmt.c", "src/kernel/lib/sha256.c",
            "src/kernel/core/bootlog.c", "tests/host/floppy_fat_test.c"]
args = ["clang", "-DFS_HOST", "-D_POSIX_C_SOURCE=200809L", "-std=c17", "-O1", "-g",
        "-Wall", "-Wextra", "-Werror", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-pthread", "-fsanitize=" + san, "-I" + str(root / "src/kernel/include")]
args += [str(root / source) for source in sources]
args += [str(fat_root / "src/kernel/fs/fat.c"), "-o", str(out / "floppy_fat_test")]
fat = fat_root / "src/kernel/fs/fat.c"
header = fat_root / "src/kernel/fs/fat.h"
pins = [hashlib.sha256(path.read_bytes()).hexdigest() for path in (fat, header)]
subprocess.run(args, check=True)
assert pins == [hashlib.sha256(path.read_bytes()).hexdigest() for path in (fat, header)], "FAT source changed during compilation"
print(f"FAT source={fat} sha256={pins[0]} header_sha256={pins[1]}", flush=True)
image = out / "floppy-fat12.img"
with image.open("wb") as stream:
    stream.truncate(1440 * 1024)
subprocess.run(["mkfs.fat", "-F", "12", str(image)], check=True)
subprocess.run([str(out / "floppy_fat_test"), str(image)], check=True)
subprocess.run(["fsck.fat", "-n", str(image)], check=True)
subprocess.run(["mdir", "-i", str(image), "::KEEP.TXT"], check=True)
image.unlink()
