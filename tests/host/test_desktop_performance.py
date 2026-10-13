"""F1-25 production compositor and asynchronous settings tests; sequential, no guest."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class DesktopPerformanceTests(unittest.TestCase):
    def run_model(self, name, sources, includes, flags=()):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="f1-25-", dir=parent) as folder:
            binary = Path(folder) / name
            env = dict(os.environ, TMPDIR=folder, ASAN_OPTIONS="detect_leaks=0")
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-pthread",
                            *flags,
                            *sum((["-I", str(ROOT / p)] for p in includes), []),
                            *(str(ROOT / p) for p in sources), "-o", str(binary)], check=True, env=env)
            result = subprocess.run([str(binary)], capture_output=True, text=True, env=env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout.strip())

    def test_compositor_counters_and_sequence(self):
        self.run_model("performance", ["tests/host/desktop/performance_test.c", "tests/host/desktop/fake.c",
                       *(f"apps/desktop/{p}.c" for p in ("client", "protocol", "input", "compositor", "theme", "shell", "autoplay", "autoplay_content", "settings", "wallpaper", "keymap", "folder")),
                       "apps/files/model.c", "apps/files/transfer.c",
                       "apps/libdeskui/draw.c"], ["apps/desktop", "tests/host/desktop", "src/kernel/include"],
                       ["-DF125_WRAP_ALLOC", "-Wl,--wrap=malloc"])

    def test_settings_worker(self):
        self.run_model("settings", ["tests/host/desktop/settings_io_test.c", "apps/desktop/settings_io.c"], ["apps/desktop"])
