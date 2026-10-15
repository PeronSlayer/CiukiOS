"""Window interaction replay, raster goldens and optional paired review frames."""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tarfile
import io
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
GOLDENS = ROOT / "tests/host/desktop/interaction-goldens.json"


def replay(reference=False):
    parent = ROOT / "build/host/f2-67"
    parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=parent) as temp:
        folder = Path(temp)
        source = ROOT
        if reference:
            source = folder / "reference"
            source.mkdir()
            archive = subprocess.run(["git", "archive", "HEAD", "apps/desktop", "apps/libdeskui", "apps/files", "apps/dosbox/autorun.h",
                                      "tests/host/desktop/fake.c", "tests/host/desktop/fake.h"],
                                     cwd=ROOT, check=True, capture_output=True).stdout
            with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
                tar.extractall(source, filter="data")
        sources = [source / f"apps/desktop/{n}.c" for n in
                   ("client", "input", "protocol", "compositor", "theme", "shell", "autoplay", "autoplay_content",
                    "settings", "wallpaper", "keymap", "folder")]
        sources += [source / f"apps/libdeskui/{n}.c" for n in
                    ("draw", "widgets", "list", "menu", "dialog", "textarea", "icons", "icon_map")]
        sources += [source / "apps/files/model.c", source / "apps/files/transfer.c", source / "tests/host/desktop/fake.c",
                    ROOT / "tests/host/desktop/interaction_test.c"]
        binary = folder / "interaction"
        env = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0"}
        objects = []
        for index, path in enumerate(sources):
            obj = folder / f"{index}.o"
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            *(["-D_POSIX_C_SOURCE=200809L"] if "libdeskui" in str(path) else []),
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                            "-I", str(source / "apps/desktop"), "-I", str(source / "apps/libdeskui"),
                            "-I", str(ROOT / "src/kernel/include"), "-c", str(path), "-o", str(obj)],
                           check=True, env=env)
            objects.append(str(obj))
        subprocess.run(["clang", "-fsanitize=address,undefined", *objects, "-o", str(binary)], check=True, env=env)
        frames = folder / "frames"
        frames.mkdir()
        result = subprocess.run([str(binary), str(frames)], env=env, text=True, capture_output=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        digests = {}
        for frame in sorted(frames.glob("*.ppm")):
            raw = frame.read_bytes()
            digests[frame.stem] = hashlib.sha256(raw).hexdigest()
            if os.environ.get("CIUKI_INTERACTION_FRAMES"):
                from PIL import Image
                dest = Path(os.environ["CIUKI_INTERACTION_FRAMES"])
                dest.mkdir(parents=True, exist_ok=True)
                Image.open(io.BytesIO(raw)).save(dest / (frame.stem + ".png"))
        return digests, result.stdout


class InteractionTests(unittest.TestCase):
    def test_interactions_and_frames(self):
        actual, output = replay()
        print(output, end="")
        if os.environ.get("CIUKI_RECORD_INTERACTION"):
            GOLDENS.write_text(json.dumps(actual, indent=2, sort_keys=True) + "\n")
        self.assertEqual(actual, json.loads(GOLDENS.read_text()))


if __name__ == "__main__":
    if "--reference" in __import__("sys").argv:
        _, output = replay(reference=True)
        print(output, end="")
    else:
        unittest.main()
