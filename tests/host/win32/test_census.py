"""Independent fixture oracles and hostile input checks for the metadata census."""
# SPDX-License-Identifier: MIT
import hashlib
import json
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT))
from scripts.win32 import census, fixtures


class CensusTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        parent = ROOT/'build/host/win32'
        parent.mkdir(parents=True,exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='census-',dir=parent)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.out = Path(cls.temp.name)
        cls.hashes = fixtures.build(cls.out)

    def test_every_fixture_matches_source_manifest(self):
        for spec in fixtures.manifests():
            ext = '.dll' if ((spec['kind']=='compiled' and spec.get('dll')) or 'dll' in spec.get('features',[])) else '.exe'
            data = (self.out/(spec['id']+ext)).read_bytes()
            with self.subTest(fixture=spec['id']):
                expected = spec['census']
                if 'error' in expected:
                    with self.assertRaises(census.PEError) as ctx:
                        census.inspect(data)
                    self.assertEqual(str(ctx.exception),expected['error'])
                else:
                    got = census.inspect(data)
                    for key,value in expected.items():
                        self.assertEqual(got[key],value,key)

    def test_committable_census_is_reproducible_and_current(self):
        text = fixtures.census_text(self.out)
        self.assertEqual(text,fixtures.census_text(self.out))
        self.assertEqual(text,(ROOT/'docs/design/win32-census/fixtures.txt').read_text())
        self.assertIn('STATIC provider.dll -> FixtureValue, #11',text)
        self.assertIn('DELAY provider.dll -> FixtureValue, #11',text)
        self.assertIn('BOUND provider.dll timestamp=123',text)
        self.assertIn('EXPORT #7 FixtureValue -> provider.#11',text)
        self.assertIn('REFUSED machine-not-i386',text)
        self.assertNotIn('fixture-data',text)
        self.assertNotIn('<assembly',text)

    def test_truncation_and_seeded_header_mutations_are_bounded(self):
        data = fixtures.template()
        for end in range(0,512):
            with self.subTest(truncated_at=end):
                try:
                    census.inspect(bytes(data[:end]))
                except census.PEError:
                    pass
        rng = random.Random(502)
        for i in range(256):
            changed = bytearray(data)
            p = rng.randrange(0,512)
            changed[p:p+4] = rng.randbytes(4)
            try:
                census.inspect(changed)
            except census.PEError:
                pass  # no struct.error, IndexError, hang or unbounded allocation

    def test_manifest_entity_and_malformed_xml(self):
        for raw,reason in [(b'<!DOCTYPE x [<!ENTITY a "x">]><x/>','manifest-entity-declaration'),
                           (b'<assembly>','manifest-xml')]:
            with self.assertRaises(census.PEError) as ctx:
                census.manifest_metadata(raw)
            self.assertEqual(str(ctx.exception),reason)
        utf16 = '<assembly><assemblyIdentity name="utf16"/></assembly>'.encode('utf-16')
        self.assertEqual(census.manifest_metadata(utf16),[{'kind':'assemblyIdentity','name':'utf16'}])
        with self.assertRaisesRegex(census.PEError,'manifest-entity-declaration'):
            census.manifest_metadata('<!DOCTYPE x [<!ENTITY a "x">]><x/>'.encode('utf-16'))

    def test_metadata_cannot_inject_text_records(self):
        self.assertEqual(census.safe('x\nFILE forged\t\x1b'),'x\\nFILE forged\\t\\u001b')
        got = census.inspect((self.out/'console.exe').read_bytes())
        got['imports'] = [{'dll':'bad\nFILE forged','symbols':['F\nREFUSED false'],'kind':'static'}]
        text = census.format_text('x\nINJECT','0'*64,got)
        self.assertNotIn('\nFILE forged',text)
        self.assertNotIn('\nREFUSED false',text)

    def test_cli_json_sidecar_batch_and_failure_exit(self):
        exe = self.out/'console.exe'
        sidecar = Path(str(exe)+'.manifest')
        sidecar.write_text('<assembly><assemblyIdentity name="Sidecar"/></assembly>')
        args = [sys.executable,ROOT/'scripts/win32/census.py','--json',exe,
                self.out/'bad-machine.exe',self.out/'absent.exe']
        p = subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=30)
        self.assertEqual(p.returncode,1,p.stderr)
        rows = [json.loads(line) for line in p.stdout.splitlines()]
        self.assertEqual(rows[0]['sha256'],hashlib.sha256(exe.read_bytes()).hexdigest())
        self.assertEqual(rows[0]['manifests'],[{'kind':'assemblyIdentity','name':'Sidecar'}])
        self.assertEqual(rows[1]['error'],'machine-not-i386')
        self.assertIn('absent.exe',rows[2]['error'])
        self.assertNotIn('sha256',rows[2])


if __name__ == '__main__':
    unittest.main()
