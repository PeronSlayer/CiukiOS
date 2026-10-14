"""F4-06/F4-12 host-only tests. Each harness runs sequentially, including conversion."""
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    return m

class SystemSoundsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        module("sound_build_initial", ROOT / "scripts/build_sounds.py").build()

    def harness(self, name, sources, *args):
        scratch = ROOT / "build/host"; scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=name, dir=scratch) as temp:
            binary = Path(temp) / name
            inc = Path(temp) / "include/ciuki"; inc.mkdir(parents=True)
            for header in ("audio.h", "display.h", "font.h", "registry.h", "devices.h"):
                (inc / header).write_bytes((ROOT / "sdk/sysroot-overlay/include/ciuki" / header).read_bytes())
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-D_POSIX_C_SOURCE=200809L", "-Wno-macro-redefined", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                            "-I", str(ROOT / "apps/desktop"), "-I", str(ROOT / "apps/control"),
                            "-I", str(Path(temp) / "include"), "-I", str(ROOT / "src/kernel/include"), "-pthread",
                            *(str(ROOT / s) for s in sources), "-lm", "-o", str(binary)], check=True)
            subprocess.run([str(binary), *map(str, args)], check=True, env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))

    def test_image_sound_mapping(self):
        image = module("sound_image_mapping",ROOT / "scripts/build_image.py")
        sources, manifest = image.sound_payloads()
        self.assertEqual(len(sources),21)
        self.assertIn("/system/sounds/scheme.cfg",sources)
        self.assertIn("/system/sounds/CREDITS.TXT",sources)
        self.assertEqual(sum(p.stat().st_size for p in sources.values()),manifest["bytes"])

    def test_worker(self):
        self.harness("sound-worker", ["tests/host/desktop/sound_runtime_test.c", "apps/desktop/sounds.c", "apps/desktop/protocol.c", "apps/desktop/sound_storage.c", "apps/desktop/master_storage.c",
                   "sdk/libreg/registry.c", "sdk/libreg/storage.c", "sdk/libreg/hardware.c"])

    def test_model_and_protocol(self):
        build = module("sound_build_model", ROOT / "scripts/build_sounds.py")
        self.harness("sounds", ["tests/host/desktop/sounds_test.c", "apps/desktop/sounds.c", "apps/desktop/protocol.c"], build.OUTPUT)

    def test_registry_preferences(self):
        scratch = ROOT / "build/host"; scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="sound-registry", dir=scratch) as temp:
            self.harness("sound-storage", ["tests/host/desktop/sound_storage_test.c", "apps/desktop/sounds.c",
                         "apps/desktop/sound_storage.c", "apps/desktop/master_storage.c", "sdk/libreg/registry.c", "sdk/libreg/storage.c", "sdk/libreg/hardware.c"],
                         Path(temp).resolve())

    def test_pack_reproducibility_and_pins(self):
        build = module("sound_build_pins", ROOT / "scripts/build_sounds.py")
        scratch = ROOT / "build/host"; scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="sounds-pack", dir=scratch) as temp:
            out = Path(temp); first = build.build(out); second = build.build(out)
            self.assertEqual(first, second); self.assertLess(first["bytes"], 1024*1024)
            self.assertEqual(build.validate(out), first)
            (out / "error.wav").write_bytes(b"corrupt")
            with self.assertRaisesRegex(ValueError, "stale sound payload"):
                build.validate(out)

    def test_sounds_control_render(self):
        desktop = module("sounds_desktop_tests", ROOT / "tests/host/desktop/test_deskui.py")
        folder = ROOT / "build/codex"; folder.mkdir(parents=True, exist_ok=True)
        frame = folder / "f4-12-sounds.xrgb"
        self.harness("sounds-control", ["tests/host/desktop/sounds_control_test.c", "apps/control/app.c", "apps/control/display.c", "apps/desktop/protocol.c", "apps/desktop/sounds.c",
                    "apps/desktop/settings.c", "apps/desktop/wallpaper.c", "apps/desktop/image.c", "apps/control/fonts.c",
                    "sdk/libfont/font.c", "apps/libdeskui/text_ttf.c", *desktop.TOOLKIT], frame)
        self.assertEqual(frame.stat().st_size, 560*420*4)
        from PIL import Image
        Image.frombytes("RGB", (560,420), frame.read_bytes(), "raw", "BGRX").save(folder / "f4-12-sounds.png")
        frame.unlink()

if __name__ == "__main__":
    unittest.main()
