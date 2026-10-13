"""Host-only libreg tests with real files and interrupted durable writes."""
# SPDX-License-Identifier: MIT
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]

class RegistryTests(unittest.TestCase):
    def test_registry(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="registry-", dir=parent) as scratch:
            work = Path(scratch)
            (work / "include").mkdir()
            (work / "include/ciuki").symlink_to(ROOT / "sdk/sysroot-overlay/include/ciuki", target_is_directory=True)
            directory = work / "registry"
            directory.mkdir()
            binary = work / "registry-test"
            env = dict(os.environ, TMPDIR=str(work), ASAN_OPTIONS="detect_leaks=0")
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-macro-redefined", "-fsanitize=address,undefined",
                            "-fno-sanitize-recover=undefined", "-I", str(work / "include"), "-I", str(ROOT / "src/kernel/include"),
                            str(ROOT / "tests/host/registry_test.c"), *map(str, sorted((ROOT / "sdk/libreg").glob("*.c"))),
                            "-Wl,--wrap=rename", "-Wl,--wrap=fsync", "-Wl,--wrap=fopen", "-o", str(binary)], check=True, env=env)
            subprocess.run([str(binary), str(directory)], check=True, env=env)

if __name__ == "__main__":
    unittest.main()
