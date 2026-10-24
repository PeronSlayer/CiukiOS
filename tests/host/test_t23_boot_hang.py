"""T23 late USB storage replay, host fixtures only; no boot image or QEMU."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class T23BootStorageTests(unittest.TestCase):
    def test_late_fat32_and_publication_diagnostic(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="t23-boot-", dir=parent) as tmp:
            out = Path(tmp)
            env = {**os.environ, "TMPDIR": tmp, "ASAN_OPTIONS": "detect_leaks=0"}
            boot, usb = out / "boot.fat", out / "usb.fat"
            for path, size, spc in ((boot, 64 * 1024**2, 1), (usb, 31283200 * 512, 16)):
                with path.open("wb") as stream:
                    stream.truncate(size)  # sparse, never a copy of the boot image
                subprocess.run(["mkfs.fat", "-F", "32", "-s", str(spc), str(path)], check=True, env=env)
            sources = [f"src/kernel/fs/{s}.c" for s in
                       ("fs_port", "blkreq", "cache", "partition", "path", "fat", "vfs", "fs_ro",
                        "iso9660", "ntfs_fs", "ntfs_ro", "mount")]
            sources += ["src/kernel/core/bootlog.c", "src/kernel/drivers/blkpart.c",
                        "src/kernel/drivers/video/attempt.c", "src/kernel/probes/fat_probes.c",
                        "src/kernel/lib/sha256.c", "src/kernel/lib/fmt.c", "tests/host/fs/fake.c",
                        "tests/host/fs/scan.c", "tests/host/fs/t23_boot_storage_test.c"]
            binary = out / "storage"
            subprocess.run(["clang", "-DFS_HOST", "-D_POSIX_C_SOURCE=200809L", "-std=c17",
                            "-Wall", "-Wextra", "-Werror", "-g", "-O1", "-fsanitize=address,undefined",
                            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                            "-Wl,--wrap=cache_publish", "-pthread", "-Isrc/kernel/include", "-Isrc/kernel/fs",
                            *sources, "-o", str(binary)], check=True, cwd=ROOT, env=env)
            subprocess.run([str(binary), str(boot), str(usb)], check=True, env=env, timeout=30)


if __name__ == "__main__":
    unittest.main()
