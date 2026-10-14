"""Mock segment store, independent patch bytes and failure atomicity."""
# SPDX-License-Identifier: MIT
import ctypes as C
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from scripts.win16 import fixtures, native


def store_for(parsed, selectors=None):
    buffers, storage = [], []
    for i, seg in enumerate(parsed.image.segments[:parsed.image.segment_count]):
        buf = (native.U8 * (seg.allocation+2))(*([0xa5]*(seg.allocation+2)))
        storage.append(buf)
        ptr = C.cast(C.byref(buf, 1), C.POINTER(native.U8))
        buffers.append(native.Buffer(ptr, seg.allocation, selectors[i] if selectors else 0x87+i*8))
    store = (native.Buffer * len(buffers))(*buffers)
    for i, buf in enumerate(store):
        e = native.Error()
        if parsed.lib.ne_load_segment(C.byref(parsed.image), i+1, buf, C.byref(e)):
            raise native.NEError(parsed.lib.ne_rule_name(e.rule).decode(), e)
    return store, storage


class RelocationTests(unittest.TestCase):
    def test_every_applicable_fixture_and_rollback(self):
        seen = []
        @native.Resolver
        def resolve(opaque, image, module, ordinal, name, out):
            seen.append((module, ordinal, name.length))
            out.contents.offset, out.contents.selector = 0x20, 0x1234
            return 0
        for spec in fixtures.manifests():
            if spec['expect']['parse'] != 'ok' or spec['expect']['load'] != 'ok':
                continue
            with self.subTest(fixture=spec['id']):
                data = fixtures.synthetic(spec)
                parsed = native.Parsed(data)
                store, storage = store_for(parsed)
                before = [bytes(b) for b in storage]
                work, error = native.Workspace(), native.Error()
                results = []
                for i in range(len(store)):
                    result = parsed.lib.ne_relocate_segment(C.byref(parsed.image), i+1, store, len(store),
                                                           resolve, None, C.byref(work), C.byref(error))
                    results.append(parsed.lib.ne_rule_name(error.rule).decode() if result else 'ok')
                    if result:
                        self.assertEqual(bytes(storage[i]), before[i], 'failed segment must not be patched')
                wanted = spec['expect'].get('relocate', 'ok')
                self.assertIn(wanted, results or ['ok'])
                if wanted == 'ok':
                    self.assertEqual(set(results or ['ok']), {'ok'})
                for patch in spec['expect'].get('patched', []):
                    raw = bytes(storage[patch['segment']-1])
                    at = patch['offset']+1
                    expected = bytes.fromhex(patch['hex'])
                    self.assertEqual(raw[at:at+len(expected)], expected)
                self.assertEqual(bytes(parsed.backing), data)
                for buf in storage:
                    self.assertEqual((buf[0], buf[-1]), (0xa5, 0xa5))
        self.assertIn((1, 91, 0), seen)
        self.assertIn((1, 0, 11), seen) # LoadLibrary is 11 bytes.

    def test_missing_import_keeps_original_identity_and_entire_segment(self):
        spec = next(s for s in fixtures.manifests() if s['id'] == 'mixed-imports')
        parsed = native.Parsed(fixtures.synthetic(spec))
        store, storage = store_for(parsed)
        before = bytes(storage[0])
        calls = []
        @native.Resolver
        def missing(opaque, image, module, ordinal, name, out):
            calls.append((module, ordinal, parsed.text(name)))
            if ordinal:
                out.contents.offset, out.contents.selector = 0x20, 0x1234
                return 0
            return -1
        error, work = native.Error(), native.Workspace()
        result = parsed.lib.ne_relocate_segment(C.byref(parsed.image), 1, store, len(store),
                                               missing, None, C.byref(work), C.byref(error))
        self.assertNotEqual(result, 0)
        self.assertEqual(parsed.lib.ne_rule_name(error.rule), b'import-unresolved')
        self.assertEqual((error.segment, error.record), (1, 1))
        self.assertEqual(calls, [(1, 91, ''), (1, 0, 'LoadLibrary')])
        self.assertEqual(bytes(storage[0]), before)

    def test_selfloading_is_refused_without_writing_store(self):
        spec = next(s for s in fixtures.manifests() if s['id'] == 'selfload-module')
        parsed = native.Parsed(fixtures.synthetic(spec))
        raw = (native.U8 * 32)(*([0xa5]*32))
        error = native.Error()
        self.assertNotEqual(parsed.lib.ne_load_segment(C.byref(parsed.image), 1,
                                                     native.Buffer(raw, 32, 0x87), C.byref(error)), 0)
        self.assertEqual(parsed.lib.ne_rule_name(error.rule), b'self-loading-unsupported')
        self.assertEqual(bytes(raw), b'\xa5'*32)

    def test_large_import_offset_is_kept_for_32bit_fixups(self):
        @native.Resolver
        def large(opaque, image, module, ordinal, name, out):
            out.contents.offset, out.contents.selector = 0x12345678, 0x4321
            return 0
        for typ, expected in ((13, bytes.fromhex('78563412')), (11, bytes.fromhex('785634122143')), (3, None)):
            spec = {'model': {'modules':['KERNEL'], 'segments':[{'flags':64, 'hex':'00'*32, 'allocation':32,
                    'relocs':[{'type':typ, 'flags':1, 'source':0, 'target1':1, 'target2':91}]}]}}
            parsed = native.Parsed(fixtures.synthetic(spec))
            store, storage = store_for(parsed)
            before = bytes(storage[0])
            error, work = native.Error(), native.Workspace()
            result = parsed.lib.ne_relocate_segment(C.byref(parsed.image), 1, store, len(store),
                                                   large, None, C.byref(work), C.byref(error))
            if expected is None:
                self.assertNotEqual(result, 0)
                self.assertEqual(parsed.lib.ne_rule_name(error.rule), b'relocation-target')
                self.assertEqual(bytes(storage[0]), before)
            else:
                self.assertEqual(result, 0)
                self.assertEqual(bytes(storage[0])[1:1+len(expected)], expected)

    def test_short_store_is_refused_before_any_write(self):
        spec = next(s for s in fixtures.manifests() if s['id'] == 'reloc-pointer16-chain')
        parsed = native.Parsed(fixtures.synthetic(spec))
        store, storage = store_for(parsed)
        before = bytes(storage[0])
        store[0].size = 1
        error, work = native.Error(), native.Workspace()
        self.assertNotEqual(parsed.lib.ne_relocate_segment(C.byref(parsed.image), 1, store, len(store),
                                                         native.Resolver(), None, C.byref(work), C.byref(error)), 0)
        self.assertEqual(parsed.lib.ne_rule_name(error.rule), b'segment-store-bounds-or-selector')
        self.assertEqual(bytes(storage[0]), before)

    def relocate(self, spec, selectors=None, resolver=None):
        parsed = native.Parsed(fixtures.synthetic(spec))
        store, storage = store_for(parsed, selectors)
        before = [bytes(b) for b in storage]
        error, work = native.Error(), native.Workspace()
        result = parsed.lib.ne_relocate_segment(C.byref(parsed.image), 1, store, len(store),
                                               resolver or native.Resolver(), None, C.byref(work),
                                               C.byref(error))
        rule = parsed.lib.ne_rule_name(error.rule).decode() if result else 'ok'
        if result:
            self.assertEqual([bytes(b) for b in storage], before)
        return rule, bytes(storage[0])[1:-1]

    def test_same_plan_relocates_against_independent_selector_layouts(self):
        # Hand-computed: a far pointer chain at 0 and 8 to segment 2 offset 0x10,
        # and a movable entry (ordinal 1 -> segment 2:0x0004) selector at 20.
        spec = {'model': {
            'segments': [{'flags': 0x40, 'hex': '00'*32, 'allocation': 32, 'relocs': [
                {'type': 3, 'source': 0, 'target1': 2, 'target2': 0x10, 'sites': [[0, 8], [8, 0xffff]]},
                {'type': 2, 'source': 20, 'target1': 0xff, 'target2': 1}]},
                {'flags': 0x41, 'size': 16, 'allocation': 64}],
            'entries': [{'kind': 'movable', 'segment': 2, 'offset': 4}]}}
        for selectors, sel in (((0x0087, 0x008f), b'\x8f\x00'), ((0x1007, 0x2a3f), b'\x3f\x2a')):
            with self.subTest(selectors=selectors):
                rule, raw = self.relocate(spec, selectors)
                self.assertEqual(rule, 'ok')
                self.assertEqual(raw[0:4], b'\x10\x00'+sel)
                self.assertEqual(raw[8:12], b'\x10\x00'+sel)
                self.assertEqual(raw[20:22], sel)
                self.assertEqual(raw[4:8]+raw[12:20]+raw[22:], bytes(4+8+10))

    def test_byte_fixups_at_the_last_segment_byte(self):
        def spec(flags, site):
            return {'model': {'segments': [{'flags': 0x40, 'hex': '00'*31+'05', 'allocation': 32,
                    'relocs': [{'type': 0, 'flags': flags, 'source': site, 'target1': 1, 'target2': 0x0a,
                                'sites': [[site, 5 if flags & 4 else 0xff]], 'site_width': 1}]}]}}
        # Additive byte: 0x05 + 0x0a in the final byte.
        rule, raw = self.relocate(spec(4, 31))
        self.assertEqual((rule, raw[31]), ('ok', 0x0f))
        # A chained byte needs a two-byte link; the final byte cannot hold one,
        # so the plan is already refused structurally, before any store exists.
        with self.assertRaises(native.NEError) as refused:
            native.Parsed(fixtures.synthetic(spec(0, 31)))
        self.assertEqual(refused.exception.rule, 'relocation-patch-bounds')
        # At 30 the link word FFFF ends the chain; only the low byte changes.
        chained = spec(0, 30)
        chained['model']['segments'][0]['relocs'][0].update(sites=[[30, 0xffff]], site_width=2)
        rule, raw = self.relocate(chained)
        self.assertEqual((rule, raw[30:32]), ('ok', b'\x0a\xff'))

    def test_constant_entry_and_unresolved_import_keep_identity(self):
        spec = {'model': {'segments': [{'flags': 0x40, 'hex': '00'*16, 'allocation': 16, 'relocs': [
                    {'type': 5, 'source': 0, 'target1': 0xff, 'target2': 1}]}],
                'entries': [{'kind': 'constant', 'offset': 0x1234, 'flags': 1}]}}
        rule, raw = self.relocate(spec)
        self.assertEqual((rule, raw[0:2]), ('ok', b'\x34\x12'))


if __name__ == '__main__':
    unittest.main()
