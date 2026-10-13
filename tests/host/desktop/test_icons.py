"""Tango 0.8.90 system icon store and icon map (host tests; never boots QEMU).

The store is pinned by SHA-256 (apps/desktop/tango-store.json and below);
its Python and C readers must reproduce the archive's PNG pixels exactly,
every icon_map.c entry must resolve at 16, 22, 32 and 48 px, and the Public
Domain notice must stay with the archive and in the store. Scratch lives
under build/host; the contact sheet is kept at build/host/icons/contact-sheet-32.png.
"""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
STORE_SHA256 = "5892ae32d3f1a121c6fb06dc0d9848e00923fe080919e72ee42fad5612bfceb4"
STORE_BYTES = 726504
DECODED_SHA256 = "a8a5101d4c0953ea455dacaa5cb5844925659de7ab8f7ca1744ec4c7120904e2"
COPYING_SHA256 = "4f740497cf8ad0e564de90d3416a06c3d6b1d69d6883fe539925e3353d68a466"
AUTHORS = ["Ulisse Perusin", "Steven Garrity", "Lapo Calamandrei", "Ryan Collier", "Rodney Dawes",
           "Andreas Nilsson", "Tuomas Kuosmanen", "Garrett LeSage", "Jakub Steiner"]
SIZES = (16, 22, 32, 48)
FLAGS = ["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-D_POSIX_C_SOURCE=200809L",
         "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]
# Objects the desktop, its programs and the directive name; identity objects use the portrait.
REQUIRED = {
    "program:files": "system-file-manager", "program:ciuknote": "accessories-text-editor",
    "program:tasks": "utilities-system-monitor", "program:dosbox": "utilities-terminal", "program:terminal": "utilities-terminal",
    "program:demo": None, "program:hello": None, "program:lua": None, "program:libc_smoke": None,
    "menu:settings": None, "menu:run": None, "menu:help": None, "menu:about": "ciuki", "menu:shutdown": None,
    "menu:restart": None, "menu:logout": None, "menu:start": "ciuki",
    "action:back": "go-previous", "action:forward": "go-next", "action:up": "go-up", "action:cut": "edit-cut",
    "action:copy": "edit-copy", "action:paste": "edit-paste", "action:delete": "edit-delete",
    "action:properties": "document-properties", "action:views": None, "action:new-folder": "folder-new",
    "action:refresh": "view-refresh", "action:save": "document-save", "action:open": "document-open",
    "place:computer": "ciuki", "place:home": "user-home", "place:desktop": "user-desktop",
    "place:harddisk": "drive-harddisk", "place:removable": "drive-removable-media", "place:optical": "drive-optical",
    "place:network": "network-workgroup", "device:computer": "ciuki",
    "type:folder": "folder", "type:unknown": None, "type:executable": "application-x-executable",
    "type:.txt": "text-x-generic", "type:.png": "image-x-generic", "type:.wav": "audio-x-generic",
    "type:.avi": "video-x-generic", "type:.zip": "package-x-generic", "type:.com": "application-x-executable",
    "type:.exe": "application-x-executable", "type:.bat": "text-x-script", "type:.lua": "text-x-script",
    "type:.cfg": None, "type:.ini": None,
    "status:volume-high": "audio-volume-high", "status:volume-medium": "audio-volume-medium",
    "status:volume-low": "audio-volume-low", "status:volume-muted": "audio-volume-muted", "status:battery": "battery",
    "device:network": "network-wired", "status:network-offline": "network-offline", "status:network-error": "network-error",
    "dialog:info": "dialog-information", "dialog:warning": "dialog-warning", "dialog:error": "dialog-error",
    "dialog:question": "help-browser",
}


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class IconStoreTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.icons = load("test_icons_convert", ROOT / "apps/desktop/convert_icons.py")
        cls.assets = load("test_icons_assets", ROOT / "tests/host/desktop/icon_assets.py")
        cls.data, cls.record = cls.icons.build_store()
        (ROOT / "build/host").mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix="icons-", dir=ROOT / "build/host")
        cls.folder = Path(cls.temp.name)
        cls.env = dict(os.environ, TMPDIR=cls.temp.name, ASAN_OPTIONS="detect_leaks=0")
        cls.binary = cls.folder / "icons_test"
        subprocess.run([*FLAGS, "-I", str(ROOT / "apps/libdeskui"), "-I", str(ROOT / "src/kernel/include"),
                        *cls.assets.defines(), str(ROOT / "tests/host/desktop/icons_test.c"),
                        str(ROOT / "tests/host/desktop/icon_io.c"), "-Wl,--wrap=pread",
                        *(str(ROOT / s) for s in ["apps/libdeskui/draw.c", *cls.assets.SOURCES, "apps/desktop/compositor.c", "apps/desktop/theme.c"]),
                        "-o", str(cls.binary)], check=True, env=cls.env)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def harness(self, *args, env=None):
        result = subprocess.run([str(self.binary), *map(str, args)], env={**self.env, **(env or {})},
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_store_deterministic_and_pinned(self):
        again, record = self.icons.build_store()
        self.assertEqual(again, self.data)
        self.assertEqual(record, self.record)
        self.assertEqual(hashlib.sha256(self.data).hexdigest(), STORE_SHA256)
        self.assertEqual(len(self.data), STORE_BYTES)
        self.assertEqual(self.icons.store_record(), self.record)      # tango-store.json is current
        self.assertEqual(self.record["decoded_sha256"], DECODED_SHA256)
        self.icons.check_store(self.data)
        self.icons.check_store_record()                               # every member hash against the archive
        self.assertEqual(self.record["archive_sha256"], self.icons.ARCHIVE_SHA)
        self.assertEqual(hashlib.sha256(self.icons.ARCHIVE.read_bytes()).hexdigest(), self.icons.ARCHIVE_SHA)
        self.assertEqual(len(self.record["members"]), 645)            # every PNG of 16x16, 22x22 and 32x32
        self.assertEqual((self.record["names"], self.record["images"]), (246, 984))
        self.assertEqual(hashlib.sha256(self.assets.store().read_bytes()).hexdigest(), STORE_SHA256)
        print(f"PASS store: {len(self.data)} bytes, sha256 {STORE_SHA256[:16]}..., {self.record['names']} names, "
              f"{self.record['images']} images ({self.record['qoi_images']} QOI, {self.record['scaled_images']} scaled)")

    def test_python_reader_round_trip(self):
        parsed = self.icons.parse_store(self.data)
        sources = self.icons.store_sources()
        self.assertEqual(set(parsed) - {""}, set(sources))
        digest = hashlib.sha256()
        for name in sorted(sources, key=lambda n: n.encode()):
            png = sources[name]["png"]
            self.assertEqual(parsed[name]["context"], sources[name]["context"])
            for size in SIZES:
                expected = png[size][2] if size in png else self.icons.area_scale(png[32][2], 32, size)
                self.assertEqual(parsed[name][size], expected, (name, size))
                digest.update(name.encode() + b"\0" + bytes((size,)) + expected)
        self.assertEqual(digest.hexdigest(), DECODED_SHA256)
        state, raw = 12345, bytearray()                               # QOI codec on runs, small/large deltas, alpha
        for k in range(5000):
            state = (state * 1103515245 + 12345) & 0x7fffffff
            mode = state % 5
            last = raw[-4:] if raw else b"\0\0\0\xff"
            raw += (bytes(last) if mode == 0 else bytes(((last[0] + 1) & 255, last[1], (last[2] - 2) & 255, last[3])) if mode == 1
                    else bytes(((last[0] + 20) & 255, (last[1] + 25) & 255, (last[2] + 30) & 255, last[3])) if mode == 2
                    else state.to_bytes(4, "little"))
        raw = bytes(raw)
        self.assertEqual(self.icons.qoi_decode(self.icons.qoi_encode(raw), len(raw) // 4), raw)
        print("PASS Python reader: every name and size equals the archive PNG (48 px: area filter of 32 px)")

    def test_png_decoder_matches_pillow(self):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest("Pillow unavailable (independent PNG cross-check)")
        with tarfile.open(self.icons.ARCHIVE) as archive:
            members = [m for m in archive.getmembers() if m.name.endswith(".png")]
            for member in members:
                raw = archive.extractfile(member).read()
                self.assertEqual(self.icons.png_rgba(raw)[2], Image.open(io.BytesIO(raw)).convert("RGBA").tobytes(), member.name)
        print(f"PASS PNG decoder: {len(members)} members equal Pillow's RGBA (8- and 16-bit)")

    def test_c_reader_round_trip_and_identity(self):
        out = self.folder / "decoded.bin"
        stdout = self.harness("dump", out)
        self.assertIn("DUMP names=246 failures=0", stdout)
        stream = out.read_bytes()
        marker = b"ciuki\0"
        split = stream.index(marker + bytes((16,)))
        self.assertEqual(hashlib.sha256(stream[:split]).hexdigest(), DECODED_SHA256)
        c = load("test_icons_portrait", ROOT / "apps/desktop/convert_portrait.py")
        xrgb = self.assets.portrait().read_bytes()
        self.assertEqual(hashlib.sha256(xrgb).hexdigest(), c.ARRAY_SHA256)
        rgba = b"".join(bytes((xrgb[i + 2], xrgb[i + 1], xrgb[i], 255)) for i in range(0, len(xrgb), 4))
        at = split
        for size in SIZES:
            expected = self.icons.area_scale(rgba, 256, size)
            self.assertEqual(stream[at:at + len(marker) + 1], marker + bytes((size,)))
            at += len(marker) + 1
            self.assertEqual(stream[at:at + size * size * 4], expected, size)
            at += size * size * 4
            if 256 % size == 0:                                       # Files' box filter (app.c shrink)
                block = 256 // size
                for y in range(0, size, 5):
                    for x in range(0, size, 5):
                        for ch in range(3):
                            total = sum(rgba[((y * block + yy) * 256 + x * block + xx) * 4 + ch]
                                        for yy in range(block) for xx in range(block))
                            self.assertEqual(expected[(y * size + x) * 4 + ch], (total + block * block // 2) // (block * block))
        self.assertEqual(at, len(stream))
        print("PASS C reader: 246 names x 4 sizes equal the pinned decoded stream; ciuki = approved portrait, area filter")

    def test_every_mapping_resolves_at_four_sizes(self):
        lines = [l.split() for l in self.harness("map").splitlines()]
        table = {l[0]: l[1] for l in lines}
        self.assertEqual(len(table), len(lines), "duplicate icon_map.c objects")
        bad = [l for l in lines if l[2:] != ["16", "22", "32", "48"]]
        self.assertEqual(bad, [])
        store = set(self.icons.parse_store(self.data)) - {""}
        self.assertEqual({t for t in table.values() if t not in store}, {"ciuki"})
        for obj, tango in REQUIRED.items():
            self.assertIn(obj, table)
            if tango:
                self.assertEqual(table[obj], tango, obj)
        programs = set(re.findall(r'"/Programs/([A-Za-z0-9_.-]+)"', (ROOT / "scripts/build_image.py").read_text()))
        self.assertEqual({p for p in programs if f"program:{p}" not in table}, set())
        for obj, tango in table.items():
            if tango == "ciuki":
                self.assertTrue(obj.endswith((":computer", ":about", ":start")), obj)
        print(f"PASS icon map: {len(lines)} objects resolve at 16/22/32/48; every /Programs program mapped; identity = portrait")

    def test_shell_tables_match_icon_map(self):
        """One icon per object (owner feedback 2026-10-12: the icon set must not change from screen to screen): the
        shell's program table, Start menu rows, desktop icons and games use the Tango names of icon_map.c, which
        Files (/Programs) and Task Manager use for the same programs."""
        table = {l.split()[0]: l.split()[1] for l in self.harness("map").splitlines()}
        shell = (ROOT / "apps/desktop/shell.c").read_text()
        catalog = (ROOT / "apps/desktop/catalog.h").read_text()
        programs = re.findall(r'\{"(\w+)", "[^"]+", "([a-z-]+)", "(?:programs|accessories|developer)"\}', shell)
        accessories = re.findall(r'\{"(\w+)", "[^"]+", "([a-z-]+)", [01], "[^"]*"\}', catalog)
        self.assertEqual(len(programs), 21)
        self.assertEqual(len(accessories), 8)
        for name, tango in programs + accessories:
            self.assertEqual(tango, table[f"program:{name}"], name)
        games = re.findall(r'\{"[^"]+", "[^"]+", "[^"]+", "([a-z-]+)"\}', catalog)
        self.assertEqual(set(games), {table["menu:game"]})
        rows = dict(re.findall(r'item\(m, "([^"]+)", "([a-z-]+)"', shell))
        objects = {"Programs": "menu:programs", "Documents": "menu:documents", "Settings": "menu:settings", "Help": "menu:help",
                   "Run...": "menu:run", "Shut Down...": "menu:shutdown", "Accessories": "menu:accessories", "Games": "menu:games",
                   "Developer": "menu:development", "Control Panel": "menu:control-panel", "My Documents": "place:home",
                   "Display": "settings:display", "Mouse": "settings:mouse", "Keyboard": "settings:keyboard",
                   "Sounds": "settings:sound", "Date & Time": "settings:date-time", "Task Manager": "program:tasks",
                   "Keyboard Shortcuts": "settings:shortcuts", "Refresh": "action:refresh", "Properties": "action:properties",
                   "Close    Alt+F4": "action:stop", "About CiukiOS": "menu:about"}
        for label, obj in objects.items():
            self.assertEqual(rows[label], table[obj], label)
        icons = dict(re.findall(r'icon_add\(d, "([^"]+)", "[^"]*", (?:"[^"]*"|[A-Z_]+), "([a-z-]+)"', shell))
        # f2-42: three fixed icons precede the real Desktop folder entries.
        # f4-07: Pictures is an optional shortcut after those entries.
        self.assertEqual(set(icons) - {"Pictures"}, {"Computer", "Documents", "Trash"})
        if "Pictures" in icons:
            self.assertEqual(icons["Pictures"], table["place:folder"])
            self.assertRegex(shell, r'if \(d->pictures_exists\) icon_add\(d, "Pictures", "files", "/Documents/Pictures"')
        for label, obj in {"Documents": "place:home", "Trash": "place:trash"}.items():
            self.assertEqual(icons[label], table[obj], label)
        self.assertIn('e->meta.type==FM_DIR ? "folder" : cat ? cat->tango : "text-x-generic"', shell)
        self.assertEqual(icons["Computer"], "ciuki-computer")    # identity: Tango computer with the approved portrait
        # The shell draws only store sizes: menus and the taskbar 16, dialogs and Alt+Tab 32, desktop icons 48.
        self.assertIn("#define SH_DESKTOP_ICON 48", shell)
        self.assertIn("m->icon = 16;", shell)
        calls = []
        for m in re.finditer(r"desk_shell_icon\(", shell):
            depth, at = 1, m.end()
            while depth:
                depth += {"(": 1, ")": -1}.get(shell[at], 0); at += 1
            args = shell[m.end():at - 1]
            if not args.startswith("const char"):                 # the definition
                calls.append((args[:args.rindex(",")].strip(), args[args.rindex(",") + 1:].strip()))
        # Only the identity portrait ("ciuki") is drawn at other sizes (Ciuki button 18, banner and badges 20).
        sizes = {size for name, size in calls if name != '"ciuki"'}
        self.assertEqual(sizes - {"16", "32", "SH_DESKTOP_ICON", "m->icon", "size"}, set(), calls)
        print(f"PASS shell/map coherence: {len(programs) + len(accessories)} program rows, {len(objects)} menu rows, desktop icons")

    def test_lookup_rules(self):
        probe = lambda name, size: self.harness("probe", name, size).splitlines()[0]
        self.assertTrue(probe("folder", 16).startswith("OK "))
        self.assertEqual(probe("places/folder", 22), probe("folder", 22))
        self.assertEqual(probe("actions/folder", 16), "NULL")
        self.assertEqual(probe("folder", 24), "NULL")
        self.assertEqual(probe("no-such-icon", 16), "NULL")
        self.assertTrue(probe("process-working-01", 48).startswith("OK "))
        src = (ROOT / "apps/libdeskui/icon_map.c").read_text()
        self.assertIn('{"type:*", "text-x-generic-template"}', src)

    def test_lazy_io_and_memory(self):
        """A directory lookup reads no pixel data; repeated icons do no I/O."""
        import struct
        out = self.harness("stats")
        rows = {r["phase"]: r for line in out.splitlines() if line.startswith("ICONS ")
                for r in [dict(item.split("=", 1) for item in line.split()[1:])]}
        directory_bytes = struct.unpack_from("<I", self.data, 36)[0]
        self.assertEqual(int(rows["directory"]["store_bytes"]), directory_bytes)
        off, length = self.image_span("folder", 32)
        self.assertGreater(off, directory_bytes)
        self.assertEqual(int(rows["folder"]["store_bytes"]), directory_bytes + length)
        self.assertEqual(int(rows["portrait"]["portrait_bytes"]), 262144)
        self.assertIn("PASS lazy cache", out)
        print(out.strip())

    def test_positional_read_boundaries(self):
        normal = self.harness("probe", "folder", 32)
        self.assertEqual(self.harness("probe", "folder", 32,
                                     env={"ICONS_IO_LIMIT": "17", "ICONS_IO_INTERRUPT": "1"}), normal)
        # Premature EOF in the directory refuses the store. EOF in pixel data
        # refuses that icon, while the valid directory remains available.
        self.assertEqual(self.harness("probe", "folder", 32,
                                      env={"ICONS_IO_EOF_AT": "48"}), "NULL\nCOUNT 0\n")
        off, _ = self.image_span("folder", 32)
        self.assertEqual(self.harness("probe", "folder", 32,
                                      env={"ICONS_IO_EOF_AT": str(off)}), "NULL\nCOUNT 246\n")

    def image_span(self, name, size):
        """(offset, length) of the stored pixels of name at size, from the header and tables."""
        import struct
        count, images = struct.unpack_from("<HH", self.data, 12)
        names_off, images_off, strings_off = struct.unpack_from("<III", self.data, 20)
        data_off = struct.unpack_from("<I", self.data, 36)[0]
        for k in range(count):
            n, _, *slots = struct.unpack_from("<II4H", self.data, names_off + 16 * k)
            end = self.data.index(b"\0", strings_off + n)
            if self.data[strings_off + n:end] == name.encode():
                _, encoding, _, off, length, _ = struct.unpack_from("<BBHIII", self.data, images_off + 16 * slots[SIZES.index(size)])
                self.assertEqual(encoding, 0)
                return data_off + off, length
        raise KeyError(name)

    def test_corrupt_store_rejected(self):
        bad = self.folder / "bad.cis"
        def probe(data, name="folder", size=16):
            bad.write_bytes(data)
            out = self.harness("probe", name, size, env={"ICONS_STORE": str(bad)}).split()
            return out[0], out[-1]
        self.assertEqual(probe(self.data), ("OK", "246"))
        self.assertEqual(probe(b"X" + self.data[1:]), ("NULL", "0"))          # magic
        self.assertEqual(probe(self.data[:-1]), ("NULL", "0"))                # truncated
        self.assertEqual(probe(self.data + b"\0"), ("NULL", "0"))             # trailing bytes
        header = bytearray(self.data)
        header[24] ^= 1                                                       # images offset
        self.assertEqual(probe(bytes(header)), ("NULL", "0"))
        off, length = self.image_span("folder", 32)
        damaged = bytearray(self.data)
        damaged[off + length // 2] ^= 0x10                                    # pixels of folder at 32 px
        damaged = bytes(damaged)
        self.assertEqual(probe(damaged, "folder", 32)[0], "NULL")            # FNV-1a check rejects it
        self.assertEqual(probe(damaged, "folder", 48)[0], "NULL")            # derived from the 32 px image
        self.assertEqual(probe(damaged, "folder", 16)[0], "OK")
        self.assertEqual(probe(damaged, "edit-copy", 32)[0], "OK")
        print("PASS corrupt stores: bad magic/layout/length rejected; a damaged image fails alone (checksum)")

    def test_licence_notice_kept(self):
        copying = (ROOT / "assets/icons/upstream/COPYING").read_bytes()
        self.assertEqual(hashlib.sha256(copying).hexdigest(), COPYING_SHA256)
        self.assertIn(b"Public Domain", copying)
        authors = (ROOT / "assets/icons/upstream/AUTHORS").read_text()
        for author in AUTHORS:
            self.assertIn(author, authors)
            self.assertIn(author, self.icons.NOTICE)
        readme = (ROOT / "assets/icons/README.md").read_text()
        self.assertIn(self.icons.ARCHIVE_SHA, readme)
        self.assertIn("Public Domain", readme)
        self.assertIn(STORE_SHA256, readme)
        self.assertIn("Public Domain", self.icons.NOTICE)
        self.assertIn(self.icons.ARCHIVE_SHA, self.icons.NOTICE)
        self.assertEqual(self.icons.parse_store(self.data)[""]["notice"], self.icons.NOTICE)
        self.assertEqual(self.harness("notice").strip(), self.icons.NOTICE)
        build = load("test_icons_build", ROOT / "apps/desktop/build_desktop.py")
        self.assertEqual(build.ICON_NOTICES["TANGO.TXT"], "upstream/COPYING")
        print("PASS licence: COPYING (Public Domain) and AUTHORS kept, notice in the store and shipped as /system/icons/TANGO.TXT")

    def test_contact_sheet(self):
        lines = [l.split() for l in self.harness("map").splitlines()]
        cells, groups = [], {}
        for obj, tango, *_ in lines:
            if obj.startswith("type:."):
                groups.setdefault(tango, []).append(obj[5:])
            elif not obj.endswith(":*"):
                cells.append((obj, obj))
        for tango, extensions in groups.items():
            cells.append(("type: " + " ".join(extensions), f"type:{extensions[0]}"))
        listing = self.folder / "sheet.txt"
        listing.write_text("".join(f"{label}\t{obj}\n" for label, obj in cells))
        ppm = self.folder / "contact-sheet-32.ppm"
        stdout = self.harness("sheet", listing, ppm)
        self.assertIn(f"SHEET cells={len(cells)} missing=0", stdout)
        target = ROOT / "build/host/icons/contact-sheet-32.png"
        write_png(ppm, target)
        print(f"PASS contact sheet: {len(cells)} mapped objects at 32 px -> {target}")


def write_png(ppm, target):
    """P6 PPM -> 8-bit RGB PNG (https://www.w3.org/TR/png/), standard library only."""
    import struct
    import zlib
    data = ppm.read_bytes()
    magic, width, height, depth = data.split(maxsplit=4)[:4]
    assert magic == b"P6" and depth == b"255"
    width, height = int(width), int(height)
    pixels = data[len(data) - width * height * 3:]
    raw = b"".join(b"\0" + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))
    chunk = lambda kind, body: struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                       chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


if __name__ == "__main__":
    unittest.main()
