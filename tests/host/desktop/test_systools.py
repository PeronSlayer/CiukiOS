"""G30 system tools: System Information, System Monitor, Resource Meter, Device Manager actions,
Add New Hardware, Task Scheduler and Maintenance Wizard models. Run with nice -n 19."""
# SPDX-License-Identifier: MIT
import os
from pathlib import Path
import subprocess
import sys
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
FLAGS = ["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-D_POSIX_C_SOURCE=200809L",
         "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]


class SystoolsTests(unittest.TestCase):
    def test_models(self):
        out = ROOT / "build/host"
        out.mkdir(parents=True, exist_ok=True)
        binary = out / "systools_test"
        sources = [ROOT / "apps/systools" / n for n in ("sysinfo.c", "resmeter.c", "monitor.c", "devctl.c", "sched.c", "profiles.c", "service.c")]
        subprocess.run([*FLAGS, "-I", str(ROOT / "apps/systools"), "-I", str(ROOT / "src/kernel/include"),
                        str(ROOT / "tests/host/desktop/systools_test.c"), *map(str, sources), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True,
                                env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"}, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for name in ("System Information", "Resource Meter", "System Monitor", "Device Manager actions",
                     "Add New Hardware", "Task Scheduler", "Maintenance Wizard", "sources", "Hardware Profiles", "Performance",
                     "scheduler service", "system tools"):
            self.assertIn("PASS G30 " + name, result.stdout)
        print(result.stdout, end="")


if __name__ == "__main__":
    unittest.main()
