"""Native PIO/VRAM model, lifecycle and target compilation. Sequential; no guest."""
from pathlib import Path
import importlib.util
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "src/kernel/drivers/video/nv2d"

class NativeTests(unittest.TestCase):
    def test_pci_contract(self):
        matches = [int(n, 16) for n in re.findall(r'\{0x10de,0x([0-9a-f]{4}),3,255\}',
            (DRIVER / "matches.h").read_text())]
        self.assertEqual(len(matches), 168)
        self.assertEqual(len(matches), len(set(matches)))
        contract = (ROOT / "docs/design/video-driver-families.md").read_text()
        for family in ("FX", "GeForce6"):
            census = re.search(r'\*\*NVIDIA ' + family + r' \(\d+\):\*\*([^\n]+)', contract)[1]
            self.assertTrue(set(int(n, 16) for n in re.findall(r'10de:([0-9a-f]{4})', census)) <= set(matches))
        for excluded in (0x0189, 0x0329, 0x016a, 0x00f5, 0x0090, 0x01d1, 0x0290, 0x0390):
            self.assertNotIn(excluded, matches)

    def test_adapter(self):
        folder = ROOT / "build/host"
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="nv2d-adapter-", dir=folder) as tmp:
            binary = Path(tmp) / "adapter"
            sanitizer = "address,undefined" if os.environ.get("CIUKI_DISPLAY_ASAN") else "undefined"
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=" + sanitizer, "-fno-sanitize-recover=undefined",
                "-I", str(ROOT / "src/kernel/include"), str(ROOT / "tests/host/nv2d_adapter_test.c"),
                str(ROOT / "src/kernel/drivers/video/registry.c"),
                str(DRIVER / "engine.c"), str(DRIVER / "hardware.c"), str(DRIVER / "dma_hw.c"),
                str(ROOT / "src/kernel/drivers/video/swdib.c"),
                str(ROOT / "src/kernel/lib/fmt.c"), str(ROOT/'tests/host/display_driver_stubs.c'), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_model(self):
        folder = ROOT / "build/host"
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="nv2d-", dir=folder) as tmp:
            binary = Path(tmp) / "model"
            sanitizer = "address,undefined" if os.environ.get("CIUKI_DISPLAY_ASAN") else "undefined"
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=" + sanitizer, "-fno-sanitize-recover=undefined",
                "-I", str(ROOT / "src/kernel/include"), str(ROOT / "tests/host/nv2d_test.c"),
                str(DRIVER / "engine.c"), str(DRIVER / "hardware.c"), str(DRIVER / "dma_hw.c"),
                str(ROOT / "src/kernel/drivers/video/swdib.c"), str(ROOT/'tests/host/display_driver_stubs.c'), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_software_3d(self):
        folder = ROOT / "build/host"
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="nv2d-render3d-", dir=folder) as tmp:
            binary = Path(tmp) / "fallback"
            sanitizer = "address,undefined" if os.environ.get("CIUKI_DISPLAY_ASAN") else "undefined"
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=" + sanitizer, "-fno-sanitize-recover=undefined",
                "-I", str(ROOT / "src/kernel/include"), str(ROOT / "tests/host/nv2d_render3d_test.c"),
                str(ROOT / "src/kernel/drivers/video/registry.c"),
                str(DRIVER / "engine.c"), str(DRIVER / "hardware.c"), str(DRIVER / "dma_hw.c"),
                str(ROOT / "src/kernel/drivers/video/swdib.c"),
                str(ROOT / "src/kernel/drivers/video/render3d.c"),
                str(ROOT / "src/kernel/drivers/video/render3d_scalar.c"),
                str(ROOT / "src/kernel/drivers/video/render3d_native.c"),
                str(ROOT / "src/kernel/drivers/video/render3d_transport.c"),
                str(ROOT / "src/kernel/lib/fmt.c"), str(ROOT / "tests/host/display_driver_stubs.c"),
                "-o", str(binary)], check=True)
            symbols = subprocess.check_output(["nm", "-g", "--defined-only", str(binary)], text=True)
            names = [line.split()[-1] for line in symbols.splitlines() if len(line.split()) == 3]
            self.assertEqual(len(names), len(set(names)), symbols)
            subprocess.run([str(binary)], check=True)

    def test_target(self):
        spec = importlib.util.spec_from_file_location("nv2d_flags", ROOT / "scripts/build_kernel.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        folder = ROOT / "build/host"
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="nv2d-target-", dir=folder) as tmp:
            for source in ("adapter.c", "engine.c", "hardware.c", "dma_hw.c"):
                subprocess.run(["clang", *module.CFLAGS, "-I", str(ROOT / "src/kernel/include"),
                    "-c", str(DRIVER / source), "-o", str(Path(tmp) / (source + ".o"))], check=True)
            symbols = subprocess.check_output(["nm", "-g", "--defined-only",
                *(str(p) for p in Path(tmp).glob("*.o"))], text=True)
            names = [line.split()[-1] for line in symbols.splitlines() if len(line.split()) == 3]
            self.assertEqual(len(names), len(set(names)), symbols)
            for name in names:
                self.assertTrue(name.startswith("nv2d_"), name)

if __name__ == "__main__":
    unittest.main()
