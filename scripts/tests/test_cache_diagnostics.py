import importlib.util
from pathlib import Path
import struct
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / 'inspect_boot_hardware.py'
spec = importlib.util.spec_from_file_location('inspect_boot_hardware_cache_test', SCRIPT)
inspector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inspector)


def fixture():
    packet = bytearray(inspector.CACHE_DIAG_SIZE)
    struct.pack_into('<4sHHII', packet, 0, b'CVFD', 0x0100,
                     inspector.CACHE_DIAG_SIZE, inspector.CACHE_RECORD_SIZE,
                     inspector.CACHE_TIME_SIZE)
    cache = inspector.CACHE_DIAG_HEADER_SIZE
    packet[cache:cache + 8] = b'CVFBCACH'
    struct.pack_into('<8I', packet, cache + 8, 576, 1, 0xE8000000, 0x01000000,
                     0xE8000003, 0x178BFBFF, 0x80050033, 0x00000600)
    struct.pack_into('<QQQ', packet, cache + 40, 0x0007040600070406,
                     0x508, 0xC06)
    struct.pack_into('<QQ', packet, cache + 64, 0xE8000001, 0xFFF800800)
    timing = cache + inspector.CACHE_RECORD_SIZE
    packet[timing:timing + 8] = b'CVFBTIME'
    struct.pack_into('<6I', packet, timing + 8,
                     210, 0x89ABCDEF, 1, 0x12345678, 0x01020304, 0)
    return packet


class CacheDiagnosticsTests(unittest.TestCase):
    def test_decodes_physical_snapshot_and_copy_counters(self):
        result = inspector.parse_cache_diagnostics(fixture())
        cache = result['cache_record']
        timing = result['copy_timing']
        self.assertEqual(cache['live'], 1)
        self.assertEqual(cache['physical_address'], 0xE8000000)
        self.assertEqual(cache['extent_bytes'], 0x01000000)
        self.assertEqual(cache['mtrrcap'], 0x508)
        self.assertEqual(cache['variable_range_count'], 8)
        self.assertEqual(cache['variable_ranges'][0]['type'], 1)
        self.assertTrue(cache['variable_ranges'][0]['enabled'])
        self.assertEqual(timing['calls'], 210)
        self.assertEqual(timing['cycles'], 0x0000000189ABCDEF)
        self.assertEqual(timing['max_cycles'], 0x12345678)

    def test_rejects_wrong_size_magic_and_embedded_record_size(self):
        good = fixture()
        for data in (good[:-1], b'NOPE' + good[4:]):
            with self.assertRaises(ValueError):
                inspector.parse_cache_diagnostics(data)
        bad_size = bytearray(good)
        struct.pack_into('<I', bad_size, 8, 575)
        with self.assertRaisesRegex(ValueError, 'embedded record sizes'):
            inspector.parse_cache_diagnostics(bad_size)

    def test_rejects_impossible_mtrr_count(self):
        bad = fixture()
        cache = inspector.CACHE_DIAG_HEADER_SIZE
        struct.pack_into('<Q', bad, cache + 48, 33)
        with self.assertRaisesRegex(ValueError, 'variable MTRRs'):
            inspector.parse_cache_diagnostics(bad)


if __name__ == '__main__':
    unittest.main()
