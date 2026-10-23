"""docs/credits.md, the About CiukiOS credits (apps/desktop/credits.inc), the pins in config/ and the
notice files must agree; no QEMU, no image build (directive f2-97)."""
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
GROUPS = ["System", "DOS", "Windows compatibility", "Network and browser", "Media", "Tools", "Icons"]
STATUSES = {"image", "build only", "source only", "integrating"}


def load_markdown():
    rows, group = [], None
    for line in (ROOT / "docs/credits.md").read_text().splitlines():
        if line.startswith("## "):
            group = line[3:].strip()
        elif line.startswith("| ") and group in GROUPS and not line.startswith("| Component") and not line.startswith("| ---"):
            cells = [c.strip() for c in line.strip().strip("|").split(" | ")]
            rows.append((group, *cells))
    return rows


def load_inc():
    text = (ROOT / "apps/desktop/credits.inc").read_text()
    groups = re.search(r'#define CREDIT_GROUPS (.*)', text).group(1)
    names = re.findall(r'"([^"]*)"', groups)
    rows = [(names[int(g)], n, v, l) for g, n, v, l in
            re.findall(r'^CREDIT\((\d+), "([^"]*)", "([^"]*)", "([^"]*)"\)$', text, re.M)]
    return names, rows


def pin(path, *keys):
    value = json.loads((ROOT / path).read_text())
    for key in keys:
        value = value[key]
    return value


class CreditsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.md = load_markdown()
        cls.inc_groups, cls.inc = load_inc()
        cls.by_name = {r[1]: r for r in cls.md}

    def test_about_dialog_data_matches_the_document(self):
        self.assertEqual(self.inc_groups, GROUPS)
        self.assertEqual([r[:4] for r in self.md], self.inc)
        self.assertEqual(len({r[1] for r in self.md}), len(self.md), "duplicate component")

    def test_rows_are_complete_and_one_line_in_the_dialog(self):
        for group, name, version, licence, notice, status in self.md:
            with self.subTest(name=name):
                self.assertTrue(all((name, version, licence, notice)))
                self.assertIn(status, STATUSES)
                self.assertLessEqual(len(name), 40)       # the About columns fit one line each
                self.assertLessEqual(len(version), 22)
                self.assertLessEqual(len(licence), 28)

    def test_notice_paths_exist(self):
        for group, name, version, licence, notice, status in self.md:
            with self.subTest(name=name):
                path = ROOT / notice.strip("`")
                if status == "integrating" and not path.exists():
                    continue                                # notice arrives with the integration (f2-95)
                self.assertTrue(path.exists(), notice)

    def test_every_third_party_directory_and_pinned_download_is_listed(self):
        text = (ROOT / "docs/credits.md").read_text()
        for needle in ("third_party/freedos", "third_party/jemm", "third_party/ctmouse", "third_party/DOSNavigator",
                       "third_party/vsbhda", "third_party/wine", "third_party/nsis", "third_party/bearssl",
                       "third_party/cacert", "third_party/jsvendor", "third_party/stb", "third_party/audio",
                       "third_party/minimp3", "third_party/tjpgd", "third_party/doomgeneric", "third_party/wolf4sdl", "ACPICA",
                       "third_party/games", "third_party/tcc", "third_party/drivers", "third_party/pixman",
                       "assets/icons", "assets/fonts", "config/openwatcom.json", "config/netsurf.json",
                       "config/sdk-pins.json", "docs/licenses/jitterentropy", "docs/licenses/lwip",
                       "Tango Icon Theme", "Ciuki portrait"):
            with self.subTest(needle=needle):
                self.assertIn(needle, text)
        # Only tracked directories: ignored local material (for example the owner's
        # private Resource Kit media under third_party/mswin98rskt) is never shipped.
        import subprocess
        tracked = subprocess.run(["git", "-C", str(ROOT), "ls-files", "third_party"], capture_output=True,
                                 text=True, check=True).stdout.splitlines()
        listed = {line.split("/")[1] for line in tracked if line.count("/") >= 2}
        for name in listed:
            self.assertIn("third_party/" + name, text, name)

    def test_versions_follow_the_pins(self):
        v = lambda name: self.by_name[name][2]
        expected = {
            "ACPICA": pin("src/kernel/acpi/manifest.json", "version"),
            "FreeType": pin("config/sdk-pins.json", "freetype", "version"),
            "newlib C library": re.search(r"newlib-([0-9.]+?)\.tar", pin("config/sdk-pins.json", "newlib", "archive")).group(1),
            "Lua": re.search(r"lua-([0-9.]+?)\.tar", pin("config/sdk-pins.json", "lua", "source", "archive")).group(1),
            "jitterentropy-library": pin("config/jitterentropy.json", "version"),
            "Open Watcom C/C++": pin("config/openwatcom.json", "version"),
            "lwIP": pin("src/kernel/net/lwip/import.json", "version"),
            "NetSurf and its libraries": re.search(r"netsurf-all-([0-9.]+?)\.tar", pin("config/netsurf.json", "archive", "file")).group(1),
            "zlib": re.search(r"zlib-([0-9.]+?)\.tar", pin("config/netsurf.json", "zlib", "file")).group(1),
            "libpng": re.search(r"libpng-([0-9.]+?)\.tar", pin("config/netsurf.json", "libpng", "file")).group(1),
            "libjpeg-turbo": re.search(r"libjpeg-turbo-([0-9.]+?)\.tar", pin("config/netsurf.json", "libjpeg", "file")).group(1),
            "Expat": re.search(r"expat-([0-9.]+?)\.tar", pin("config/netsurf.json", "expat", "file")).group(1),
            "libwebp (decoder)": re.search(r"libwebp-([0-9.]+?)\.tar", pin("config/netsurf.json", "libwebp", "file")).group(1),
            "Wine reused DLLs (16 PEs)": pin("config/wine-pin.json", "release"),
            "NSIS installer fixture": re.search(r"nsis-([0-9.]+)-src", pin("config/nsis.json", "source", "file")).group(1),
            "Tiny C Compiler": pin("third_party/tcc/UPSTREAM.json", "release"),
            "FreeDOS kernel (unified base)": pin("third_party/freedos/1.3/UPSTREAM.json", "kernel", "version"),
            "FreeCOM": pin("third_party/freedos/1.3/UPSTREAM.json", "shell", "version"),
        }
        for name, want in expected.items():
            with self.subTest(name=name):
                self.assertIn(want, v(name))
        text = lambda path: (ROOT / path).read_text()
        self.assertIn(v("BearSSL"), re.search(r"BearSSL ([0-9.]+)", text("third_party/bearssl/README.ciuki")).group(1))
        self.assertIn("0.8.90", text("assets/icons/README.md"))
        self.assertEqual(v("Tango Icon Theme"), "0.8.90")
        self.assertIn(pin("third_party/vsbhda/UPSTREAM.json", "commit")[:7], v("VSBHDA and DBOPL (OPL3)"))
        self.assertIn(pin("third_party/doomgeneric/UPSTREAM.json", "commit")[:7], v("doomgeneric (DOOM engine)"))
        self.assertIn(pin("third_party/wolf4sdl/UPSTREAM.json", "commit")[:8], v("Wolf4SDL (Wolfenstein 3D engine)"))
        self.assertIn(pin("third_party/jemm/UPSTREAM.json", "commit")[:8], v("Jemm and JLOAD"))
        self.assertIn("2.30", text("third_party/stb/README.md"))
        self.assertEqual(v("stb_image"), "2.30")
        self.assertIn(v("minimp3"), text("third_party/minimp3/README.md"))
        self.assertIn(v("TJpgDec"), "R" + re.search(r"R([0-9.]+)", text("third_party/tjpgd/README.md")).group(1))

    def test_licences_follow_the_pins(self):
        lic = lambda name: self.by_name[name][3]
        self.assertEqual(lic("Open Watcom C/C++"), pin("config/openwatcom.json", "license"))
        self.assertEqual(lic("FreeType").split()[0], pin("config/sdk-pins.json", "freetype", "license_choice"))
        self.assertIn(pin("src/kernel/acpi/manifest.json", "license"), lic("ACPICA"))
        self.assertIn("BSD-3", lic("jitterentropy-library"))
        self.assertIn("GPL-2.0", lic("jitterentropy-library"))
        self.assertIn("BSD-3-Clause", pin("src/kernel/net/lwip/import.json", "license"))
        self.assertIn("BSD-3-Clause", lic("lwIP"))
        self.assertIn("GNU LESSER GENERAL PUBLIC LICENSE", (ROOT / "third_party/wine/COPYING.LIB").read_text().upper())
        self.assertEqual(lic("Wine reused DLLs (16 PEs)"), "LGPL-2.1-or-later")
        self.assertIn("LGPL-2.1", (ROOT / "third_party/tcc/README.md").read_text())
        self.assertIn("GPL-2.0-or-later", pin("third_party/vsbhda/UPSTREAM.json", "license"))
        self.assertTrue(pin("third_party/wolf4sdl/UPSTREAM.json", "license").startswith("GPL-2.0-or-later"))
        self.assertTrue(pin("third_party/doomgeneric/UPSTREAM.json", "license").startswith("GPL-2.0-or-later"))
        self.assertTrue((ROOT / "LICENSE").read_text().lstrip().startswith("GNU GENERAL PUBLIC LICENSE"))
        self.assertTrue((ROOT / "sdk/LICENSE").read_text().startswith("MIT License"))
        self.assertIn("Public Domain", (ROOT / "assets/icons/README.md").read_text())

    def test_the_wine_dll_count_and_identity_are_current(self):
        self.assertEqual(len(pin("config/wine-upper.json", "fit", "reused_dlls")), 16)
        self.assertIn("sixteen reused Wine 10.0 PEs", pin("config/release-policy.json", "licensing"))

    def test_identity_lines(self):
        text = (ROOT / "docs/credits.md").read_text()
        self.assertIn("Classic computing, one woof at a time.", text)
        self.assertIn("Ciuki", text)
        self.assertIn("pixman", text)
        self.assertIn("MIT", self.by_name["pixman, X11 regions (integrating)"][3])


if __name__ == "__main__":
    unittest.main()
