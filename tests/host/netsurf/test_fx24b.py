"""FX-24B large decoder and CSS selector regressions (no guest)."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
SAN = dict(os.environ, ASAN_OPTIONS="detect_leaks=1", UBSAN_OPTIONS="halt_on_error=1")


class SecondPartTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location("recipe", ROOT / "sdk/netsurf/build.py")
        recipe = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(recipe)
        recipe.build(host=True)
        cls.base = ROOT / "build/external/netsurf-host"
        cls.source = cls.base / "source"
        cls.original = recipe.sources()["archive"]
        cls.out = ROOT / "build/host/netsurf"
        cls.out.mkdir(parents=True, exist_ok=True)

    def test_large_decoders(self):
        netsurf = self.source / "netsurf"
        prefix = self.base / "prefix"
        # Exercise the exact decoder bodies without the content-factory table:
        # ASan retains that unused table and its whole browser dependency graph.
        # Actual handler registration is covered by the rendered-page tests.
        decoder_include = self.out / "decoder-include"
        (decoder_include / "image").mkdir(parents=True, exist_ok=True)
        for name in ("png", "webp"):
            text = (netsurf / f"content/handlers/image/{name}.c").read_text()
            (decoder_include / f"image/{name}.c").write_text(text.split("static const content_handler ", 1)[0])
        flags = ["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra",
                 "-Wno-unused-function", "-Wno-unused-parameter", "-ffunction-sections", "-fdata-sections",
                 "-fsanitize=address,undefined", "-fsanitize-address-globals-dead-stripping", "-fno-sanitize-recover=undefined", "-I" + str(ROOT),
                 "-I" + str(netsurf), "-I" + str(netsurf / "include"),
                 "-I" + str(decoder_include), "-I" + str(netsurf / "content/handlers"), "-I" + str(prefix / "include"),
                 "-I" + str(netsurf / "content/handlers/image"),
                 "-I" + str(self.source / "libnsgif"), "-I" + str(self.source / "libnsgif/src")]
        fixture = HERE / "fixtures/fx24b"
        for kind, names in (("PNG", ("large.png", "adam7.png")),
                            ("WEBP", ("large.webp", "lossy.webp")), ("GIF", ("large.gif", "animated.gif"))):
            binary = self.out / ("large-" + kind.lower())
            sources = [HERE / "large_image_test.c"]
            sources += [self.source / "libnsgif/src/lzw.c"] if kind == "GIF" else [netsurf / "desktop/bitmap.c"]
            libraries = [] if kind == "GIF" else ["-lnsgif", "-lpng", "-lwebp", "-lz"]
            subprocess.run([*flags, "-DTEST_" + kind, *map(str, sources), "-Wl,--gc-sections",
                            "-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc", "-L" + str(prefix / "lib"),
                            *libraries, "-lm", "-o", str(binary)], check=True)
            for name in names:
                for free in (56 * 1024 * 1024, 16 * 1024 * 1024):
                    with self.subTest(image=name, free=free):
                        subprocess.run([str(binary), str(fixture / name), str(fixture / (name + ".samples")),
                                        "4" if name == "animated.gif" else "1"], check=True, timeout=60,
                                       env=dict(SAN, CIUKI_NETSURF_FREE_BYTES=str(free)))

    def test_large_rendered_images(self):
        from styled import pixel
        fixture = HERE / "fixtures/fx24b"
        names = ("large.png", "adam7.png", "large.gif", "large.webp", "lossy.webp")
        with tempfile.TemporaryDirectory(dir=self.out) as directory:
            page = Path(directory) / "large.html"
            capture = Path(directory) / "large.xrgb"
            page.write_text('<!doctype html><title>Large Ciuki raster fixtures</title>'
                            '<style>body {margin:0;background:white} img {position:absolute;width:160px;height:120px}</style>'
                            + "".join(f'<img src="{(fixture / name).as_uri()}" style="left:{10+(i%3)*180}px;top:{10+(i//3)*140}px">'
                                      for i, name in enumerate(names)))
            completed = subprocess.run([str(self.base / "stage/Programs/netsurf"), "-w", "640", "-h", "480",
                                        "--ciuki-resources=" + str(self.base / "stage/system/netsurf"),
                                        "--ciuki-profile=" + str(Path(directory) / "profile"),
                                        "--ciuki-capture=" + str(capture), page.as_uri()],
                                       capture_output=True, text=True, timeout=30)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertNotIn("NetSurf error page:", completed.stderr)
            raw = capture.read_bytes()
            for i, name in enumerate(names):
                left, top = 10 + (i % 3) * 180, 10 + (i // 3) * 140
                bottom_right = (255, 255, 255) if name.endswith(("png", "large.webp")) else (255, 255, 0)
                for x, y, want in ((40, 30, (255, 0, 0)), (120, 30, (0, 255, 0)),
                                   (40, 90, (0, 0, 255)), (120, 90, bottom_right)):
                    got = pixel(raw, 640, left + x, top + y)
                    self.assertTrue(all(abs(a-b) <= 8 for a, b in zip(got, want)), (name, x, y, got, want))

    def test_upstream_css_selection(self):
        """Pinned expectations, with one explicit CSS flex shorthand correction."""
        css = self.source / "libcss"
        prefix = self.base / "prefix"
        binary = self.out / "css-select"
        driver = self.out / "css-select-fixture.c"
        # Flush the upstream driver's final success marker before sanitizer
        # shutdown, so a sandbox LSan failure cannot hide a golden mismatch.
        driver.write_text((css / "test/select.c").read_text().replace(
            'printf("PASS\\n");', 'printf("PASS\\n"); fflush(stdout);'))
        subprocess.run(["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                        "-include", "strings.h", "-I" + str(css / "src"), "-I" + str(css / "test"), "-I" + str(prefix / "include"),
                        str(driver), "-L" + str(prefix / "lib"), "-lcss", "-lparserutils",
                        "-lwapcaplet", "-lm", "-o", str(binary)], check=True)
        for fixture in sorted((css / "test/data/select").glob("*.dat")):
            with self.subTest(fixture=fixture.name):
                selected = fixture
                if fixture.name == "tests1.dat":
                    # Upstream case 65 expects flex:0 0 to mean shrink:1.
                    # Flexbox 1 requires the second number to be shrink:0.
                    # Keep the pinned input intact and apply this one reviewed
                    # expectation fix to a disposable copy, never rebaseline.
                    selected = self.out / "css-select-tests1.dat"
                    selected.write_bytes(fixture.read_bytes())
                    subprocess.run(["patch", "--batch", "--fuzz=0", str(selected),
                                    str(HERE / "fixtures/flex-shorthand-oracle.patch")], check=True)
                completed = subprocess.run([str(binary), str(selected)], env=SAN,
                                           capture_output=True, text=True, timeout=30)
                self.assertTrue(completed.stdout.endswith("PASS\n"), completed.stdout + completed.stderr)
                print("PASS FX-24B upstream CSS:", fixture.name, "pinned computed styles and dictionary lifetime")
                if completed.returncode:
                    print(completed.stderr, file=sys.stderr, end="")
                completed.check_returncode()

    def test_css_cost(self):
        css = self.source / "libcss"
        prefix = self.base / "prefix"
        for baseline in (False, True):
            with self.subTest(baseline=baseline):
                binary = self.out / ("css-cost-baseline" if baseline else "css-cost")
                source = self.original / "libcss/src/select/select.c" if baseline else css / "src/select/select.c"
                flags = ["-DCSS_BASELINE"] if baseline else []
                subprocess.run(["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O2", "-g",
                                "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                                "-I" + str(css / "src"), "-I" + str(ROOT), "-I" + str(prefix / "include"),
                                '-DCSS_SELECTION_SOURCE="' + str(source) + '"', *flags,
                                str(HERE / "css_cost_test.c"), "-L" + str(prefix / "lib"),
                                "-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc", "-lcss", "-lparserutils",
                                "-lwapcaplet", "-lm", "-o", str(binary)], check=True)
                subprocess.run([str(binary)], check=True, env=SAN, timeout=30)


if __name__ == "__main__":
    unittest.main()
