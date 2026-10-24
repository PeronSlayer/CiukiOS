"""G30 Resource Meter tray feed (apps/desktop/resources.c). Run with nice -n 19."""
# SPDX-License-Identifier: MIT
import os
from pathlib import Path
import subprocess
import sys
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]


class ResourcesTests(unittest.TestCase):
    def test_feed(self):
        out = ROOT / "build/host"
        out.mkdir(parents=True, exist_ok=True)
        binary = out / "resources_test"
        sources = [ROOT / "apps/desktop/resources.c", ROOT / "apps/systools/resmeter.c", ROOT / "tests/host/desktop/resources_test.c"]
        subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-D_POSIX_C_SOURCE=200809L",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                        "-include", str(ROOT / "tests/host/desktop/replay_base.h"),
                        "-I", str(ROOT / "apps/desktop"), "-I", str(ROOT / "apps/libdeskui"), "-I", str(ROOT / "src/kernel/include"),
                        *map(str, sources), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True,
                                env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"}, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS G30 Resource Meter tray feed", result.stdout)
        print(result.stdout, end="")


if __name__ == "__main__":
    unittest.main()
