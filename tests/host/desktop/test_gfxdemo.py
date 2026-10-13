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
GOLDENS = {
    "benchmark": "72c7ebc66384de94621c6ea64e1c0cae476c532e8e3a872265b56b10f19f304c",
    "cube-flat": "cc8274ee2da43bcefb2836cf158a91428de81bef27db7650ca58156363896fd3",
    "cube-gouraud": "a5d44f172923fefd86294a76bdac7e6c63580f52d4fb6b0db1e5681cbe922123",
    "sprites": "d7448591b33975d85d86bbd26389abfc1ab44194355ae9d44bc406216cb580ff",
    "software-only": "647bcc1fbb81cfea8768990ea1e6e66922b5a3e5b68863e3e876e10ca211eae4",
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
