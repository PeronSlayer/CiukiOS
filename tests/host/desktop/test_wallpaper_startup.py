"""f2-44 worker/cache and unchanged wallpaper goldens; no guest."""
# SPDX-License-Identifier: MIT
import hashlib
from datetime import datetime, timezone
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import struct
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
GOLDENS = {
    "center": "cd0588d75b1ba009f3e9613fc1d26fea5f4c07a22c8357a7024d151f892e1fb9",
    "stretch": "992d2355100e6cb8b035d74b232e850645284b20ac908b59315d699417c665a2",
    "tile": "6fa75faa8ec41d90c1feff4016cde395587dd52807a8e334dbd985152d8a9d2c",
    "fit": "ebedb9f69008fdf1c328c4866b1d67a495ec6ea25f0dd39aafc009259a0554af",
    "fill": "f3d1986340a988945887b639f71c9b83b70d84dfbbccad231a32fa4f53db5d09",
}
# f2-90b: the seeded frame is rendered with the default theme's desktop colour (2b4a63), as the desktop requests it.
BUILD_FILL_GOLDEN = "4fda65bf5cd684d549057e9d2b3e4da4cfb7f8297f07d237829b67f97a056b0e"


def image_module():
    spec = importlib.util.spec_from_file_location("startup_image", ROOT / "scripts/build_image.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class WallpaperStartupTests(unittest.TestCase):
    def test_startup_logs_do_not_coalesce(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="wp-stages-", dir=parent) as folder:
            binary = Path(folder) / "test"
            subprocess.run(["clang", "-std=c17", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
                            "-I", str(ROOT / "apps/desktop"), str(ROOT / "tests/host/desktop/settings_io_test.c"),
                            str(ROOT / "apps/desktop/settings_io.c"), "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            lines = [line for line in result.stderr.splitlines() if line.startswith("[desktop] stage=")]
            self.assertEqual(len(lines), 10)
            names = ("entry", "settings_read", "fonts_icons_ready", "first_frame_presented", "wallpaper_file_read",
                     "wallpaper_decoded", "wallpaper_scaled", "wallpaper_applied", "first_input_handled", "wallpaper_cache_read")
            for i, name in enumerate(names):
                suffix = " cache_hit=1 skipped=1" if i in (5, 6) else ""
                self.assertIn(f"[desktop] stage={name} us={100 + i}{suffix}", lines)

    def test_worker_cache_and_goldens(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="wp-", dir=parent) as folder:
            folder = Path(folder)
            source = folder / "Ciuk2.png"
            shutil.copy2(ROOT / "assets/wallpapers/ciuki/Ciuk2.png", source)
            binary = folder / "test"
            env = dict(os.environ, TMPDIR=str(folder), ASAN_OPTIONS="detect_leaks=0")
            subprocess.run(["clang", "-std=c17", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-Wl,--wrap=mkdir,--wrap=read,--wrap=write",
                            "-I", str(ROOT / "apps/desktop"), "-I", str(ROOT / "src/kernel/include"),
                            str(ROOT / "tests/host/desktop/wallpaper_io_test.c"),
                            *(str(ROOT / f"apps/desktop/{n}.c") for n in ("wallpaper_io", "wallpaper", "image", "settings", "theme")),
                            "-o", str(binary)], check=True, env=env)
            result = subprocess.run([str(binary), str(source), str(folder)], capture_output=True, text=True, env=env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            identity = f"wallpaper-v1\n{source}\n1234\n5678\n160x120\n4\n375564\n32,32\n"
            self.assertIn("KEY " + hashlib.sha256(identity.encode()).hexdigest()[:8] + "-160x120-fill.raw", result.stdout)
            for mode, digest in GOLDENS.items():
                self.assertEqual(hashlib.sha256((folder / f"{mode}.raw").read_bytes()).hexdigest(), digest)
            print(result.stdout.strip())

    def test_preload_hit_fallback_and_model(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="wp-pre-", dir=parent) as folder:
            folder = Path(folder)
            source = folder / "Ciuk2.png"
            shutil.copy2(ROOT / "assets/wallpapers/ciuki/Ciuk2.png", source)
            (folder / "cache").mkdir()
            binary = folder / "test"
            subprocess.run(["clang", "-std=c17", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-Wl,--wrap=read",
                            "-I", str(ROOT / "apps/desktop"), "-I", str(ROOT / "src/kernel/include"),
                            str(ROOT / "tests/host/desktop/wallpaper_preload_test.c"),
                            *(str(ROOT / f"apps/desktop/{n}.c") for n in ("wallpaper_io", "wallpaper", "image", "settings", "theme")),
                            "-o", str(binary)], check=True)
            env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
            result = subprocess.run([str(binary), str(source), str(folder / "cache")], capture_output=True, text=True, env=env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS wallpaper preload", result.stdout)
            print(result.stdout.strip())

    def test_seed_build_is_deterministic(self):
        """f2-90: two independent renders into fresh directories give byte-identical native frames."""
        image = image_module()
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="wp-det-", dir=parent) as folder:
            first, one = image.wallpaper_cache_payloads(Path(folder) / "a")
            second, two = image.wallpaper_cache_payloads(Path(folder) / "b")
            self.assertEqual(one["files"], two["files"])
            self.assertEqual(sorted(p.name for p in first.values()), sorted(p.name for p in second.values()))
            for key, path in first.items():
                self.assertEqual(path.read_bytes(), second[key].read_bytes())
                self.assertEqual(len(path.read_bytes()) % 4, 0)

    def test_boot_request_resolves_to_seeded_names(self):
        """f2-90b: the request desktop.c builds at boot hits a seeded file whatever the FAT mtime is."""
        image = image_module()
        _, record = image.wallpaper_cache_payloads()
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="wp-id-", dir=parent) as folder:
            binary = Path(folder) / "identity"
            subprocess.run(["clang", "-std=c17", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
                            "-I", str(ROOT / "apps/desktop"), "-I", str(ROOT / "src/kernel/include"),
                            str(ROOT / "tests/host/desktop/wallpaper_boot_identity.c"),
                            *(str(ROOT / f"apps/desktop/{n}.c") for n in ("wallpaper_io", "wallpaper", "image", "settings", "theme")),
                            "-o", str(binary)], check=True)
            # Checkout time, FAT 2 s truncation, UTC vs local offsets, epoch.
            mtimes = ["1791827142", "1791827143", "1791834342", "1791812742", "946684800", "0"]
            out = subprocess.run([str(binary), str(record["inputs"]["source_size"]), *mtimes], check=True,
                                 capture_output=True, text=True).stdout.split()
            self.assertEqual(len(out), 5 * len(mtimes))
            for name in out:
                self.assertIn(name, record["files"])
            self.assertEqual(set(out), set(record["files"]))

    def test_build_cache_inventory_and_fat(self):
        image = image_module()
        sources, record = image.wallpaper_cache_payloads()
        self.assertEqual(len(sources), 5)
        self.assertEqual({(f["width"], f["height"]) for f in record["files"].values()}, set(image.WALLPAPER_SIZES))
        for name, frame in record["files"].items():
            identity = (f"wallpaper-v1\n/system/wallpapers/Ciuk2.png\n{record['inputs']['source_size']}\n"
                        f"0\n{frame['width']}x{frame['height']}\n4\n2b4a63\n32,32\n")
            self.assertEqual(name, hashlib.sha256(identity.encode()).hexdigest()[:8] +
                             f"-{frame['width']}x{frame['height']}-fill.raw")
            data = sources[f"/System/cache/wallpaper/{name}"].read_bytes()
            self.assertEqual(len(data), frame["width"] * frame["height"] * 4)
            self.assertEqual(hashlib.sha256(data).hexdigest(), frame["sha256"])
            self.assertFalse(any(data[3::4]))
            if frame["width"] == 1024:
                self.assertEqual(frame["sha256"], BUILD_FILL_GOLDEN)
        again, cached = image.wallpaper_cache_payloads()
        self.assertEqual(record, cached)
        self.assertEqual(sources, again)
        if all(shutil.which(t) for t in ("mkfs.fat", "mcopy", "mmd", "mdir")):
            parent = ROOT / "build/host"
            parent.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(prefix="wp-fat-", dir=parent) as folder:
                volume = Path(folder) / "fixture.fat"
                with volume.open("wb") as stream:
                    stream.truncate(32 * 1024 * 1024)
                subprocess.run(["mkfs.fat", str(volume)], check=True, stdout=subprocess.DEVNULL)
                original = "/system/wallpapers/Ciuk2.png"
                sources[original] = ROOT / "assets/wallpapers/ciuki/Ciuk2.png"
                directories = {"/System", "/System/cache", "/System/cache/wallpaper", "/system/wallpapers"}
                image.populate_payloads(volume, sources, directories)
                with patch.object(image, "PART_LBA", 0):
                    image.check_payloads(volume, image.payload_records(sources), directories)
        print("PASS five default frames: production-render goldens, SHA keys, manifest hashes, XRGB rows and FAT readback")
