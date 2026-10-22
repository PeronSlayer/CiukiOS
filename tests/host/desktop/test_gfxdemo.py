"""Sequential ASan/UBSan tests with pinned deterministic production frames."""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
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
# F2-62: same scenes and viewport, rendered with the new Blue default.
# F2-63: re-recorded for the pinned FreeType UI atlases.
# F7-01: TinyGL scenes at native viewport size; GL tabs and product acknowledgment.
GOLDENS = {
    'benchmark': '17b617f0e3b209f9ee7d0168a38c346e83a131bf7f0114ad26db42b380211e6f',
    'cube-flat': '39b432352407495b416d45b9923a1523b49c144504d3bef27121e64ffe63f8e5',
    'cube-gouraud': 'bf2f59d9d9bc0550221d64fdc60b87f5f82b145a2f4068e7ec4115a172d43de9',
    'sprites': '634c85fca6ea267679bf34fb74e80bcfe4b316dd7661bd3d101d0eb16dc6ea4e',
    'software-only': '34337f9c73e297f3fa29e0ee1f6412c72998800a7236e329b2a01a13a37a95d2',
}

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m

class GraphicsDemoTests(unittest.TestCase):
    def test_frames_and_benchmark(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="gfxdemo-", dir=parent) as temp:
            folder = Path(temp)
            assets = load("gfx_assets", ROOT / "tests/host/desktop/icon_assets.py")
            acc = load("gfx_toolkit", ROOT / "tests/host/desktop/test_accessories.py")
            portrait = load("gfx_portrait", ROOT / "apps/desktop/convert_portrait.py")
            (folder / "portrait.xrgb").write_bytes(portrait.convert(ROOT / "assets/brand/ciuki-logo.png"))
            sources = ["apps/gfxdemo/app.c", "apps/gfxdemo/scene.c", "apps/accessories/acc.c",
                       "apps/desktop/protocol.c", *acc.TOOLKIT, "tests/host/desktop/gfxdemo_test.c"]
            gl = load("gfx_libgl", ROOT / "sdk/libgl/build.py")
            library = gl.host(sanitize=True)
            binary = folder / "gfxdemo"
            env = {**os.environ, "TMPDIR": temp, "ASAN_OPTIONS": "detect_leaks=0"}
            subprocess.run([*acc.FLAGS, *assets.defines(), *acc.INCLUDES, *gl.includes(), *(str(ROOT / p) for p in sources),
                            str(library), "-lm", "-o", str(binary)], check=True, env=env)
            result = subprocess.run([str(binary), str(folder / "portrait.xrgb"), temp],
                                    capture_output=True, text=True, env=env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout)
            digests = {p.stem: hashlib.sha256(p.read_bytes()).hexdigest() for p in folder.glob("*.xrgb") if p.stem != "portrait"}
            self.assertEqual(set(digests), {"benchmark", "cube-flat", "cube-gouraud", "sprites", "software-only"})
            self.assertEqual(len(set(digests.values())), 5)
            print("GFX_GOLDENS", digests)
            for p in folder.glob("*.xrgb"):
                if p.stem != "portrait":
                    capture("gfxdemo-" + p.stem, p.read_bytes(), 760, 640)
            target = os.environ.get("CIUKI_GFX_FRAMES")
            if target:
                Path(target).mkdir(parents=True, exist_ok=True)
                for p in folder.glob("*.xrgb"):
                    if p.stem != "portrait":
                        (Path(target) / p.name).write_bytes(p.read_bytes())
            self.assertEqual(digests, GOLDENS)

if __name__ == "__main__":
    unittest.main()
