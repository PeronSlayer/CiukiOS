"""G30B runtime models; production code under ASan/UBSan, including leak checks."""
from pathlib import Path
import os
import subprocess
import unittest
ROOT = Path(__file__).resolve().parents[3]
class RuntimeTests(unittest.TestCase):
    def test_runtime(self):
        out = ROOT / "build/host/g30b-runtime"
        out.parent.mkdir(parents=True, exist_ok=True)
        sources = ["tests/host/desktop/g30b_runtime_test.c", "apps/systools/devctl.c", "apps/systools/profiles.c",
                   "apps/desktop/protocol.c", "apps/systools/sched.c", "apps/systools/service.c", "apps/systools/monitor.c", "src/kernel/core/system_settings.c"]
        subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-ffunction-sections", "-fdata-sections",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-I", str(ROOT / "src/kernel/include"),
                        *(str(ROOT / s) for s in sources), "-Wl,--gc-sections", "-o", str(out)], check=True)
        result = subprocess.run([out], text=True, capture_output=True, env=os.environ, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        print(result.stdout, end="")
if __name__ == "__main__":
    unittest.main()
