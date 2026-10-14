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
# F2-62: same scenes and viewport, rendered with the new Blue default.
GOLDENS = {
    'benchmark': 'aef4229de656100fcb376e001a95896312643842d67ee709b843c634b3af75c0',
    'cube-flat': '800428880bcd2c6c13981a7cc6004a38b94588f72f58dc716d214e49e90e1ba9',
    'cube-gouraud': '048dce531e7d19924a5e529017c855189957dd528058adf5ceb35973c27b2664',
    'software-only': '07b0fb892558270bed03e65862f9e7523ac67036a9a506952cd6551db468f0d5',
    'sprites': 'c821fa7e3a973a04cc62ef4920f1789a1396f5a14e0b013ffac2ae70fd474c90',
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
