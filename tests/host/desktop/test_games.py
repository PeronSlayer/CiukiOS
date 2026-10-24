"""Games accessories (G27): Minesweeper, Solitaire, FreeCell, Hearts engines and windows."""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
import importlib.util as _review_util  # optional PNG review copies (frame_review.py, any import mode)
_review_spec = _review_util.spec_from_file_location("frame_review", __file__.rsplit("/", 1)[0] + "/frame_review.py")
_review = _review_util.module_from_spec(_review_spec); _review_spec.loader.exec_module(_review)
capture = _review.capture

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
FLAGS = ["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-D_POSIX_C_SOURCE=200809L",
         "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
         "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections"]
TOOLKIT = ["apps/libdeskui/draw.c", "apps/libdeskui/widgets.c", "apps/libdeskui/list.c", "apps/libdeskui/menu.c",
           "apps/libdeskui/dialog.c", "apps/libdeskui/controls.c", "apps/libdeskui/commdlg.c", "apps/libdeskui/textarea.c", "apps/libdeskui/recent.c", "apps/libdeskui/keymap.c",
           "apps/desktop/compositor.c", "apps/desktop/theme.c", "apps/libdeskui/icons.c", "apps/libdeskui/icon_map.c"]
INCLUDES = ["-I", str(ROOT / "apps/accessories"), "-I", str(ROOT / "apps/libdeskui"), "-I", str(ROOT / "apps/desktop"),
            "-I", str(ROOT / "src/kernel/include")]
ACC = "apps/accessories/"
GAMES = "apps/games/"
TESTS = {
    "mines": [GAMES + "mines_model.c", GAMES + "mines_view.c", GAMES + "cards.c"],
    "freecell": [GAMES + "freecell_model.c", GAMES + "freecell_view.c", GAMES + "cards.c"],
    "serial": ["apps/serial/screen.c", "apps/serial/xmodem.c", "apps/serial/serial_model.c", "apps/serial/serial_view.c"],
    "hearts": [GAMES + "hearts_model.c", GAMES + "hearts_view.c", GAMES + "cards.c"],
    "solitaire": [GAMES + "solitaire_model.c", GAMES + "solitaire_view.c", GAMES + "cards.c"],
}
INCLUDES += ["-I", str(ROOT / "apps/games"), "-I", str(ROOT / "apps/serial")]
RENDERS = {
    "mines-initial.xrgb": "PIN-FROM-FIRST-RUN",
    "mines-playing.xrgb": "PIN-FROM-FIRST-RUN",
    "mines-expert.xrgb": "PIN-FROM-FIRST-RUN",
    "mines-lost.xrgb": "PIN-FROM-FIRST-RUN",
    "freecell-game1.xrgb": "PIN-FROM-FIRST-RUN",
    "freecell-selected.xrgb": "PIN-FROM-FIRST-RUN",
    "freecell-drag.xrgb": "PIN-FROM-FIRST-RUN",
    "freecell-long.xrgb": "PIN-FROM-FIRST-RUN",
    "serial-disconnected.xrgb": "PIN-FROM-FIRST-RUN",
    "serial-connected.xrgb": "PIN-FROM-FIRST-RUN",
    "serial-scrollback.xrgb": "PIN-FROM-FIRST-RUN",
    "serial-properties.xrgb": "PIN-FROM-FIRST-RUN",
    "serial-transfer.xrgb": "PIN-FROM-FIRST-RUN",
    "hearts-pass.xrgb": "PIN-FROM-FIRST-RUN",
    "hearts-marked.xrgb": "PIN-FROM-FIRST-RUN",
    "hearts-playing.xrgb": "PIN-FROM-FIRST-RUN",
    "hearts-round-end.xrgb": "PIN-FROM-FIRST-RUN",
    "solitaire-deal.xrgb": "PIN-FROM-FIRST-RUN",
    "solitaire-draw3.xrgb": "PIN-FROM-FIRST-RUN",
    "solitaire-long.xrgb": "PIN-FROM-FIRST-RUN",
}


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class GamesTests(unittest.TestCase):
    def run_native(self, name):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=f"games-{name}-", dir=parent) as temp:
            folder = Path(temp)
            renders = folder / "renders"; renders.mkdir()
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS="detect_leaks=0", HOME=str(folder))
            binary = folder / name
            sources = [*TESTS[name], *TOOLKIT, f"tests/host/desktop/{name}_test.c", "apps/accessories/acc.c"]
            icons = load("games_icon_assets", ROOT / "tests/host/desktop/icon_assets.py")
            build = subprocess.run([*FLAGS, *icons.defines(), *INCLUDES, *(str(ROOT / s) for s in sources), "-lm", "-o", str(binary)],
                                   env=env, capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            scratch = folder / "scratch"; scratch.mkdir()
            args = [str(scratch), str(renders)] if name == "serial" else [str(renders)]   # serial writes capture and transfer files too
            result = subprocess.run([str(binary), *args], env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS", result.stdout)
            for p in renders.glob("*.xrgb"):
                width, height = map(int, p.with_suffix(".size").read_text().split())
                capture(p.stem, p.read_bytes(), width, height)
            return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(renders.glob("*.xrgb"))}

    def check_renders(self, actual):
        for name, digest in actual.items():
            self.assertIn(name, RENDERS)
            if RENDERS[name] != "PIN-FROM-FIRST-RUN":
                self.assertEqual(digest, RENDERS[name], name)

    def test_mines(self): self.check_renders(self.run_native("mines"))
    def test_freecell(self): self.check_renders(self.run_native("freecell"))
    def test_serial(self): self.check_renders(self.run_native("serial"))
    def test_hearts(self): self.check_renders(self.run_native("hearts"))
    def test_solitaire(self): self.check_renders(self.run_native("solitaire"))


if __name__ == "__main__":
    unittest.main()
