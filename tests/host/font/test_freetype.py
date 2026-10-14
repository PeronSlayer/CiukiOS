"""F2-63 minimal engine pin, dependency boundaries and reproducibility."""
# SPDX-License-Identifier: MIT
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("repro_freetype", ROOT / "sdk/freetype/build.py")
ft = importlib.util.module_from_spec(spec); spec.loader.exec_module(ft)


class FreeTypeBuildTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        (ROOT / "build/host/f2-63").mkdir(parents=True, exist_ok=True)

    def test_pinned_source_configuration_and_notices(self):
        source = ft.source()
        self.assertEqual(ft.pin()["version"], "2.13.3")
        self.assertIn("2.13.3", (source / "README").read_text())
        # Exercise the same include chain as the engine; directly including our
        # options misses filename collisions in FreeType's quoted includes.
        flags = ["clang", "-E", "-dM", *ft.includes(), "-x", "c", "-"]
        macros = subprocess.check_output(flags, input="#include <ft2build.h>\n#include FT_FREETYPE_H\n", text=True)
        for option in ("USE_BZIP2", "USE_BROTLI", "USE_PNG", "USE_HARFBUZZ", "USE_ZLIB", "USE_LZW", "ENVIRONMENT_PROPERTIES"):
            self.assertNotIn("#define FT_CONFIG_OPTION_" + option, macros)
        self.assertIn("#define TT_CONFIG_OPTION_BYTECODE_INTERPRETER", macros)
        self.assertIn("#define TT_CONFIG_OPTION_GX_VAR_SUPPORT", macros)
        self.assertNotIn("#define TT_CONFIG_OPTION_SUBPIXEL_HINTING", macros)
        # Preprocess the installed headers as SDK programs do: target flags and
        # the newlib sysroot only, never the host C library headers.
        sdk = ROOT / "build/tools/ciuki-sdk"
        resource = subprocess.check_output(["clang", "-print-resource-dir"], text=True).strip()
        target = json.loads((sdk / "manifest.json").read_text())["flags"]
        installed_macros = subprocess.check_output(["clang", *target, "-nostdinc", "-isystem", resource + "/include",
                                                    "-I", str(sdk / "sysroot/include"), "-E", "-dM", "-x", "c", "-"],
            input="#include <ft2build.h>\n#include FT_FREETYPE_H\n", text=True)
        for option in ("USE_BZIP2", "USE_BROTLI", "USE_PNG", "USE_HARFBUZZ", "USE_ZLIB", "USE_LZW", "ENVIRONMENT_PROPERTIES"):
            self.assertNotIn("#define FT_CONFIG_OPTION_" + option, installed_macros)
        self.assertIn("CiukiOS: do not run font bytecode", (source / "src/truetype/ttobjs.c").read_text())
        with tempfile.TemporaryDirectory(dir=ROOT / "build/host") as folder:
            ft.notices(folder)
            self.assertEqual((Path(folder) / "FreeType-FTL.txt").read_bytes(), (source / "docs/FTL.TXT").read_bytes())
            self.assertIn("FreeType team", (Path(folder) / "FreeType-NOTICE.txt").read_text())

    def test_independent_path_host_and_i686_archives_are_reproducible(self):
        sdk = ROOT / "build/tools/ciuki-sdk"
        self.assertTrue((sdk / "manifest.json").exists(), "build the SDK before target reproducibility qualification")
        manifest = json.loads((sdk / "manifest.json").read_text())
        resource = subprocess.check_output(["clang", "-print-resource-dir"], text=True).strip()
        target = [*manifest["flags"], "-nostdinc", "-isystem", resource + "/include", "-I", str(sdk / "sysroot/include")]
        with tempfile.TemporaryDirectory(prefix="ft-repro-", dir=ROOT / "build/host") as temp:
            folder = Path(temp)
            hashes = {}
            for name, flags in (("host", ["-std=c17", "-O2", "-fPIC"]), ("i686", target)):
                first = ft.build(folder / (name + "-first"), flags=flags)
                second = ft.build(folder / (name + "-second"), flags=flags)
                self.assertEqual(ft.digest(first), ft.digest(second), name)
                hashes[name] = ft.digest(first)
                undefined = subprocess.check_output(["llvm-nm", "--undefined-only", first], text=True)
                for library in ("BZ2_", "png_", "hb_", "inflate", "dlopen", "getenv"):
                    self.assertNotIn(library, undefined)
            (ROOT / "build/host/f2-63/reproducibility.json").write_text(json.dumps(hashes, indent=2) + "\n")


if __name__ == "__main__":
    (ROOT / "build/host").mkdir(parents=True, exist_ok=True)
    unittest.main()
