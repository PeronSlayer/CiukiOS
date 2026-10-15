"""Production text selection, shared clipboard and review frames; host only."""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result

class SelectionTests(unittest.TestCase):
    def test_text_controls_and_clipboard(self):
        helper = module("selection_toolkit", ROOT / "tests/host/desktop/test_ciuknote.py")
        review = module("selection_review", ROOT / "tests/host/desktop/frame_review.py")
        parent = ROOT / "build/host/f2-68"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="selection-", dir=parent) as temp:
            temp = Path(temp)
            binary = temp / "selection"
            sources = [*helper.TOOLKIT, "tests/host/desktop/text_selection_test.c"]
            env = dict(os.environ, TMPDIR=str(temp), ASAN_OPTIONS="detect_leaks=0")
            subprocess.run(["clang", *helper.FLAGS, *helper.INCLUDES, *helper.ICONS.defines(),
                            *(str(ROOT / s) for s in sources), "-o", str(binary)], check=True, env=env)
            result = subprocess.run([str(binary), str(temp)], capture_output=True, text=True, env=env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout.strip())
            actual = {}
            for frame in sorted(temp.glob("*.xrgb")):
                pixels = frame.read_bytes()
                actual[frame.stem] = hashlib.sha256(pixels).hexdigest()
                review.capture(frame.stem, pixels, 400, 300)
            (parent / "selection-goldens.json").write_text(json.dumps(actual, indent=2, sort_keys=True) + "\n")
            pins = ROOT / "tests/host/desktop/text-selection-goldens.json"
            self.assertTrue(pins.exists(), json.dumps(actual, indent=2))
            self.assertEqual(actual, json.loads(pins.read_text()))

if __name__ == "__main__":
    unittest.main()
