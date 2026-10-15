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
    'benchmark': 'e75a6247ee04331eca5f7a4493484b3257f86033192852af226a4847286a51cd',
    'cube-flat': '7437fff6f62953feccc31270f932f28e95200308c002ebe964cb942d0cf0adf8',
    'cube-gouraud': '89f284494dd8f8325ea77f210e40a18ba4cc283209d86e5192b0422a3a91c3ed',
    'sprites': '4d6413c6c2b6d4411dbc7aa30b6decc0633cd04967ff27edee466ae851f52944',
    'software-only': '9ffdd290e6d29e1f3b7fbc610409f55e84d2db88f7c190e1687a7345cc9e593b',
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
