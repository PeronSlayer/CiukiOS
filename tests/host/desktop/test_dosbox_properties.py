"""G17 host evidence: production profiles, controls, clipboard and paste loop."""
# SPDX-License-Identifier: MIT
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class DOSPropertiesTests(unittest.TestCase):
    def test_properties_and_window_controls(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="g17-", dir=parent) as tmp:
            work = Path(tmp)
            inc = work / "include/ciuki"
            inc.mkdir(parents=True)
            for name in ("dosvm.h", "channel.h", "surface.h"):
                (inc / name).write_bytes((ROOT / "sdk/sysroot-overlay/include/ciuki" / name).read_bytes())
            (inc / "desktop.h").write_bytes((ROOT / "apps/desktop/protocol.h").read_bytes())
            binary = work / "properties"
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                            '-DDOSBOX_SETTINGS_DIR="' + str(work / "prefs") + '"',
                            "-I", str(work / "include"), "-I", str(ROOT / "src/kernel/include"),
                            str(ROOT / "tests/host/desktop/dosbox_properties_test.c"),
                            str(ROOT / "apps/libdeskui/widgets.c"), "-o", str(binary)], check=True)
            result = subprocess.run([binary], capture_output=True, text=True, timeout=30)
            print(result.stdout, end="", flush=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS G17 DOS program properties and window controls", result.stdout)


if __name__ == "__main__":
    unittest.main()
