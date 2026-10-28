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
# F7-14: shared GL engine scenes; backend/native/full-screen controls, GL status line, narrow two-row layout.
GOLDENS = {
    'benchmark': '97cd36a37115739655975fe635a71fcdc283b1448b688e93b975a4d2a1477071',
    'cube-flat': '098ff069e91f0514f66bda39e3d911c14926d8350309507d45ddb3b4c201c30f',
    'cube-gouraud': '1afd6ac15f8fc678377d8a2fd659c07b1da51f976b9a4ed64aaa7e31c931f453',
    'sprites': '1d75e9d95fc49247ef266c608e675c847f1189ed9f4f2b85110bbb66ed288645',
    'software-only': '98606386748c1b2242b2648e7c02c45b1c3889861637f4d3d2aeeb766dba66f6',
    'cube-hardware-unavailable': '5d8893f68de227acffb0b312231a9b09c4bac9fa12a2e7e3622daca7e722a3be',
    'narrow-controls': '80efcfd42c78c6545d6688e441b7ead544e65c777cda01e7d56c012ca8fc4774',
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
            self.assertEqual(set(digests), set(GOLDENS))
            self.assertEqual(len(set(digests.values())), len(GOLDENS))
            print("GFX_GOLDENS", digests)
            for p in folder.glob("*.xrgb"):
                if p.stem != "portrait":
                    width, height = map(int, p.with_suffix(".size").read_text().split())
                    capture("gfxdemo-" + p.stem, p.read_bytes(), width, height)
            target = os.environ.get("CIUKI_GFX_FRAMES")
            if target:
                Path(target).mkdir(parents=True, exist_ok=True)
                for p in folder.glob("*.xrgb"):
                    if p.stem != "portrait":
                        (Path(target) / p.name).write_bytes(p.read_bytes())
            self.assertEqual(digests, GOLDENS)

if __name__ == "__main__":
    unittest.main()
