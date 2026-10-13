"""Host tests for the deterministic Windows INF hardware ID extraction."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("build_hwids", ROOT / "scripts/build_hwids.py")
hwids = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(hwids)


class HardwareIdTests(unittest.TestCase):
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


if __name__ == '__main__':
    unittest.main()
