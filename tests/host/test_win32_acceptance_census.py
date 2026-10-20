"""Acceptance resolver classifications from actual authored PE export tables."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from scripts.win32 import acceptance_census as ac, fixtures


class CensusResolutionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.providers = self.base / 'providers'; self.providers.mkdir()
        self.coverage = self.base / 'provider-coverage.json'
        self.provider = self.providers / 'provider.dll'
        self.provider.write_bytes(fixtures.synthetic({'id': 'provider', 'features': ['dll', 'exports']}))

    def resolver(self, state='implemented'):
        self.coverage.write_text(json.dumps({'exports': [{'name': 'FixtureValue', 'status': state, 'directive': 'f5-test'}]}))
        return ac.Resolver([self.providers], [self.coverage])

    def test_implemented_stub_and_ordinal_alias_are_computed(self):
        for value, expected in [('implemented', 'implemented'), ('W21 typed failure', 'W21 stub')]:
            r = self.resolver(value)
            self.assertEqual(r.resolve('PROVIDER.DLL', 'FixtureValue')['resolution'], expected)
            self.assertEqual(r.resolve('provider.dll', '#7')['resolution'], expected)
            self.assertEqual(r.resolve('provider.dll', '#8')['resolution'], 'unresolved')
            self.assertEqual(r.resolve('provider.dll', 'Missing')['resolution'], 'unresolved')

    def test_presence_without_classification_cannot_pass(self):
        r = ac.Resolver([self.providers])
        self.assertEqual(r.resolve('provider.dll', 'FixtureValue')['reason'], 'export-classification-pending')
        r = self.resolver(); self.provider.unlink()
        self.assertEqual(ac.Resolver([self.providers], [self.coverage]).resolve('provider.dll', 'FixtureValue')['resolution'], 'unresolved')

    def test_forwarder_to_stub_and_cycle(self):
        forward = self.providers / 'forward.dll'
        forward.write_bytes(fixtures.synthetic({'id': 'forward', 'features': ['dll', 'forward-ordinal'],
                                               'forwarder': 'provider.#7'}))
        result = self.resolver('W21 typed failure').resolve('forward.dll', 'FixtureValue')
        self.assertEqual(result['resolution'], 'W21 stub')
        self.assertEqual(result['chain'], ['forward.dll!FixtureValue', 'provider.dll!#7'])
        forward.write_bytes(fixtures.synthetic({'id': 'forward', 'features': ['dll', 'forward-name'],
                                               'forwarder': 'forward.FixtureValue'}))
        self.assertEqual(self.resolver().resolve('forward.dll', '#7')['reason'], 'forwarder-cycle-or-depth')

    def test_w24_requires_explicit_directive_and_generated_stub_metadata(self):
        self.coverage.write_text(json.dumps({'exports': [{'name': 'Absent', 'status': 'W24 refusal', 'directive': 'f5-owner-decision'}]}))
        r = ac.Resolver([], [self.coverage])
        self.assertEqual(r.resolve('provider.dll', 'Absent')['resolution'], 'W24 refusal')
        metadata = {'dlls': [{'name': 'provider.dll', 'exports': [{'name': 'FixtureValue', 'ordinal': 7, 'implementation': None}]}]}
        self.coverage.write_text(json.dumps(metadata))
        r = ac.Resolver([self.providers], [self.coverage])
        self.assertEqual(r.resolve('provider.dll', '#7')['resolution'], 'W21 stub')
        self.assertEqual(r.resolve('provider.dll', '#7')['directive'], 'f5-08')

    def test_store_hash_and_duplicate_provider_refusals(self):
        store = self.providers / 'builtin-store.json'
        store.write_text(json.dumps({'dlls': [{'path': 'provider.dll', 'sha256': '0' * 64}]}))
        with self.assertRaisesRegex(ValueError, 'hash mismatch'): ac.Resolver([self.providers])
        store.unlink()
        with self.assertRaisesRegex(ValueError, 'duplicate provider'): ac.Resolver([self.providers, self.providers])

    def test_regenerated_absent_inputs_are_explicit_and_reproducible(self):
        (self.base / 'tests/win32').mkdir(parents=True)
        (self.base / 'tests/win32/workloads.json').write_bytes((ROOT / 'tests/win32/workloads.json').read_bytes())
        result = ac.generate(self.base)
        self.assertEqual(result, ac.generate(self.base))
        self.assertEqual(len(result['workloads']), 10)
        self.assertFalse(result['complete'])
        self.assertEqual(sum(result['counts'].values()), 0)
        self.assertTrue(all('acceptance_executables_absent' in r['missing'] for r in result['workloads']))
        # Add an authored binary under the Diablo extraction location, with
        # known mixed static imports; no commercial executable enters the test.
        folder = self.base / 'build/downloads/win32/diablo-shareware/extracted'
        folder.mkdir(parents=True)
        (folder / 'SETUP.EXE').write_bytes(fixtures.synthetic({'id': 'importer', 'features': ['import-mixed']}))
        result = ac.generate(self.base, self.resolver('W21 typed failure'))
        self.assertEqual(len(result['imports']), 2)
        self.assertEqual(result['counts']['W21 stub'], 1)
        self.assertEqual(result['counts']['unresolved'], 1)  # #11 not exported
        self.assertEqual(result['unresolved'][0]['symbol'], '#11')


if __name__ == '__main__':
    unittest.main()
