"""Sequential ASan/UBSan tests with pinned deterministic production frames."""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
# f2-59: integer viewport, smooth poses and completion-based FPS/status copy.
GOLDENS = {
    'benchmark': 'ef4e7049cf2acf4ef50e6c6d8f4e9a3c101ff0668fa7083fcb0eb66e66396dd6',
    'cube-flat': 'f3b375ab64adfe85b8ed3f321fd97e3769a62b8b796538ef0b1193703edb8780',
    'cube-gouraud': 'dbe0138fa6197fab752286cac49abcfb97d1c2dd0ecbeffff43d50fca2f06212',
    'software-only': '5769307868c58d539e93066b6e5e8ed8997f5b1a4082b2c98edf35b1e12bff5c',
    'sprites': '4bf980005cb1921658bf32eecdec42e13a26bf6d4a5dfdaf549b2b0b3e21ecf6',
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
            binary = folder / "gfxdemo"
            env = {**os.environ, "TMPDIR": temp, "ASAN_OPTIONS": "detect_leaks=0"}
            subprocess.run([*acc.FLAGS, *assets.defines(), *acc.INCLUDES, *(str(ROOT / p) for p in sources),
                            "-o", str(binary)], check=True, env=env)
            result = subprocess.run([str(binary), str(folder / "portrait.xrgb"), temp],
                                    capture_output=True, text=True, env=env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout)
            digests = {p.stem: hashlib.sha256(p.read_bytes()).hexdigest() for p in folder.glob("*.xrgb") if p.stem != "portrait"}
            self.assertEqual(set(digests), {"benchmark", "cube-flat", "cube-gouraud", "sprites", "software-only"})
            self.assertEqual(len(set(digests.values())), 5)
            print("GFX_GOLDENS", digests)
            target = os.environ.get("CIUKI_GFX_FRAMES")
            if target:
                Path(target).mkdir(parents=True, exist_ok=True)
                for p in folder.glob("*.xrgb"):
                    if p.stem != "portrait":
                        (Path(target) / p.name).write_bytes(p.read_bytes())
            self.assertEqual(digests, GOLDENS)

if __name__ == "__main__":
    unittest.main()
