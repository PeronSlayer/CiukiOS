"""G31 shared native spool and production Printers UI host tests."""
# SPDX-License-Identifier: MIT
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
class PrintingTests(unittest.TestCase):
    def run_case(self, ui):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="printing-", dir=parent) as scratch:
            sources = ["tests/host/desktop/printers_test.c" if ui else "tests/host/printing_test.c", "apps/libprint/print.c"]
            if ui:
                sources += ["apps/libdeskui/" + s + ".c" for s in ("draw", "widgets", "list", "menu", "dialog", "textarea", "icons", "icon_map")]
                sources += ["apps/desktop/compositor.c", "apps/desktop/theme.c"]
            else:
                sources += ["apps/libprint/render.c"]
            binary = Path(scratch) / "printing"
            flags = ["clang", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", "-pthread", "-I", str(ROOT / "src/kernel/include")]
            env = dict(os.environ, TMPDIR=scratch)
            subprocess.run([*flags, *(str(ROOT / s) for s in sources), "-o", str(binary)], check=True, env=env)
            subprocess.run([str(binary)], check=True, env=env)
    def test_spool_and_drivers(self):
        self.run_case(False)
    def test_printers_ui(self):
        self.run_case(True)
if __name__ == "__main__":
    unittest.main()
