"""Production parser checked against source-authored metadata/refusal oracles."""
# SPDX-License-Identifier: MIT
import ctypes as C
import hashlib
import json
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from scripts.win16 import fixtures, census, native


def subset(test, got, expected):
    if isinstance(expected, dict):
        for key, value in expected.items():
            test.assertIn(key, got)
            subset(test, got[key], value)
    elif isinstance(expected, list):
        test.assertEqual(len(got), len(expected))
        for actual, wanted in zip(got, expected):
            subset(test, actual, wanted)
    else:
        test.assertEqual(got, expected)


class ParserTests(unittest.TestCase):
    def test_every_fixture_has_exact_parse_policy_and_metadata(self):
        for spec in fixtures.manifests():
            with self.subTest(fixture=spec['id']):
                data = fixtures.synthetic(spec)
                expected = spec['expect']
                if expected['parse'] != 'ok':
                    with self.assertRaises(native.NEError) as refused:
                        census.inspect(data)
                    self.assertEqual(str(refused.exception), expected['parse'])
                    continue
                actual = census.inspect(data)
                self.assertEqual(actual['load_policy'], expected['load'])
                for key in ('imports', 'exports', 'segments', 'resources', 'startup', 'alignment',
                            'alignment_stored', 'dll', 'module_flags', 'expected_version', 'selfload'):
                    if key in expected:
                        subset(self, actual[key], expected[key])
                for patch in expected.get('loaded', []):
                    p = native.Parsed(data)
                    size = p.image.segments[patch['segment']-1].allocation
                    buf = (native.U8 * (size+2))(*([0xa5]*(size+2)))
                    ptr = C.cast(C.byref(buf, 1), C.POINTER(native.U8))
                    error = native.Error()
                    result = p.lib.ne_load_segment(C.byref(p.image), patch['segment'],
                                                   native.Buffer(ptr, size, 0x87), C.byref(error))
                    self.assertEqual(result, 0)
                    off = 1+patch['offset']
                    wanted = bytes.fromhex(patch['hex'])
                    self.assertEqual(bytes(buf)[off:off+len(wanted)], wanted)
                    self.assertEqual((buf[0], buf[-1]), (0xa5, 0xa5))

    def test_fixture_hashes_are_pinned_and_all_models_are_distinct(self):
        hashes = {s['id']+'.ne': hashlib.sha256(fixtures.synthetic(s)).hexdigest()
                  for s in fixtures.manifests()}
        self.assertEqual(hashes, json.loads((fixtures.SOURCES/'hashes.json').read_text()))
        self.assertEqual(len(hashes), len(set(hashes.values())))
        self.assertGreaterEqual(len(hashes), 40)

    def test_truncation_sweep_never_returns_a_valid_partial_plan(self):
        data = fixtures.synthetic(fixtures.manifests()[0])
        lib = native.library()
        for size in range(len(data)):
            backing = (native.U8 * size).from_buffer_copy(data[:size])
            image, error = native.Image(), native.Error()
            result = lib.ne_parse(backing, size, C.byref(image), C.byref(error))
            # Last nonresident padding can be omitted only if header length permits;
            # this fixture explicitly binds the complete table length.
            self.assertNotEqual(result, 0, size)
            self.assertEqual(image.valid, 0, size)
            self.assertNotEqual(lib.ne_rule_name(error.rule), b'unknown-rule')

    def test_empty_optional_resource_table_and_early_entry_table(self):
        data = bytearray(fixtures.synthetic(fixtures.manifests()[0]))
        h = 0x80
        # Primary NE tables can put entries before the segment table. Move the
        # six-byte bundle to DOS-stub slack and use h=0x70 to make it header-relative.
        # Instead extend the metadata padding at the end: put entries at h+0x200,
        # then the segment table at h+0x210. Both are before payload at 0x400.
        ent = h+int.from_bytes(data[h+4:h+6], 'little')
        seg = h+int.from_bytes(data[h+34:h+36], 'little')
        data[h+0x200:h+0x206] = data[ent:ent+6]
        data[h+0x210:h+0x220] = data[seg:seg+16]
        struct.pack_into('<H', data, h+4, 0x200)
        struct.pack_into('<H', data, h+34, 0x210)
        # Imported names have no references here. Restrict their span using entry
        # placement; trailing old bytes are padding, never exports.
        actual = census.inspect(data)
        self.assertEqual(actual['exports'][0]['names'], ['Main'])
        resident = int.from_bytes(data[h+38:h+40], 'little')
        struct.pack_into('<H', data, h+36, resident)
        self.assertEqual(census.inspect(data)['resources'], [])

    def test_native_layout_and_null_arguments(self):
        lib = native.library()
        image, error = native.Image(), native.Error()
        self.assertNotEqual(lib.ne_parse(None, 0, C.byref(image), C.byref(error)), 0)
        self.assertEqual(lib.ne_rule_name(error.rule), b'invalid-argument')
        self.assertNotEqual(lib.ne_load_policy(C.byref(image), C.byref(error)), 0)


if __name__ == '__main__':
    unittest.main()
