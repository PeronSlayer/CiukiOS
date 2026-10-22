"""f5-15B per-engine acceptance drivers on the H1 Linux adapter.

One command per engine:
  nice -n 19 python3 -B -m unittest tests.host.win32.installers.test_titles.WiseTests
  ... InstallShieldTests, IExpressTests, WinZipTests, VendorTests, CdImageTests
Each title runs only when the lead imported its download
(scripts/win32/workloads.py --import-unpinned) and, for CD installs, extracted
the CD tree under build/win32-workloads/<id>/cd/. A pass here means the
personality accepted the original installer and served every request up to
the adapter's documented boundary (desktop service, USER queue, threads) or
to completion; the records go to build/host/win32/installers/titles/<id>/.
The UI-driven phases are the QEMU `installers` tier (the lead).
"""
# SPDX-License-Identifier: MIT
import json
import unittest

from scripts.win32 import installer_fixture as fixture
from . import host_services, test_flow_fixtures, titles

OUT = test_flow_fixtures.OUT


class TitleTests(unittest.TestCase):
    engine = None
    _program = None
    _library = None

    @classmethod
    def setUpClass(cls):
        if cls.engine is None:
            raise unittest.SkipTest('engine base class')
        OUT.mkdir(parents=True, exist_ok=True)
        try:
            fixture.build(OUT / 'fixtures')
        except FileNotFoundError as error:
            raise unittest.SkipTest(str(error)) from error

    def program(self):
        if TitleTests._program is None:
            TitleTests._program = test_flow_fixtures.h1_program(OUT / 'fixtures', self)
            TitleTests._library, _, _ = host_services.build_bridge(OUT / 'services')
        return TitleTests._program, TitleTests._library

    def titles(self):
        found = [ident for ident in titles.spec_ids() if titles.load_spec(ident)['engine'] == self.engine]
        self.assertTrue(found, 'no title spec for engine ' + self.engine)
        return found

    def test_specs_match_the_workload_manifest(self):
        for ident in self.titles():
            spec = titles.load_spec(ident)
            row = spec['_row']
            self.assertEqual(row['gate'], 'F5', ident)
            self.assertNotEqual(row['licence']['class'], 'owner-local', ident)
            if spec['entry']['kind'] == 'download':
                self.assertEqual(spec['entry']['file'], row['source']['file'], ident)
            self.assertIn(spec['qemu_case'], self.suite_cases(), ident)

    @staticmethod
    def suite_cases():
        suite = json.loads((titles.ROOT / 'tests/suites/f5-installers.json').read_text())
        return {case['id'] for case in suite['cases']}

    def test_original_installers_on_the_adapter(self):
        for ident in self.titles():
            with self.subTest(title=ident):
                spec = titles.load_spec(ident)
                if spec['deferred']:
                    signature = titles.win16_child(spec)
                    if signature is None:
                        self.skipTest(ident + ': deferred (' + spec['deferred']['reason'] + '); setup stub not extracted')
                    self.assertEqual(signature, 'NE', ident + ': the deferral rests on a Win16 setup stub')
                    record = titles.run_title(ident, None, None)
                    self.assertEqual(record['deferred']['reason'], spec['deferred']['reason'])
                    continue
                if titles.payload(spec) is None:
                    self.skipTest(ident + ': payload absent under ' + str(titles.DOWNLOADS / ident)
                                  + (' or ' + str(titles.CD_TREES / ident / spec['entry']['tree'])
                                     if spec['entry']['kind'] == 'cd' else ''))
                program, library = self.program()
                record = titles.run_title(ident, program, library)
                self.assertIsNotNone(record['receipt'], ident + ': import receipt missing (workloads.py --import-unpinned)')
                self.assertTrue(record['receipt']['sha1_verified'], ident)
                for result in record['variants']:
                    with self.subTest(title=ident, variant=result['variant']):
                        self.assertIsNone(result.get('close_error'), result.get('close_error'))
                        self.assertIsNone(result['refuse'], f"{ident}: loader refusal {result['refuse']}")
                        self.assertIsNone(result['crash'], f"{ident}: crash {result['crash']}")
                        self.assertIn(result['verdict'], ('boundary', 'completed'),
                                      f"{ident}/{result['variant']}: {result['verdict']} "
                                      f"boundary={result['boundary']} enosys={result['enosys']} "
                                      f"terminate={result['terminate']} stubs={result['stubs']}")
                        self.assertFalse([k for k in result['files_delta'] if k.startswith('C/System/')],
                                         f"{ident}/{result['variant']}: writes under C:\\System")
                        variant = next(v for v in spec['variants'] if v['name'] == result['variant'])
                        for wanted in variant.get('expect_records', ()):
                            self.assertTrue(any(wanted == r['event'] + ' ' + r['module'] + ' ' + r['detail']
                                                for r in result['records']),
                                            f"{ident}/{result['variant']}: record '{wanted}' absent: {result['records']}")
                        if result['verdict'] == 'completed' and spec['expected']['status'] == 'verified':
                            self.assertEqual(result['phases']['copy'], 'pass', result['phases'])


class WiseTests(TitleTests):
    engine = 'wise'


class InstallShieldTests(TitleTests):
    engine = 'installshield'


class IExpressTests(TitleTests):
    engine = 'iexpress'


class WinZipTests(TitleTests):
    engine = 'winzip'


class VendorTests(TitleTests):
    engine = 'vendor'


class CdImageTests(TitleTests):
    engine = 'cd'


if __name__ == '__main__':
    unittest.main()
