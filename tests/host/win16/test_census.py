"""Reproducible corpus and text/JSON census compared to independent manifests."""
# SPDX-License-Identifier: MIT
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from scripts.win16 import census, fixtures, native


class CensusTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        parent = ROOT/'build/host/win16'
        parent.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='repro-', dir=parent)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.first, cls.second = Path(cls.temp.name)/'first', Path(cls.temp.name)/'second'
        cls.hashes = fixtures.build(cls.first)
        cls.other = fixtures.build(cls.second)

    def test_two_builds_match_pinned_hashes_and_text_census(self):
        self.assertEqual(self.hashes, self.other)
        self.assertEqual(self.hashes, json.loads((fixtures.SOURCES/'hashes.json').read_text()))
        self.assertEqual(fixtures.census_text(self.first), fixtures.census_text(self.second))

    def test_every_census_import_and_export_matches_manifests(self):
        for spec in fixtures.manifests():
            with self.subTest(fixture=spec['id']):
                data = (self.first/(spec['id']+'.ne')).read_bytes()
                digest = hashlib.sha256(data).hexdigest()
                if spec['expect']['parse'] != 'ok':
                    with self.assertRaises(native.NEError) as error:
                        census.inspect(data)
                    self.assertEqual(str(error.exception), spec['expect']['parse'])
                    text = census.format_text(spec['id'], digest, error=str(error.exception), size=len(data))
                    self.assertIn('REFUSED '+spec['expect']['parse'], text)
                    self.assertNotIn('STATIC', text)
                else:
                    result = census.inspect(data)
                    self.assertEqual(result['imports'], spec['expect']['imports'])
                    self.assertEqual(result['exports'], spec['expect']['exports'])
                    text = census.format_text(spec['id'], digest, result, size=len(data))
                    self.assertIn('DYNAMIC not-observed', text)
                    self.assertIn('EVIDENCE static-host-only', text)
                    for imp in spec['expect']['imports']:
                        self.assertIn(census.safe(imp['dll']), text)
                        for symbol in imp['symbols']:
                            self.assertIn(census.safe(symbol), text)
                self.assertIn('SHA256 '+digest, text)

    def test_cli_continues_after_refusal_and_never_reuses_a_previous_digest(self):
        command = ['nice', '-n', '19', sys.executable, '-B', ROOT/'scripts/win16/census.py', '--json',
                   self.first/'import-name.ne', self.first/'bad-ne-magic.ne', self.first/'missing.ne',
                   self.first/'basic.ne']
        p = subprocess.run(command, capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 1, p.stderr)
        rows = [json.loads(line) for line in p.stdout.splitlines()]
        self.assertEqual(rows[0]['imports'][0]['symbols'], ['LoadLibrary'])
        self.assertEqual(rows[1]['error'], 'ne-signature')
        self.assertNotEqual(rows[0]['sha256'], rows[1]['sha256'])
        self.assertNotIn('sha256', rows[2])
        self.assertEqual(rows[3]['imports'], [])

    def test_committed_text_census_is_reproduced(self):
        # Golden evidence: regenerate with scripts/win16/fixtures.py --census.
        self.assertEqual(fixtures.census_text(self.first),
                         (fixtures.SOURCES/'census.txt').read_text())

    def test_cli_binds_acceptance_provenance_to_each_record(self):
        command = ['nice', '-n', '19', sys.executable, '-B', ROOT/'scripts/win16/census.py',
                   '--workload', 'gate-6', '--archive', 'demo.zip sha256=unknown',
                   '--member', 'SETUP.EXE', '--provenance', 'owner-local-restricted',
                   '--extraction', 'unzip 6.0', self.first/'mixed-imports.ne']
        p = subprocess.run(command, capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 0, p.stderr)
        lines = p.stdout.splitlines()
        self.assertEqual(lines[3:8], ['WORKLOAD gate-6', 'ARCHIVE demo.zip sha256=unknown',
                                      'MEMBER SETUP.EXE', 'PROVENANCE owner-local-restricted',
                                      'EXTRACTION unzip 6.0'])
        self.assertIn('STATIC [1] KERNEL -> #91, LoadLibrary', lines)
        self.assertIn('STATIC [2] USER -> #1', lines)
        twice = subprocess.run(command[:-1]+[self.first/'basic.ne', self.first/'basic.ne'],
                               capture_output=True, text=True, timeout=60)
        self.assertEqual(twice.returncode, 2)

    def test_text_escapes_arbitrary_metadata_without_record_injection(self):
        text = census.format_text('line\nFILE spoof', '0'*64, error='bad\nSTATIC fake')
        self.assertIn(r'line\nFILE spoof', text)
        self.assertNotIn('\nSTATIC fake', text)
        spec = {'model': {'modules':['KERNEL\nFILE spoof'], 'resident':[['FIXTURE',0], ['Main\nREFUSED spoof',1]]}}
        result = census.inspect(fixtures.synthetic(spec))
        text = census.format_text('synthetic', '0'*64, result)
        self.assertNotIn('\nFILE spoof', text)
        self.assertNotIn('\nREFUSED spoof', text)


if __name__ == '__main__':
    unittest.main()
