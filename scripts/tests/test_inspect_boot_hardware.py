import importlib.util
import struct
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / 'inspect_boot_hardware.py'
spec = importlib.util.spec_from_file_location('inspect_boot_hardware', SCRIPT)
inspect = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inspect)


def sample():
    data = bytearray(inspect.TOTAL_SIZE)
    data[:4] = b'CVB1'
    struct.pack_into('<HH', data, 4, 1024, 768)
    data[8:12] = bytes((1, 32, 1, 3))
    struct.pack_into('<HHHHH', data, 12, 0x118, 4096, 0, 2, 0x118)
    data[22:24] = bytes((1, 2))
    struct.pack_into('<IHH', data, 24, 9, 0x004F, 0x4118)
    mode = 32
    data[mode] = 0x9B
    data[mode + 2] = 5
    struct.pack_into('<HHHH', data, mode + 4, 4, 64, 0xA000, 4096)
    struct.pack_into('<HH', data, mode + 18, 1024, 768)
    data[mode + 25:mode + 28] = bytes((32, 0, 6))
    data[mode + 31:mode + 39] = bytes((8, 16, 8, 8, 8, 0, 8, 24))
    struct.pack_into('<H', data, mode + 50, 4096)
    data[mode + 54:mode + 62] = bytes((8, 16, 8, 8, 8, 0, 8, 24))
    ctrl = 32 + 256
    data[ctrl:ctrl + 4] = b'VESA'
    struct.pack_into('<H', data, ctrl + 4, 0x0300)
    edid = len(data) - 128
    data[edid:edid + 8] = bytes.fromhex('00ffffffffffff00')
    data[edid + 18:edid + 20] = bytes((1, 4))
    data[edid + 24] = 2
    dt = edid + 54
    # EDID DTD active dimensions: 1024x768 in the preferred timing.
    data[dt:dt + 2] = bytes((1, 0))
    data[dt + 4] = 0x40
    data[dt + 7] = 0x30
    data[edid + 127] = (-sum(data[edid:edid + 127])) & 0xFF
    return data


class InspectBootHardwareTests(unittest.TestCase):
    def test_rejects_truncated_and_wrong_magic(self):
        with self.assertRaisesRegex(ValueError, 'expected exactly 928'):
            inspect.parse_log(bytes(inspect.TOTAL_SIZE - 1))
        bad = sample()
        bad[:4] = b'NOPE'
        with self.assertRaisesRegex(ValueError, 'bad magic'):
            inspect.parse_log(bad)

    def test_schema_transport_and_mode_readback(self):
        result = inspect.parse_log(sample())
        self.assertEqual(result['size_bytes'], 928)
        self.assertEqual(result['transport_flags']['raw'], 3)
        self.assertTrue(result['transport_flags']['bank_window_path_entered'])
        self.assertTrue(result['transport_flags']['local_lfb_path_entered'])
        self.assertIn('entered bank-window path', result['transport_evidence'][0])
        self.assertIn('9 protected session row transfers completed', result['transport_evidence'])
        self.assertTrue(result['checks']['vbe_mode_readback_matches'])
        self.assertTrue(result['checks']['full_color_direct_mode'])
        self.assertTrue(result['checks']['rgb_masks_valid'])
        self.assertTrue(result['checks']['usable_banked_window'])
        self.assertTrue(result['checks']['vbe_lfb_readback_matches_header'])
        self.assertTrue(result['checks']['edid_preferred_timing_valid'])
        self.assertEqual(result['edid']['preferred_width'], 1024)
        self.assertEqual(result['edid']['preferred_height'], 768)

    def test_edid_all_zero_and_overlapping_masks_are_not_valid(self):
        data = sample()
        data[-128:] = bytes(128)
        mode = 32
        data[mode + 33] = 20  # green overlaps red's 16..23 field
        result = inspect.parse_log(data)
        self.assertTrue(result['checks']['edid_checksum_valid'])
        self.assertFalse(result['checks']['edid_header_valid'])
        self.assertFalse(result['checks']['edid_preferred_timing_valid'])
        self.assertIsNone(result['edid']['preferred_width'])
        self.assertFalse(result['checks']['rgb_masks_valid'])

    def test_vga_log_marks_modeinfo_inactive(self):
        data = sample()
        data[8] = 0
        result = inspect.parse_log(data)
        self.assertIn('raw VBE ModeInfo is inactive', result['renderer']['active_path'])
        self.assertFalse(result['checks']['full_color_direct_mode'])


if __name__ == '__main__':
    unittest.main()
