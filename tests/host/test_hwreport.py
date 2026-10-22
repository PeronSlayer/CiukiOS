"""f9-07 diagnostic formatting and export failure fixtures; no guest image."""
# SPDX-License-Identifier: MIT
from pathlib import Path
import gzip
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class HardwareReportTests(unittest.TestCase):
    def test_smbios_identity(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="hwreport-smbios-", dir=parent) as temp:
            binary = Path(temp) / "smbios"
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-fsanitize=" + os.environ.get("CIUKI_HWREPORT_SANITIZERS", "address,undefined"),
                "-fno-sanitize-recover=undefined", "-I", str(ROOT / "src/kernel/include"),
                str(ROOT / "tests/host/hwreport_smbios_test.c"), "-o", str(binary)], check=True, env={**os.environ, "TMPDIR": temp})
            subprocess.run([binary], check=True, env={**os.environ, "TMPDIR": temp})

    def test_native_files_and_archive(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="hwreport-native-", dir=parent) as temp:
            binary = Path(temp) / "native"
            includes = Path(temp) / "include"
            shutil.copytree(ROOT / "sdk/sysroot-overlay/include/ciuki", includes / "ciuki")
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-D_POSIX_C_SOURCE=200809L", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-fsanitize=" + os.environ.get("CIUKI_HWREPORT_SANITIZERS", "address,undefined"), "-fno-sanitize-recover=undefined",
                "-I", str(includes), "-I", str(ROOT / "src/kernel/include"),
                str(ROOT / "tests/host/hwreport_native_test.c"), str(ROOT / "apps/hwreport/report.c"),
                str(ROOT / "apps/hwreport/archive.c"), "-o", str(binary)], check=True, env={**os.environ, "TMPDIR": temp})
            subprocess.run([binary, temp], check=True, env={**os.environ, "TMPDIR": temp})
            logs = gzip.decompress((Path(temp) / "CIUKILOG.GZ").read_bytes())
            self.assertEqual(logs.count(b"[ata] count=2 error=-5"), 28000)
            self.assertIn(b"[cpu] pages=123", logs)
            self.assertNotIn(b"private", logs)

    def test_format_and_export(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="hwreport-", dir=parent) as temp:
            binary = Path(temp) / "report"
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=" + os.environ.get("CIUKI_HWREPORT_SANITIZERS", "address,undefined"), "-fno-sanitize-recover=undefined",
                "-I", str(ROOT / "src/kernel/include"), str(ROOT / "tests/host/hwreport_test.c"),
                str(ROOT / "apps/hwreport/report.c"), str(ROOT / "apps/hwreport/archive.c"), "-o", str(binary)], check=True, env={**os.environ, "TMPDIR": temp})
            archive = Path(temp) / "logs.gz"
            subprocess.run([binary, archive], check=True, env={**os.environ, "TMPDIR": temp})
            self.assertEqual(gzip.decompress(archive.read_bytes()), Path(str(archive) + ".txt").read_bytes())
            self.assertNotIn(b"secret", gzip.decompress(archive.read_bytes()))

    def test_app_compiles(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="hwreport-app-", dir=parent) as temp:
            includes = Path(temp) / "include"
            shutil.copytree(ROOT / "sdk/sysroot-overlay/include/ciuki", includes / "ciuki")
            for source in ("main.c", "native.c", "archive.c"):
                subprocess.run(["clang", "-std=c17", "-Wall", "-Wextra", "-Werror", "-D_POSIX_C_SOURCE=200809L",
                    "-include", str(ROOT / "tests/host/desktop/replay_base.h"),
                    "-I", str(includes), "-I", str(ROOT / "src/kernel/include"), "-fsyntax-only",
                    str(ROOT / "apps/hwreport" / source)], check=True, env={**os.environ, "TMPDIR": temp})

if __name__ == "__main__":
    unittest.main()
