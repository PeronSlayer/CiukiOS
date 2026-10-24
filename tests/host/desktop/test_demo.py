"""Production Demo rendering, including its ordinary text path."""
# SPDX-License-Identifier: MIT
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import importlib.util as _review_util  # optional PNG review copies (frame_review.py, any import mode)
_review_spec = _review_util.spec_from_file_location("frame_review", __file__.rsplit("/", 1)[0] + "/frame_review.py")
_review = _review_util.module_from_spec(_review_spec); _review_spec.loader.exec_module(_review)
capture = _review.capture

ROOT = Path(__file__).resolve().parents[3]
GOLDEN = "a230f4330c4721034c5ce21e2393bc4499caf8cb93ccb7a2bd174c1fbb0918e4"  # qa4: 360x240 panel layout


class DemoTests(unittest.TestCase):
    def test_production_demo_text(self):
        with tempfile.TemporaryDirectory(prefix="demo-", dir=ROOT / "build/host") as temp:
            folder = Path(temp); binary = folder / "demo"
            inc = folder / "include/ciuki"; inc.mkdir(parents=True)
            (inc / "raw.h").write_text("#include <stdint.h>\nuint32_t ciuki_raw_probe_report(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);\n")
            (inc / "runtime.h").write_text("#include <stdint.h>\n#define CU_PTR(p) ((uint32_t)(uintptr_t)(p))\nstatic inline int ciuki_error(uint32_t r) { return r >= (uint32_t)-4095 ? -(int32_t)r : 0; }\n")
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-macro-redefined", "-D_POSIX_C_SOURCE=200809L",
                            "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                            "-idirafter", str(ROOT / "sdk/sysroot-overlay/include"), "-I", str(folder / "include"), "-I", str(ROOT / "src/kernel/include"),
                            "-I", str(ROOT / "apps/desktop"), str(ROOT / "tests/host/desktop/demo_render_test.c"),
                            str(ROOT / "apps/desktop/theme.c"), str(ROOT / "apps/desktop/compositor.c"), "-o", str(binary)], check=True)
            frame = folder / "demo.xrgb"
            subprocess.run([binary, frame], check=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
            capture("demo", frame.read_bytes(), 360, 240)
            digest = hashlib.sha256(frame.read_bytes()).hexdigest()
            if GOLDEN == "PIN-FROM-FIRST-RUN": print("DEMO_GOLDEN", digest)
            else: self.assertEqual(digest, GOLDEN)


if __name__ == "__main__":
    unittest.main()
