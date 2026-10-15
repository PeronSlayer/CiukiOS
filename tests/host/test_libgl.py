"""TinyGL offscreen goldens, pixel semantics, host timings and target recipe."""
# SPDX-License-Identifier: MIT
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def offscreen(sanitize=True, seconds=None):
    gl = load("offscreen_gl", ROOT / "sdk/libgl/build.py")
    library = gl.host(sanitize=sanitize)
    parent = ROOT / "build/host"
    with tempfile.TemporaryDirectory(prefix="opengl-", dir=parent) as temp:
        folder = Path(temp)
        portrait = load("opengl_portrait", ROOT / "apps/desktop/convert_portrait.py")
        texture = folder / "portrait"
        texture.write_bytes(portrait.convert(ROOT / "assets/brand/ciuki-logo.png"))
        binary = folder / "render"
        flags = ["-std=c17", "-O2", "-ffp-contract=off", "-Wall", "-Wextra", "-Werror"]
        if sanitize:
            flags += ["-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]
        subprocess.run(["clang", *flags, *gl.includes(), "-I", str(ROOT / "src/kernel/include"),
                        str(ROOT / "tests/host/gl_render.c"), str(ROOT / "apps/gfxdemo/scene.c"),
                        str(library), "-lm", "-o", str(binary)], check=True)
        result = subprocess.run([str(binary), str(texture), temp,
                                 *(["--benchmark", str(seconds)] if seconds else [])],
                                capture_output=True, text=True,
                                env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        digests = {p.stem: hashlib.sha256(p.read_bytes()).hexdigest() for p in folder.glob("*.xrgb")}
        review = load("opengl_review", ROOT / "tests/host/desktop/frame_review.py")
        for p in folder.glob("*.xrgb"):
            review.capture("opengl-" + p.stem, p.read_bytes(), 640, 480)
        return result.stdout, digests


class OpenGLTests(unittest.TestCase):
    def test_offscreen_scenes_and_pixel_contract(self):
        output, digests = offscreen()
        print(output, end="")
        print("GL_GOLDENS", digests)
        expected = json.loads((ROOT / "tests/host/opengl-goldens.json").read_text())
        self.assertEqual(digests, expected)

    def test_host_frame_time_sanity(self):
        output, _ = offscreen(sanitize=False, seconds=1)
        print(output, end="")
        self.assertEqual(output.count("GL_HOST "), 3)

    def test_target_archive_reproducibility(self):
        sdk = ROOT / "build/tools/ciuki-sdk"
        if not (sdk / "manifest.json").exists():
            self.skipTest("build SDK before target reproducibility qualification")
        gl = load("target_gl", ROOT / "sdk/libgl/build.py")
        m = json.loads((sdk / "manifest.json").read_text())
        resource = subprocess.check_output([m["tools"]["clang"]["path"], "-print-resource-dir"], text=True).strip()
        flags = [*m["flags"], "-nostdinc", "-isystem", resource + "/include", "-I", str(sdk / "sysroot/include")]
        with tempfile.TemporaryDirectory(prefix="gl-repro-", dir=ROOT / "build/host") as temp:
            first = gl.build(Path(temp) / "first", cc=m["tools"]["clang"]["path"], ar=m["tools"]["llvm-ar"]["path"], flags=flags)
            second = gl.build(Path(temp) / "second", cc=m["tools"]["clang"]["path"], ar=m["tools"]["llvm-ar"]["path"], flags=flags)
            self.assertEqual(gl.digest(first), gl.digest(second))
            self.assertEqual(gl.digest(first), gl.digest(sdk / "sysroot/lib/libgl.a"))
            load("gl_audit", ROOT / "sdk/tests/check_sdk.py").audit_code(first, m)
            print("PASS libgl target deterministic archive, installed parity, x87/no SSE/MMX audit")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--benchmark", type=int, choices=range(1, 61))
    args, remaining = parser.parse_known_args()
    if args.benchmark:
        print(offscreen(sanitize=False, seconds=args.benchmark)[0], end="")
    else:
        unittest.main(argv=[__file__, *remaining])
