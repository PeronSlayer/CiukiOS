"""Resize layout and compositor captures; no guest qualification."""
# SPDX-License-Identifier: MIT
import importlib.util
import os
from pathlib import Path
import struct
import shutil
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[3]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def png(raw, width, height):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        row = raw[y * width * 4:(y + 1) * width * 4]
        for x in range(0, len(row), 4):
            rows.extend((row[x + 2], row[x + 1], row[x]))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


class WindowResizeTests(unittest.TestCase):
    def test_files_layout_and_compositor_captures(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="window-resize-", dir=parent) as temp:
            folder = Path(temp)
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS="detect_leaks=0")
            include = folder / "include/ciuki"
            include.mkdir(parents=True)
            for header in ("channel.h", "surface.h"):
                shutil.copy2(ROOT / "sdk/sysroot-overlay/include/ciuki" / header, include / header)
            icons = load("resize_icon_assets", ROOT / "tests/host/desktop/icon_assets.py")
            toolkit = load("resize_toolkit", ROOT / "tests/host/desktop/test_deskui.py").TOOLKIT
            sources = [*toolkit, "apps/desktop/client.c", "apps/desktop/protocol.c", "apps/desktop/input.c",
                       "apps/desktop/shell.c", "apps/desktop/folder.c", "apps/files/transfer.c", "apps/desktop/settings.c", "apps/desktop/wallpaper.c", "apps/desktop/keymap.c",
                       "apps/files/app.c", "apps/files/model.c", "tests/host/desktop/fake_vfs.c",
                       "tests/host/desktop/files_resize_capture.c", "tests/host/desktop/resize_transport.c",
                       "tests/host/desktop/resize_client.c"]
            binary = folder / "capture"
            objects = []
            for i, s in enumerate(sources):
                obj = folder / f"{i}.o"
                subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                                *([] if s == "apps/desktop/client.c" else ["-D_POSIX_C_SOURCE=200809L"]),
                                "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                                "-I", str(ROOT / "src/kernel/include"), "-I", str(folder / "include"), *icons.defines(),
                                "-c", str(ROOT / s), "-o", str(obj)], check=True, env=env)
                objects.append(str(obj))
            subprocess.run(["clang", "-fsanitize=address,undefined", *objects, "-o", str(binary)], check=True, env=env)
            portrait = load("resize_portrait", ROOT / "apps/desktop/convert_portrait.py")
            source = folder / "portrait.xrgb"
            source.write_bytes(portrait.convert(ROOT / "assets/brand/ciuki-logo.png"))
            result = subprocess.run([str(binary), str(source), str(folder)], env=env,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout.strip())
            frames = sorted(folder.glob("files-*.xrgb"))
            self.assertEqual(len(frames), 6)
            for frame in frames:
                self.assertEqual(frame.stat().st_size, 1024 * 768 * 4)
                target = os.environ.get("CIUKI_RESIZE_FRAMES")
                if target:
                    dest = Path(target); dest.mkdir(parents=True, exist_ok=True)
                    (dest / (frame.stem + ".png")).write_bytes(png(frame.read_bytes(), 1024, 768))


if __name__ == "__main__":
    unittest.main()
