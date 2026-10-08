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


def display_info(backend=3, status=True):
    data = bytearray(inspect.DISPLAY_INFO_SIZE)
    data[:4] = b'CVGD'
    struct.pack_into('<HH', data, 4, 0x0100, inspect.DISPLAY_INFO_SIZE)
    struct.pack_into('<IIIIIIIIIIII', data, 8, backend, 1, 1024, 768, 4096, 32,
                     0, 32, 1, 0xE0000000, 4 * 1024 * 1024, 0)
    if status:
        struct.pack_into('<I', data, 60, 0x80000007)
        # ready, capabilities, device, mapped addresses, memory and engine tests
        values = [1, 7, 0, 0x8C2, 0xF0000000, 0xE0000000, 0xE0000000,
                  4 * 1024 * 1024, 0, 65536, 4096, 32, 2, 1, 0, 0, 0, 1,
                  1, 2, 1024, 768, 8, 1, 1, 0, 0, 0, 1024, 768, 1, 3]
        struct.pack_into('<32I', data, 64, *values)
    return data


def cg3d(result=0, before=None, after=None):
    data = bytearray(inspect.GPU_LOG_SIZE)
    data[:4] = b'CG3D'
    struct.pack_into('<HHII', data, 4, 0x0100, 16, result, 0x1234)
    if before is not None:
        data[16:208] = before
    if after is not None:
        data[208:400] = after
    return data


def cvt1(result=0):
    data = bytearray(inspect.TRACE_RECORD_SIZE)
    data[:4] = b'CVT1'
    struct.pack_into('<HBBHBBH', data, 4, 0x118, 2, result, 0x004F, 1, 3, 0x0100)
    mode = 16
    data[mode] = 0x99
    data[mode + 24] = 1
    struct.pack_into('<HH', data, mode + 18, 1024, 768)
    data[mode + 25] = 32
    data[mode + 27] = 6
    struct.pack_into('<H', data, mode + 16, 2048)
    struct.pack_into('<H', data, mode + 50, 4096)
    struct.pack_into('<I', data, mode + 40, 0xE0000000)
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

    def test_cvgd_snapshot_qualifies_only_actual_owned_tested_backend(self):
        result = inspect.parse_display_info(display_info())
        self.assertTrue(result['s3_engine_qualified'])
        self.assertTrue(result['s3_triangles_qualified'])
        self.assertEqual(result['s3_status']['copy_selftests'], 3)
        result = inspect.parse_display_info(display_info(backend=0))
        self.assertFalse(result['s3_engine_qualified'])

    def test_cvgd_decodes_preflight_without_changing_legacy_status(self):
        data = display_info(backend=0)
        struct.pack_into('<I', data, 64 + 8, 1 | 0x10000)
        struct.pack_into('<I', data, 64 + 17 * 4, 0xE800000C)
        result = inspect.parse_display_info(data)
        self.assertEqual(result['s3_status']['error_stage'], 0x10001)
        self.assertEqual(result['s3_status']['stage'], 1)
        self.assertEqual(result['s3_status']['preflight'], {
            'reason_code': 0x10000, 'reason': 'framebuffer_bar',
            'raw_bar1': 0xE800000C})
        self.assertFalse(result['s3_engine_qualified'])
        struct.pack_into('<I', data, 64 + 8, 1)
        self.assertIsNone(inspect.parse_display_info(data)['s3_status']['preflight'])
        struct.pack_into('<I', data, 64 + 8, 8 | 0x10000)
        self.assertIsNone(inspect.parse_display_info(data)['s3_status']['preflight'])

    def test_cvgd_separates_2d_from_triangle_qualification_and_uses_latest_probe(self):
        data = display_info()
        struct.pack_into('<I', data, 68, 1)  # fill capability only
        result = inspect.parse_display_info(data)
        self.assertTrue(result['s3_engine_qualified'])
        self.assertFalse(result['s3_triangles_qualified'])

        data = display_info()
        struct.pack_into('<I', data, 64 + 25 * 4, 4)  # historical probe failures
        result = inspect.parse_display_info(data)
        self.assertTrue(result['s3_engine_qualified'])
        self.assertTrue(result['s3_triangles_qualified'])

    def test_cvgd_rejects_bad_size_abi_and_reserved_flags(self):
        data = display_info()
        with self.assertRaisesRegex(ValueError, 'expected exactly 192'):
            inspect.parse_display_info(data[:-1])
        bad = bytearray(data)
        struct.pack_into('<H', bad, 4, 0x0200)
        with self.assertRaisesRegex(ValueError, 'ABI version'):
            inspect.parse_display_info(bad)
        bad = bytearray(data)
        struct.pack_into('<I', bad, 60, 0x80000008)
        with self.assertRaisesRegex(ValueError, 'reserved CVGD flags'):
            inspect.parse_display_info(bad)

    def test_cg3d_log_parses_and_allows_zero_snapshots_only_for_errors(self):
        complete = inspect.parse_gpu_log(cg3d(before=display_info(), after=display_info()))
        self.assertEqual(complete['result_name'], 'completed')
        self.assertTrue(complete['after']['data']['s3_engine_qualified'])
        unavailable = inspect.parse_gpu_log(cg3d(result=1))
        self.assertFalse(unavailable['before']['available'])
        with self.assertRaisesRegex(ValueError, 'zero before.*completed'):
            inspect.parse_gpu_log(cg3d())

    def test_cg3d_rejects_truncated_bad_header_and_nonzero_bad_snapshot(self):
        with self.assertRaisesRegex(ValueError, 'expected exactly'):
            inspect.parse_gpu_log(bytes(inspect.GPU_LOG_SIZE - 1))
        bad = cg3d(result=1)
        bad[:4] = b'NOPE'
        with self.assertRaisesRegex(ValueError, 'bad CG3D magic'):
            inspect.parse_gpu_log(bad)
        bad = cg3d(result=1)
        struct.pack_into('<H', bad, 6, 15)
        with self.assertRaisesRegex(ValueError, 'header size'):
            inspect.parse_gpu_log(bad)
        bad = cg3d(result=1, before=display_info())
        bad[16] ^= 1
        with self.assertRaisesRegex(ValueError, 'bad CVGD magic'):
            inspect.parse_gpu_log(bad)

    def test_cvt1_trace_fields_limits_and_malformed_records(self):
        record = cvt1(result=1)
        parsed = inspect.parse_trace(record)
        self.assertEqual(parsed['record_count'], 1)
        self.assertEqual(parsed['records'][0]['mode_info']['physical_base'], 0xE0000000)
        self.assertEqual(parsed['records'][0]['mode_info']['attributes'], 0x0099)
        self.assertEqual(parsed['records'][0]['mode_info']['banked_pitch'], 2048)
        self.assertEqual(parsed['records'][0]['mode_info']['linear_pitch'], 4096)
        self.assertEqual(parsed['records'][0]['framebuffer_path'], 'local')
        self.assertEqual(parsed['records'][0]['failed_internal_status'], 0x0100)
        self.assertIsNone(inspect.parse_trace(cvt1())['records'][0]['failed_internal_status'])
        protected = bytearray(cvt1())
        protected[10] = 2
        self.assertEqual(inspect.parse_trace(protected)['records'][0]['framebuffer_path'], 'protected')
        with self.assertRaisesRegex(ValueError, 'multiple of 272'):
            inspect.parse_trace(record[:-1])
        with self.assertRaisesRegex(ValueError, 'maximum is 128'):
            inspect.parse_trace(bytes(inspect.TRACE_RECORD_SIZE * 129))
        bad = bytearray(record)
        bad[:4] = b'NOPE'
        with self.assertRaisesRegex(ValueError, 'bad CVT1 magic'):
            inspect.parse_trace(bad)
        bad = bytearray(record)
        struct.pack_into('<H', bad, 14, 1)
        with self.assertRaisesRegex(ValueError, 'reserved field'):
            inspect.parse_trace(bad)
        bad = bytearray(record)
        bad[7] = 2
        with self.assertRaisesRegex(ValueError, 'result 2'):
            inspect.parse_trace(bad)
        bad = bytearray(record)
        bad[10] = 3
        with self.assertRaisesRegex(ValueError, 'LFB mode 3'):
            inspect.parse_trace(bad)


if __name__ == '__main__':
    unittest.main()
