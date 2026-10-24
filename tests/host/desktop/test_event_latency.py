"""fx-26: event -> frame latency of the volume popup and the notification.

The production desktop.c service-loop functions in virtual time; see
tests/host/desktop/event_latency_test.h. Run with nice -n 19.
"""
# SPDX-License-Identifier: MIT
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_desktop import ROOT
from .test_memory_footprint import build_footprint


class EventLatencyTests(unittest.TestCase):
    def test_volume_popup_and_notification_next_frame(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="event-latency-", dir=parent) as temp:
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS="detect_leaks=0")
            binary = build_footprint(Path(temp), env)
            result = subprocess.run([str(binary), "--event-latency"], capture_output=True, text=True, env=env, timeout=300)
            print(result.stdout, end="")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr[-4000:])
            self.assertIn("fx-26 volume popup and notification", result.stdout)
            self.assertIn("PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
