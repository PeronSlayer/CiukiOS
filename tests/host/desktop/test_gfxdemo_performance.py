"""Graphics Demo host frame pipeline. Run one suite at a time with nice -n 19.
Modern TSC/1GHz is directional, not a Pentium III simulation. No guest runs.
"""
# SPDX-License-Identifier: MIT
import argparse
import importlib.util
import io
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[3]

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

bench = load("gfx_compositor_bench", ROOT / "tests/host/desktop/test_compositor_performance.py")
acc = load("gfx_acc", ROOT / "tests/host/desktop/test_accessories.py")

def run(sanitize=False, reference=None, runtime=False, pipeline_reference=False):
    parent = ROOT / "build/host"
    parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="gfx-budget-", dir=parent) as temp:
        folder = Path(temp)
        source_root = ROOT
        if reference:
            source_root = folder / "reference"
            source_root.mkdir()
            archive = subprocess.run(["git", "archive", "--format=tar", reference,
                                      "apps/desktop", "apps/gfxdemo", "apps/accessories", "apps/libdeskui",
                                      "apps/files", "apps/dosbox", "tests/host/desktop", "tests/host/fbdev_test.c",
                                      "src/kernel/drivers/fbdev.c", "src/kernel/drivers/fbdev_probe.c"], cwd=ROOT, capture_output=True, check=True).stdout
            with tarfile.open(fileobj=io.BytesIO(archive)) as files:
                files.extractall(source_root, filter="data")
            for fixture in ("gfxdemo_performance.c", "gfxdemo_upload.c"):
                shutil.copy2(ROOT / "tests/host/desktop" / fixture, source_root / "tests/host/desktop")
        include = folder / "include/ciuki"
        include.mkdir(parents=True)
        for header in ("channel.h", "surface.h"):
            shutil.copy2(ROOT / "sdk/sysroot-overlay/include/ciuki" / header, include / header)
        sources = [s for s in bench.SOURCES if not s.endswith("compositor_benchmark.c")]
        sources += [s for s in acc.TOOLKIT if s not in sources]
        sources += ["apps/gfxdemo/app.c", "apps/gfxdemo/scene.c", "apps/accessories/acc.c",
                    "tests/host/desktop/resize_transport.c", "tests/host/desktop/resize_client.c",
                    "tests/host/desktop/gfxdemo_performance.c"]
        if runtime:
            sources[-1] = "tests/host/desktop/gfxdemo_frame_test.c"
        if not runtime:
            sources.append("tests/host/desktop/gfxdemo_upload.c")
        binary = folder / "gfx-budget"
        flags = (["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"] if sanitize else
                 ["-O2", "-fno-strict-aliasing", "-fno-builtin", "-mno-sse", "-mno-sse2", "-mno-mmx"])
        objects = []
        for i, source in enumerate(sources):
            obj = folder / f"{i}.o"
            subprocess.run(["clang", "-std=c17", *flags, "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-function", *(["-D_POSIX_C_SOURCE=200809L"] if source.endswith(("resize_client.c", "gfxdemo_frame_test.c")) else []),
                            *(["-DGFX_PIPELINE_REFERENCE" if pipeline_reference else "-DGFX_REFERENCE"] if reference else []),
                            "-I", str(source_root / "apps/desktop"), "-I", str(ROOT / "src/kernel/include"),
                            "-I", str(source_root / "apps/libdeskui"), "-I", str(include.parent),
                            "-c", str(source_root / source), "-o", str(obj)], check=True)
            objects.append(str(obj))
        subprocess.run(["clang", *flags, *objects, "-o", str(binary)], check=True)
        portrait, store = bench.assets()
        result = subprocess.run([str(binary), *([] if runtime else [str(portrait), str(store), "Platinum"])], capture_output=True, text=True,
                                env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        return result.stdout

class GraphicsPerformanceTests(unittest.TestCase):
    def test_runtime_completion_resize_and_bounds(self):
        output = run(sanitize=True, runtime=True)
        self.assertIn("PASS actual runtime completion FPS", output)
        print(output)

    def test_frame_pipeline_scanout(self):
        output = run(sanitize=True)
        self.assertIn("PASS Graphics Demo frame budget", output)
        self.assertEqual(sum(line.startswith("GFX_STAGE ") for line in output.splitlines()), 36)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--benchmark", action="store_true")
    parser.add_argument("--reference")
    parser.add_argument("--pipeline-reference", action="store_true", help="reference the f2-59 source-copy pipeline, rather than the older full-window scaler")
    args, remaining = parser.parse_known_args()
    if args.benchmark:
        if args.pipeline_reference and not args.reference:
            parser.error("--pipeline-reference requires --reference")
        print(run(reference=args.reference, pipeline_reference=args.pipeline_reference), end="")
    else:
        unittest.main(argv=[__file__, *remaining])
