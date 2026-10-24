"""G14 Registry Checker, System File Checker and ScanDisk report (host only)."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SysRecoveryTests(unittest.TestCase):
    def test_registry_checker_sfc_and_report(self):
        base = ROOT / "build/host"
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="sysrecovery-", dir=base) as scratch:
            work = Path(scratch)
            (work / "include").mkdir()
            (work / "include/ciuki").symlink_to(ROOT / "sdk/sysroot-overlay/include/ciuki", target_is_directory=True)
            fixture = work / "drive"
            fixture.mkdir()
            binary = work / "sysrecovery-test"
            env = dict(os.environ, TMPDIR=str(work), ASAN_OPTIONS="detect_leaks=0")
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-macro-redefined",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                            "-I", str(work / "include"), "-I", str(ROOT / "src/kernel/include"),
                            str(ROOT / "tests/host/sysrecovery_test.c"),
                            *map(str, sorted((ROOT / "apps/sysrecovery").glob("*.c"))),
                            str(ROOT / "apps/accessories/msconfig_model.c"), "-I", str(ROOT / "apps/desktop"),
                            *map(str, sorted((ROOT / "sdk/libreg").glob("*.c"))),
                            str(ROOT / "src/kernel/lib/sha256.c"), "-o", str(binary)], check=True, env=env)
            result = subprocess.run([str(binary), str(fixture)], capture_output=True, text=True, env=env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("sysrecovery: 0 failures", result.stdout)


if __name__ == "__main__":
    unittest.main()
