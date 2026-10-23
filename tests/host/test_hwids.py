"""Host tests for the deterministic Windows INF hardware ID extraction."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("build_hwids", ROOT / "scripts/build_hwids.py")
hwids = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(hwids)


class HardwareIdTests(unittest.TestCase):
    def test_literal_quoted_token_mixed_decorated_models_only(self):
        text = r'''[Manufacturer]
%Maker%=Board,NTx86
[Board]
Literal Radeon=Install,PCI\VEN_1002&DEV_5144
"Quoted; Radeon"=Install,PCI\VEN_1002&DEV_514c ; comment
%Chip%=Install,PCI\VEN_1002&DEV_5159
"%Chip% mobile"=Install,PCI\VEN_1002&DEV_4c59
%missing%=Install,PCI\VEN_0001&DEV_0001
[Board.NTx86]
"A = B"=Install,USB\VID_0001&PID_0002,USB\VID_0001&PID_0003
[Strings.0409]
Chip="Wrong locale"
[Strings]
Maker="Vendor"
Chip="ATI Radeon"
registry="PCI\VEN_0001&DEV_9999"
[ControlFlags]
ExcludeFromSelect=PCI\VEN_0001&DEV_9998
[Install.AddReg]
HKR,,PCI\VEN_0001&DEV_9997
'''
        self.assertEqual(hwids.parse_inf(text), [
            ('pci', '1002:5144', 'Literal Radeon'),
            ('pci', '1002:514c', 'Quoted Radeon'),
            ('pci', '1002:5159', 'ATI Radeon'),
            ('pci', '1002:4c59', 'ATI Radeon mobile'),
            ('usb', '0001:0002', 'A B'), ('usb', '0001:0003', 'A B')])

    def test_literal_duplicate_winner_is_stable(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'z.inf').write_text('[Models]\nLong Widget=I,PCI\\VEN_1234&DEV_5678\n')
            (root / 'a.inf').write_text('[Models]\n"Beta"=I,PCI\\VEN_1234&DEV_5678\nalpha=I,PCI\\VEN_1234&DEV_5678\n')
            self.assertEqual(hwids.collect(root)[1], {'1234:5678': 'Beta'})
            (root / 'z.inf').write_text('[Models]\nZeta=I,PCI\\VEN_1234&DEV_5678\n')
            self.assertEqual(hwids.collect(root)[1], {'1234:5678': 'Beta'})

    def test_class_scope(self):
        model = '\n[Manufacturer]\nV=Board\n[Board.NTx86]\n%s=I,PCI\\VEN_0001&DEV_0002\n'
        for setup in hwids.INF_CLASSES:
            self.assertTrue(hwids.parse_inf('[Version]\nClass='+setup+model % 'Device', scoped=True))
        for setup in ('Printer', 'Unknown', 'System'):
            self.assertFalse(hwids.parse_inf('[Version]\nClass='+setup+model % 'Device', scoped=True))
        self.assertTrue(hwids.parse_inf('[Version]\nClass=System'+model % 'PCI bridge', scoped=True))
        for name in ('ATI SMBus', 'Accelerated Graphics Port', 'PCI-Express Root Port',
                     'Processor to I/O Controller', 'CPU to AGP Controller', 'LPC Interface'):
            self.assertTrue(hwids.parse_inf('[Version]\nClass=System'+model % name, scoped=True), name)
        for name in ('Memory Controller', 'I/O APIC Interrupt Controller', 'Timer'):
            self.assertFalse(hwids.parse_inf('[Version]\nClass=System'+model % name, scoped=True), name)

    def test_public_ids_hierarchy_precedence_and_scope(self):
        text = '1234  Vendor\n\t0001  Device\n\t\t0001 0002 Subsystem\n\t0002  Other\nC 03  Display\n\t00  VGA\nAT 0001 Something\n\t0003  Not a device\n'
        ids = hwids.parse_ids(text)
        self.assertEqual(ids, {'1234:0001': 'Vendor Device', '1234:0002': 'Vendor Other'})
        scope = {'pci': {'display': ['1234:0001', '1234:0002']}, 'usb': {'input': ['1234:0001']}}
        pci, usb = hwids.database_names({'1234:0001': 'INF wins'}, {}, ids, ids, scope)
        self.assertEqual(pci, {'1234:0001': 'INF wins', '1234:0002': 'Vendor Other'})
        self.assertEqual(usb, {'1234:0001': 'Vendor Device'})
        with self.assertRaises(ValueError):
            hwids.database_names({}, {}, ids, ids, {'pci': {'printer': []}, 'usb': {}})

    def test_snapshot_hash_failure_and_output_unchanged(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bad = root / 'pci.ids'; bad.write_text('untrusted')
            with self.assertRaisesRegex(ValueError, 'SHA-256 mismatch'):
                hwids.read_snapshot('pci', bad)
            out = root / 'out'; out.mkdir()
            (out / 'pci-ids.json').write_text('old output')
            with patch('sys.stderr'):
                self.assertEqual(hwids.main([str(root), '--pci-ids', str(bad), '--output-dir', str(out)]), 1)
            self.assertEqual((out / 'pci-ids.json').read_text(), 'old output')

    def test_generator_precedence_determinism_and_size_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'a.inf').write_text('[Version]\nClass=Display\n[Models]\nOwner literal=I,PCI\\VEN_1002&DEV_5144\n')
            out = root / 'output'
            args = [str(root), '--output-dir', str(out)]
            public = {'1002:5144': 'Public loses', '1002:4c4d': 'Public fills gap'}
            with patch.object(hwids, 'read_snapshot', side_effect=lambda kind, path: public if kind == 'pci' else {}), patch('builtins.print'):
                self.assertEqual(hwids.main(args), 0)
                pci = json.loads((out / 'pci-ids.json').read_text())
                self.assertEqual(pci['1002:5144'], 'Owner literal')
                self.assertEqual(pci['1002:4c4d'], 'Public fills gap')
                self.assertIn('8086:00d1', pci)
                first = [(out / name).read_bytes() for name in ('pci-ids.json', 'usb-ids.json')]
                self.assertEqual(hwids.main(args), 0)
                self.assertEqual(first, [(out / name).read_bytes() for name in ('pci-ids.json', 'usb-ids.json')])
                with patch.object(hwids, 'MAX_JSON', 1):
                    self.assertEqual(hwids.main(args), 1)
                self.assertEqual(first, [(out / name).read_bytes() for name in ('pci-ids.json', 'usb-ids.json')])

    def test_scoped_system_inf_label_beats_public_and_malformed_ids(self):
        text = r'''[Version]
Class=System
[Models]
INF USB controller=I,PCI\VEN_8086&DEV_2482
Malformed=I,PCI\VEN_1234&DEV_12345,USB\VID_1234&PID_1234z
'''
        self.assertEqual(hwids.parse_inf(text, scoped=True, allowed={'pci': {'8086:2482'}}),
                         [('pci', '8086:2482', 'INF USB controller')])

    def test_requested_display_families_and_complete_scope(self):
        pci = json.loads((ROOT / 'assets/hwids/pci-ids.json').read_text())
        families = {
            'ATI Radeon': '1002:5144 1002:4c59 1002:514c 1002:4e44 1002:4a49 1002:5549',
            'ATI Rage': '1002:4742 1002:4c4d 1002:5245 1002:5046',
            'Matrox': '102b:0520 102b:0521 102b:0525 102b:2527',
            'Intel': '8086:7800 8086:00d1 8086:7121 8086:7123 8086:7125 8086:1132 8086:3577 8086:2562 8086:3582 8086:2572 8086:2582',
            '3dfx': '121a:0005 121a:0009',
            'S3': '5333:8811 5333:8901 5333:5631 5333:8a01 5333:8a22 5333:8a25 5333:8d03 5333:8c2e',
            'SiS': '1039:0300 1039:0315 1039:0330 1039:6325',
            'Trident': '1023:9880 1023:8400 1023:8500 1023:8820',
            'Cirrus': '1013:00a0 1013:00a4 1013:00ac 1013:00b8',
            'NeoMagic': '10c8:0001 10c8:0002 10c8:0003 10c8:0083 10c8:0004 10c8:0005 10c8:0025 10c8:0006 10c8:0016',
            'VIA': '1106:3122 1106:7205 1106:3108 1106:3118',
            'C&T corrected IDs': '102c:00c0 102c:00d8 102c:00e0 102c:00e4 102c:00e5',
        }
        for family, keys in families.items():
            for key in keys.split():
                with self.subTest(family=family, key=key):
                    self.assertTrue(pci.get(key))
        scope = json.loads((ROOT / 'assets/hwids/retro-scope.json').read_text())
        for kind in ('pci', 'usb'):
            published = json.loads((ROOT / f'assets/hwids/{kind}-ids.json').read_text())
            for category, keys in scope[kind].items():
                self.assertIn(category, hwids.CLASSES)
                for key in keys:
                    self.assertTrue(published.get(key), key)
        for incorrect in ('102c:00c4', '102c:00c5'):
            self.assertNotIn(incorrect, pci)

    def test_sections_strings_subsys_duplicates_and_malformed(self):
        inf = r'''[Models]
%long%=Install, PCI\VEN_1234&DEV_ABCD&SUBSYS_00000000
%short%=Install, PCI\VEN_1234&DEV_ABCD&SUBSYS_00000001
%usb%=Install, USB\VID_0ABC&PID_0123
not a valid model row
%missing%=Install, PCI\VEN_1234&DEV_NOPE
[Strings]
long="A Long PCI Device Name"
short="Tiny"
usb="USB Widget"
'''
        found = hwids.parse_inf(inf)
        self.assertEqual(found, [('pci', '1234:abcd', 'A Long PCI Device Name'),
                                 ('pci', '1234:abcd', 'Tiny'), ('usb', '0abc:0123', 'USB Widget')])

    def test_encoding_and_ascii_bound(self):
        text = '[Models]\n%device%=Install, PCI\\VEN_0001&DEV_0002\n[Strings]\ndevice="Café ' + ('Device ' * 12) + 'Name"\n'
        rows = hwids.parse_inf(text)
        self.assertEqual(len(rows[0][2]), 64)
        self.assertTrue(rows[0][2].isascii())
        with tempfile.TemporaryDirectory() as temp:
            inf = Path(temp) / 'utf16.inf'
            inf.write_text(text, encoding='utf-16')
            self.assertEqual(hwids.decode_inf(inf), text)
            inf.write_bytes(text.encode('utf-16-le'))
            self.assertEqual(hwids.decode_inf(inf), text)
            literal = '[Models]\n"Literal GPU"=I,PCI\\VEN_1002&DEV_5144\n'
            for encoding in ('utf-16', 'utf-16-le', 'utf-16-be'):
                inf.write_bytes(literal.encode(encoding))
                self.assertEqual(hwids.parse_inf(hwids.decode_inf(inf)), [('pci', '1002:5144', 'Literal GPU')])

    def test_output_determinism_and_bound(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / 'infs'; root.mkdir()
            (root / 'b.INF').write_text('[Models]\n%desc%=Install, USB\\VID_0001&PID_0002\n[Strings]\ndesc="Long Widget"\n')
            (root / 'a.inf').write_text('[Models]\n%desc%=Install, USB\\VID_0001&PID_0002\n[Strings]\ndesc="USB"\n')
            files1, pci1, usb1 = hwids.collect(root)
            first = hwids.encoded(pci1), hwids.encoded(usb1)
            files2, pci2, usb2 = hwids.collect(root)
            second = hwids.encoded(pci2), hwids.encoded(usb2)
            self.assertEqual(len(files1), 2)
            self.assertEqual(first, second)
            self.assertEqual(usb1, {'0001:0002': 'USB'})
            self.assertLessEqual(sum(map(len, first)), hwids.MAX_JSON)
        for path in (ROOT / 'assets/hwids/pci-ids.json', ROOT / 'assets/hwids/usb-ids.json'):
            data = path.read_bytes()
            self.assertTrue(data.isascii())
            self.assertIsInstance(json.loads(data), dict)
        self.assertLessEqual(sum((ROOT / 'assets/hwids' / f).stat().st_size for f in ('pci-ids.json', 'usb-ids.json')), hwids.MAX_JSON)

    def test_image_payload_mapping(self):
        builder = (ROOT / 'scripts/build_image.py').read_text()
        self.assertIn('sources["/System/hwids/pci-ids.json"]', builder)
        self.assertIn('sources["/System/hwids/usb-ids.json"]', builder)
        self.assertIn('"/System/hwids"', builder)
        # integ-periph (4d5472d8) merged the hwids pair and the cmipci pair into one notice loop.
        self.assertIn('for name in ("hwids-NOTICES.txt", "hwids-GPL-2.0.txt",', builder)
        self.assertIn('sources[f"/system/licenses/{name}"] = ROOT / "docs/licenses" / name', builder)

    def test_native_nic_labels_and_inf_precedence(self):
        result = hwids.native_names({'1011:0019': 'Owner INF label'})
        self.assertEqual(result['1011:0019'], 'Owner INF label')
        self.assertEqual(result['1106:3065'], 'VIA VT6102 Rhine II Fast Ethernet Controller')
        published = json.loads((ROOT / 'assets/hwids/pci-ids.json').read_text())
        for key in ('1011:0009', '1011:0019', '1106:3043', '1106:3065', '1106:3106', '8086:1017'):
            self.assertIn(key, published)


if __name__ == '__main__':
    unittest.main()
