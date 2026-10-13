"""F2-50 fixed interaction replay, serialized host builds; never runs a guest.

nice -n 19 python3 tests/host/desktop/test_rendering_architecture.py --benchmark
"""
# SPDX-License-Identifier: MIT
import argparse
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location(
    "rendering_benchmark", ROOT / "tests/host/desktop/test_compositor_performance.py")
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


def run_replay(sanitize=False):
    parent = ROOT / "build/host"
    parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="rendering-replay-", dir=parent) as folder:
        bench.SOURCES = [s.replace("compositor_benchmark.c", "rendering_replay.c") for s in bench.SOURCES]
        binary = bench.build(Path(folder), sanitize=sanitize, profile=True)
        portrait, store = bench.assets()
        for theme in bench.THEMES:
            result = subprocess.run([str(binary), str(portrait), str(store), theme,
                                     str(ROOT / "tests/host/desktop/rendering-interaction.csv")],
                                    capture_output=True, text=True,
                                    env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
            yield theme, result.stdout


class RenderingArchitectureTests(unittest.TestCase):
    def test_recorded_interaction_scanout_and_latency(self):
        for theme, output in run_replay(sanitize=True):
            with self.subTest(theme=theme):
                self.assertIn("PASS rendering replay", output)
                for caps in (0, 3):
                    frames = [line for line in output.splitlines()
                              if line.startswith("FRAME ") and f"accel={caps} " in line]
                    self.assertGreater(len(frames), 10)
                    for action in ("drag", "start_open", "start_close", "launch", "first_frame", "full"):
                        self.assertTrue(any(f"action={action} " in line for line in frames), action)
                    self.assertIn(f"LATENCY theme={theme} accel={caps} ", output)
                for line in output.splitlines():
                    if line.startswith("BREAKDOWN "):
                        self.assertIn("present_front_copy=0", line)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--benchmark", action="store_true")
    args, remaining = parser.parse_known_args()
    if args.benchmark:
        for _, output in run_replay():
            print(output, end="", flush=True)
    else:
        unittest.main(argv=[__file__, *remaining])
