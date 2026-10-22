"""f6-18: the browser test profile's pure helpers (gate, sanitiser, report format)."""
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
PORT = ROOT / "sdk/netsurf/port"


class TestModeTests(unittest.TestCase):
    def test_gate_sanitiser_and_report_format(self):
        out = ROOT / "build/host"
        out.mkdir(parents=True, exist_ok=True)
        binary = out / "netsurf-testmode-test"
        subprocess.run(["nice", "-n", "19", "clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-I", str(ROOT / "build/external/netsurf-target/prefix/include"),
                        "-I", str(PORT), str(ROOT / "tests/host/netsurf/testmode_test.c"), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=30)
        self.assertIn("PASS browser test profile", result.stdout)

    def test_inc_is_included_by_chrome_and_fetcher_uses_the_header(self):
        chrome = (PORT / "chrome.c").read_text()
        self.assertIn('#include "testmode.inc"', chrome)
        self.assertIn('"--test=browser"', chrome)
        self.assertIn("ciuki_test_arm();", chrome)
        fetcher = (PORT / "fetch_ciuki.c").read_text()
        self.assertIn("ciuki_test_anchors(&count)", fetcher)
        self.assertIn("observe(f);", fetcher)
        # The fixture CA reaches the browser only through the generated test anchor header.
        build = (ROOT / "sdk/netsurf/build.py").read_text()
        self.assertIn('tests/suites/fixtures/f6-browser/ca.pem', build)
        self.assertIn('"ciuki_test"', build)


if __name__ == "__main__":
    unittest.main()
