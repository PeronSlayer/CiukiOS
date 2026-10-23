"""F2-98c production loop, model idle time and real completion transport."""
# SPDX-License-Identifier: MIT
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class NativeEventWaitTests(unittest.TestCase):
    def test_dos_error_windows(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="dos-error-wait-", dir=parent) as tmp:
            work = Path(tmp)
            include = work / "include/ciuki"
            include.mkdir(parents=True)
            for name in ("dosvm.h", "channel.h", "surface.h"):
                (include / name).write_bytes((ROOT / "sdk/sysroot-overlay/include/ciuki" / name).read_bytes())
            (include / "desktop.h").write_bytes((ROOT / "apps/desktop/protocol.h").read_bytes())
            binary = work / "dos-error"
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                            '-DDOSBOX_SETTINGS_DIR="' + str(work / "prefs") + '"', "-I", str(work / "include"),
                            "-I", str(ROOT / "src/kernel/include"), str(ROOT / "tests/host/desktop/dosbox_loading_test.c"),
                            str(ROOT / "apps/desktop/protocol.c"), "-o", str(binary)], check=True)
            for scenario in (1, 2, 3, 5, 9, 21):
                with self.subTest(scenario=scenario):
                    result = subprocess.run([binary, str(scenario)], capture_output=True, text=True, timeout=10,
                                            env={**os.environ, "TMPDIR":tmp})
                    print(result.stdout, end="", flush=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn("one infinite wait for 10 s modeled idle", result.stdout)

    def test_run_loop_and_job_publication(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="native-wait-", dir=parent) as tmp:
            work = Path(tmp)
            (work / "include").mkdir()
            (work / "include/ciuki").symlink_to(ROOT / "sdk/sysroot-overlay/include/ciuki", target_is_directory=True)
            source = (ROOT / "apps/libdeskui/client.c").read_text()
            functions = []
            for name in ("du_window_wait", "du_run"):
                match = re.search(r"^int " + name + r"\([^\n]+\)\n\{", source, re.M)
                self.assertIsNotNone(match)
                end, depth = match.end(), 1
                while depth:
                    depth += (source[end] == "{") - (source[end] == "}")
                    end += 1
                functions.append(source[match.start():end])
            (work / "native_run.inc").write_text("\n".join(functions))
            binary = work / "wait"
            subprocess.run(["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-pthread",
                            "-I", str(work), "-I", str(work / "include"), "-I", str(ROOT / "src/kernel/include"),
                            str(ROOT / "tests/host/desktop/native_event_wait_test.c"),
                            str(ROOT / "tests/host/desktop/native_event_channel_host.c"), "-o", str(binary)], check=True)
            result = subprocess.run([binary], capture_output=True, text=True, timeout=20, env={**os.environ, "TMPDIR":tmp})
            print(result.stdout, end="", flush=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
