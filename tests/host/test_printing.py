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
                sources += ["apps/libdeskui/" + s + ".c" for s in ("draw", "widgets", "list", "menu", "dialog", "textarea", "icons", "icon_map", "iconx", "fileicon")]
                sources += ["apps/desktop/compositor.c", "apps/desktop/theme.c"]
            else:
                sources += ["apps/libprint/render.c"]
            binary = Path(scratch) / "printing"
            flags = ["clang", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", "-pthread", "-I", str(ROOT / "src/kernel/include")]
            env = dict(os.environ, TMPDIR=scratch)
            if not ui:
                flags.append("-Wl,--wrap=open")  # printing_test.c counts the library's opens
            subprocess.run([*flags, *(str(ROOT / s) for s in sources), "-o", str(binary)], check=True, env=env)
            subprocess.run([str(binary)], check=True, env=env)
    def spool_wait(self, spool_source):
        """Build apps/printers/spool.c (or a given source) over libprint, in a scratch queue."""
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="spool-wait-", dir=parent) as scratch:
            shim = Path(scratch) / "shim/ciuki"
            shim.mkdir(parents=True)
            # The target header's declarations; ciuki/abi.h itself collides with the host's signal.h.
            (shim / "channel.h").write_text("#include <stdint.h>\n"
                                            "struct ciuki_timespec { int64_t tv_sec; int32_t tv_nsec; uint32_t reserved; };\n"
                                            "int ciuki_wait_word(uint32_t *,uint32_t,const struct ciuki_timespec *,int);\n")
            binary = Path(scratch) / "spool-wait"
            root = Path(scratch) / "spool"
            subprocess.run(["clang", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-pthread",
                            f'-DCP_ROOT="{root}"', "-DCP_RETRY_MS=300u", "-DCP_IDLE_RESCAN_MS=1000u", "-Dmain=printspool_main",
                            "-Wl,--wrap=opendir", "-Wl,--wrap=closedir", "-Wl,--wrap=kill",
                            "-I", str(Path(scratch) / "shim"), "-I", str(ROOT / "src/kernel/include"),
                            str(spool_source), str(ROOT / "apps/libprint/print.c"),
                            str(ROOT / "tests/host/print_spool_wait_test.c"), "-o", str(binary)],
                           check=True, env=dict(os.environ, TMPDIR=scratch))
            return subprocess.run([str(binary)], env=dict(os.environ, TMPDIR=scratch), timeout=60,
                                  capture_output=True, text=True)
    def test_spool_waits_for_jobs(self):
        # The test scales the intervals; production keeps the 15 s retry and the 60 s safety rescan.
        header = (ROOT / "apps/libprint/print.h").read_text()
        self.assertIn("#define CP_RETRY_MS 15000u", header)
        self.assertIn("#define CP_IDLE_RESCAN_MS 60000u", header)
        result = self.spool_wait(ROOT / "apps/printers/spool.c")
        print(result.stdout, result.stderr)
        self.assertEqual(result.returncode, 0)
    def test_spool_and_drivers(self):
        self.run_case(False)
    def test_printers_ui(self):
        self.run_case(True)
if __name__ == "__main__":
    unittest.main()
