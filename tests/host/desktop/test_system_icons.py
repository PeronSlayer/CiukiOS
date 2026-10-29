"""Inventory, provenance and production reader coverage for fx-icons (host only)."""
# SPDX-License-Identifier: MIT
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from . import icon_assets
from . import test_icons as baseline

ROOT = Path(__file__).resolve().parents[3]


class SystemIconTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.system = icon_assets.load('test_system_icons', ROOT / 'apps/desktop/system_icons.py')
        cls.files = cls.system.payloads()
        cls.manifest = json.loads(cls.files['MANIFEST.JSON'])
        cls.rows = json.loads(cls.files['INVENTORY.JSON'])
        cls.c = cls.system.converter()
        cls.parsed = cls.c.parse_store(cls.files['tango.cis'])
        (ROOT / 'build/host').mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='system-icons-', dir=ROOT / 'build/host')
        cls.folder = Path(cls.temp.name)
        cls.store = cls.folder / 'tango.cis'
        cls.store.write_bytes(cls.files['tango.cis'])
        # Use the repository's existing ASan/UBSan icon harness and its environment
        # unchanged. This sandbox cannot run LeakSanitizer's ptrace phase.
        baseline.IconStoreTests.setUpClass()
        cls.reader = baseline.IconStoreTests()

    @classmethod
    def tearDownClass(cls):
        baseline.IconStoreTests.tearDownClass()
        cls.temp.cleanup()

    def test_every_inventory_entry_has_three_staged_sizes(self):
        names = {row['name']: row for row in self.manifest['icons']}
        for row in self.rows:
            targets = [f'process-working-{i:02}' for i in range(32)] if row['icon'] == 'process-working-*' else [row['icon']]
            for name in targets:
                for size in (16, 32, 48):
                    entry = names[name]['png'][str(size)]
                    raw = self.files[entry['path']]
                    self.assertEqual(self.system.sha(raw), entry['sha256'])
                    w, h, rgba = self.c.png_rgba(raw)
                    self.assertEqual((w, h), (size, size), row['object'])
                    if name in self.parsed:
                        self.assertEqual(rgba, self.parsed[name][size], row['object'])
        # sec-user-04: Users and Passwords are one User Accounts applet.
        self.assertEqual(len([r for r in self.rows if r['object'].startswith('applet:')]), 18)
        self.assertEqual(self.rows, self.system.inventory(self.system.sources()))

    def test_all_staged_icons_have_allowed_provenance(self):
        covered = set()
        for row in self.manifest['icons']:
            self.assertIn(row['license'], {'Public Domain', 'MIT', 'Project asset (no new license granted)'})
            if row['license'].startswith('Project'):
                self.assertIn(row['name'], {'ciuki', 'ciuki-computer'})
            self.assertTrue(row['sources'])
            covered.update(p['path'] for p in row['png'].values())
        self.assertEqual(covered, {p for p in self.files if p.endswith('.png')})
        self.assertEqual(set(self.parsed) - {''}, {r['name'] for r in self.manifest['icons']} - {'ciuki', 'ciuki-computer'})
        self.assertEqual(self.manifest['store_sha256'], self.system.sha(self.files['tango.cis']))
        self.system.check_store_record()
        self.assertEqual(self.system.payloads(), self.files)
        for name, source in self.system.NOTICES.items():
            self.assertTrue((ROOT / 'assets/icons' / source).is_file(), name)

    def test_exact_identity_and_tango_pixels_preserved(self):
        portrait, rgba = self.system.portrait_pixels(self.c)
        for name in ('ciuki', 'ciuki-computer'):
            row = next(r for r in self.manifest['icons'] if r['name'] == name)
            self.assertEqual(row['source_sha256'], portrait.SOURCE_SHA256)
            self.assertEqual(row['conversion_sha256'], portrait.ARRAY_SHA256)
        for size in (16, 32, 48):
            self.assertEqual(self.c.png_rgba(self.files[f'ciuki-{size}.png'])[2], self.c.area_scale(rgba, 256, size))
        original = self.c.parse_store(self.c.build_store()[0])
        for name in set(original) - {''}:
            for size in (16, 22, 32, 48):
                self.assertEqual(original[name][size], self.parsed[name][size], (name, size))
        self.assertEqual(self.system.sha((ROOT / 'assets/brand/ciuki-logo.png').read_bytes()), portrait.SOURCE_SHA256)

    def test_c_reader_resolves_map_and_matches_all_store_pixels(self):
        env = {'ICONS_STORE': str(self.store)}
        result = self.reader.harness('map', env=env)
        for line in result.splitlines():
            self.assertEqual(line.split()[2:], ['16', '22', '32', '48'], line)
        decoded = self.folder / 'decoded.bin'
        self.reader.harness('dump', decoded, env=env)
        stream = decoded.read_bytes()
        expected = b''.join(name.encode() + b'\0' + bytes([size]) + self.parsed[name][size]
                            for name in sorted(set(self.parsed) - {''}) for size in (16, 22, 32, 48))
        self.assertEqual(stream[:len(expected)], expected)

    def test_image_builder_stages_every_icon_and_notice(self):
        build = icon_assets.load('test_system_icon_build', ROOT / 'apps/desktop/build_desktop.py')
        self.assertEqual(set(build.ICON_FILES), set(self.files) | set(build.ICON_NOTICES))
        image = icon_assets.load('test_system_icon_image', ROOT / 'scripts/build_image.py')
        # Exercise the production image file mapping without building any image,
        # ELF or unrelated fonts/wallpaper/sound resources.
        fake = type('Desktop', (), {'ICON_FILES': build.ICON_FILES,
                    'validate_payloads': staticmethod(lambda: dict(protocol_version=1, elf={}, portrait={}, icons={}))})()
        spec = type('Spec', (), {'loader': type('Loader', (), {'exec_module': staticmethod(lambda _: None)})()})()
        with patch.object(image.importlib.util, 'spec_from_file_location', return_value=spec), \
             patch.object(image.importlib.util, 'module_from_spec', return_value=fake), \
             patch.object(image, 'desktop_license_payloads', return_value={}), \
             patch.object(image, 'font_payloads', return_value=({}, {})), \
             patch.object(image, 'wallpaper_cache_payloads', return_value=({}, {})), \
             patch.object(image, 'wallpaper_thumbnail_payloads', return_value=({}, {})), \
             patch.object(image, 'sound_payloads', return_value=({}, {})), \
             patch.object(image, 'firmware_payloads', return_value={}), \
             patch.object(image, 'sha256', return_value='fixture'):
            sources, directories, _ = image.desktop_payloads()
        staged = {path[len('/system/icons/'):]: source for path, source in sources.items() if path.startswith('/system/icons/')}
        self.assertEqual(set(staged), set(build.ICON_FILES))
        for name, source in staged.items():
            self.assertEqual(source, image.DESKTOP / name)
        self.assertIn('/system/icons', directories)

    def test_changed_sources_and_stale_inventory_rejected(self):
        with patch.object(self.system, 'HAIKU_SHA', '0' * 64):
            with self.assertRaisesRegex(ValueError, 'archive changed'):
                self.system.sources()
        bad = self.folder / 'inventory.json'
        bad.write_text('[]')
        with patch.object(self.system, 'INVENTORY', bad):
            with self.assertRaisesRegex(ValueError, 'inventory stale'):
                self.system.payloads()
        with self.assertRaisesRegex(ValueError, 'differs'):
            self.system.check_store(self.files['tango.cis'][:-1])


if __name__ == '__main__':
    unittest.main()
