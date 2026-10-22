"""f2-88c system wallpaper thumbnails: staging only, never an OS image build."""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
GOLDENS = {
    "Ciuk1-192x144.raw": "14475b4bafdf3c61d4f4757cb5326bc158e147f51c2faf5c668bbf0ff0f1118b",
    "Ciuk2-192x144.raw": "248987a2bc668112bfe4cce0034786fd986112c3c38af5286ddf42fe5f1ddcfe",
    "Ciuk3-192x144.raw": "35d0b8a7f6b0fbe8ac29bbee55afaf655e445fc5d1cd9f2863147d90eb1ec77a",
}


def image_module():
    spec = importlib.util.spec_from_file_location("control_thumbnails_image", ROOT / "scripts/build_image.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ControlThumbnailTests(unittest.TestCase):
    def test_deterministic_previews_inventory_and_invalidation(self):
        image = image_module()
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="control-thumbs-", dir=parent) as temporary:
            out = Path(temporary)
            first, record = image.wallpaper_thumbnail_payloads(out / "first")
            second, repeated = image.wallpaper_thumbnail_payloads(out / "second")
            self.assertEqual(record, repeated)
            self.assertEqual(len(first), len(image.WALLPAPERS))
            for path, source in first.items():
                self.assertEqual(source.read_bytes(), second[path].read_bytes())
                data = source.read_bytes()
                self.assertEqual(len(data), 192 * 144 * 4)
                self.assertFalse(any(data[3::4]))
                frame = record["files"][source.name]
                self.assertEqual(hashlib.sha256(data).hexdigest(), frame["sha256"])
                self.assertEqual((frame["width"], frame["height"]), (192, 144))
                self.assertIn(Path(frame["source_path"]).name, image.WALLPAPERS)
            self.assertEqual({name: f["sha256"] for name, f in record["files"].items()}, GOLDENS)
            again, cached = image.wallpaper_thumbnail_payloads(out / "first")
            self.assertEqual((first, record), (again, cached))
            next(iter(first.values())).write_bytes(b"corrupt")
            restored, repaired = image.wallpaper_thumbnail_payloads(out / "first")
            self.assertEqual(record, repaired)
            self.assertEqual(image.payload_records(restored), image.payload_records(second))
            # Exercise the actual desktop payload assembler with unrelated
            # ELF/license/font/sound validation replaced by bounded fixtures.
            class DesktopBuild:
                ICON_FILES = ()
                @staticmethod
                def validate_payloads():
                    return dict(protocol_version=2, elf={}, portrait={}, icons={})
            original_loader = image.importlib.util.module_from_spec
            def module_from_spec(spec):
                return DesktopBuild if spec.name == "build_desktop" else original_loader(spec)
            with patch.object(image.importlib.util, "module_from_spec", side_effect=module_from_spec), \
                 patch.object(image.importlib.machinery.SourceFileLoader, "exec_module"), \
                 patch.object(image, "desktop_license_payloads", return_value={}), \
                 patch.object(image, "font_payloads", return_value=({}, {})), \
                 patch.object(image, "sound_payloads", return_value=({}, {})), \
                 patch.object(image, "wallpaper_cache_payloads", return_value=({}, {})), \
                 patch.object(image, "wallpaper_thumbnail_payloads", return_value=(first, record)), \
                 patch.object(image, "sha256", return_value="fixture"):
                payloads, directories, metadata = image.desktop_payloads()
            self.assertTrue(set(first) <= set(payloads))
            self.assertIn("/System/cache/wallpaper/thumbnails", directories)
            self.assertEqual(metadata["wallpaper_thumbnails"], record)
            self.assertEqual({r["path"] for r in image.payload_records(first)}, set(first))
        print("PASS thumbnail inventory: deterministic production scaler, three previews, hashes, corruption invalidation, canonical payload integration")
