"""Pinned DS-1 firmware provenance and deterministic import, without downloads."""
import hashlib
import json
from pathlib import Path
import re
import struct
import unittest

ROOT = Path(__file__).resolve().parents[2]
FIRMWARE = ROOT / 'third_party/yamaha'
HASHES = {
    'ds1_dsp.fw': ('DspInst', 128, '79ed987978fc99ba999c537f401584ca952af9e4d77367f7b1da8b034f06162b'),
    'ds1_ctrl.fw': ('CntrlInst', 12288, 'cd6dcf547cd70882e42b901dbc9940c5db9edcf078124092e2aec4d9320bf526'),
    'ds1e_ctrl.fw': ('CntrlInst1E', 12288, '399f62b9539ff4aba84e934752c80b57826ad16b9b606424f1aa986f42afe9e2'),
}


class YamahaFirmwareTests(unittest.TestCase):
    def test_microcode_images_are_exact_little_endian_donor_words(self):
        source = (FIRMWARE / 'ds1_microcode.h').read_text()
        manifest = json.loads((FIRMWARE / 'UPSTREAM.json').read_text())
        self.assertEqual(manifest['firmware_ref'], 'v1.2.4')
        self.assertEqual(manifest['upstream_git_blob'], '499a6669748fedbcf8c3995a7017424381eb6d9b')
        self.assertEqual(hashlib.sha256((FIRMWARE / 'ds1_microcode.h').read_bytes()).hexdigest(),
                         manifest['imported_sha256'])
        for filename, (symbol, size, expected) in HASHES.items():
            with self.subTest(image=filename):
                body = re.search(r'static const uint32_t\s+' + symbol + r'\[[^]]+\]\s*=\s*\{(.*?)\};',
                                 source, re.S).group(1)
                words = [int(word, 16) for word in re.findall(r'0x[\da-fA-F]+', body)]
                self.assertLessEqual(len(words), size // 4)
                words.extend([0] * (size // 4 - len(words)))
                data = struct.pack('<' + 'I' * len(words), *words)
                self.assertEqual(hashlib.sha256(data).hexdigest(), expected)
                self.assertEqual(manifest['images'][filename], {'symbol': symbol, 'bytes': size, 'sha256': expected})
                changed = bytearray(data)
                changed[-1] ^= 1
                self.assertNotEqual(hashlib.sha256(changed).hexdigest(), expected)

    def test_firmware_keeps_its_own_notice_and_credits(self):
        source = (FIRMWARE / 'ds1_microcode.h').read_text()
        self.assertIn('Copyright (c) 1997-1999\tYamaha Corporation.\tAll Rights Reserved.', source)
        notice = (FIRMWARE / 'NOTICE.txt').read_text()
        for name in ('ds1_microcode.h', 'ymfpci.c', 'ymfpci.h', 'ymfpci_main.c', 'v6.12', 'v1.2.4'):
            self.assertIn(name, notice)
        self.assertIn('no explicit redistribution grant', notice)
        self.assertIn('third_party/yamaha/NOTICE.txt', (ROOT / 'docs/credits.md').read_text())
        self.assertIn('third_party/yamaha/NOTICE.txt', (ROOT / 'scripts/build_image.py').read_text())


if __name__ == '__main__':
    unittest.main()
