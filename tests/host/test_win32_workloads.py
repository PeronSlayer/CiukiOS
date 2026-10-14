"""F5-17A manifest, provenance and bounded streaming-import host evidence."""
import copy
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'scripts/win32'))
import workloads


class WorkloadTests(unittest.TestCase):
    def setUp(self):
        self.manifest = workloads.load()

    def test_approved_rows_and_explicit_unknowns(self):
        rows = self.manifest['workloads']
        self.assertEqual(len(rows), 14)
        self.assertEqual({r['id'] for r in rows if r['gate'] == 'F5'}, workloads.APPROVED)
        self.assertTrue(all(r['source']['sha256'] is None for r in rows))
        self.assertTrue(all(not r['licence']['distribution_verified'] for r in rows))
        self.assertEqual(sum(r['scene']['focus_round_trips'] == 10 for r in rows), 3)
        self.assertEqual({r['id'] for r in rows if r['gate'] == 'F7'}, {'quake3-demo','tomb-raider4-demo'})
        self.assertEqual(rows[0]['source']['size'], 137576448)
        self.assertEqual(rows[1]['source']['sha1'], 'a29ccfb4b34d55034bbae6bde23199fc4766f288')

    def test_schema_rejects_dangerous_or_unsubstantiated_mutations(self):
        mutations = [
            lambda m: m.update(schema_version=2),
            lambda m: m.update(download_directory='/tmp'),
            lambda m: m['workloads'].pop(0),
            lambda m: m['workloads'].append(copy.deepcopy(m['workloads'][0])),
            lambda m: m['workloads'][0]['source'].update(file='../payload'),
            lambda m: m['workloads'][0]['source'].update(sha256='x'*64),
            lambda m: m['workloads'][0]['source'].update(size=True),
            lambda m: m['workloads'][0]['source'].update(url='http://example.org/game'),
            lambda m: m['workloads'][0]['licence'].update(distribution_verified=True),
            lambda m: m['workloads'][0]['installer'].update(arguments=['/S']),
            lambda m: m['workloads'][0]['scene'].update(play_seconds=179),
            lambda m: m['workloads'][0]['scene'].update(case_timeout_seconds=601),
            lambda m: m['workloads'][0]['unknowns'].remove('source.sha256'),
            lambda m: m['workloads'][12]['source'].update(url='https://example.org/game'),
            lambda m: m['workloads'][12]['source'].update(path_pattern='legacy/local/win32/../../game'),
        ]
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                m = copy.deepcopy(self.manifest)
                mutation(m)
                with self.assertRaises(ValueError):
                    workloads.validate(m)

    def test_verified_inventory_needs_actual_names_and_sizes(self):
        inventory = self.manifest['workloads'][0]['installed_files']
        inventory.update(status='verified', entries=[{'name':'DIABLO.EXE','size':123}])
        workloads.validate(self.manifest)
        inventory['entries'].append({'name':'diablo.exe','size':123})
        with self.assertRaises(ValueError):
            workloads.validate(self.manifest)

    def test_per_title_operator_records_exist_and_preserve_gate_requirements(self):
        for row in self.manifest['workloads']:
            operator = json.loads((ROOT/row['installer']['operator_script']).read_text())
            self.assertEqual(operator['workload'], row['id'])
            self.assertEqual(operator['scene'], row['scene']['id'])
            self.assertIsNone(operator['threshold_policy'])
            self.assertEqual(operator['serial'], '38400 8N1')
            self.assertEqual(operator['case_timeout_seconds'], 600)
            by_segment = {s['segment']:s for s in operator['steps']}
            self.assertEqual(by_segment['play']['minimum_seconds'], 180)
            self.assertEqual(by_segment['start']['deadline_seconds'], 120)
            self.assertEqual(by_segment['exit']['ledger_deadline_seconds'], 2)
            self.assertEqual('focus' in by_segment, row['scene']['focus_round_trips'] == 10)

    def fixture_row(self):
        payload = b'Ciuki self-authored download test; no original game bytes.\n'
        row = copy.deepcopy(self.manifest['workloads'][0])
        row['source'].update(file='fixture.bin', url='https://fixture.invalid/payload', size=len(payload),
            sha1=hashlib.sha1(payload).hexdigest(), sha256=hashlib.sha256(payload).hexdigest())
        return row, payload

    def test_streaming_fetch_and_cached_verification(self):
        row, payload = self.fixture_row()
        calls = []
        def opener(url, timeout):
            calls.append((url, timeout))
            return io.BytesIO(payload)
        with tempfile.TemporaryDirectory() as root:
            a = workloads.fetch(row, root, opener=opener)
            b = workloads.fetch(row, root, opener=opener)
            self.assertEqual(a, b)
            self.assertEqual(a['status'], 'pinned')
            self.assertEqual(len(calls), 1)
            path = Path(root)/workloads.DOWNLOAD_DIRECTORY/row['id']/'fixture.bin'
            self.assertEqual(path.read_bytes(), payload)
            path.write_bytes(b'bad')
            with self.assertRaisesRegex(ValueError, 'mismatch'):
                workloads.fetch(row, root, opener=opener)
            self.assertEqual(len(calls), 1)

    def test_sha1_candidate_import_does_not_promote_pin(self):
        row, payload = self.fixture_row()
        row['source']['sha256'] = None
        with tempfile.TemporaryDirectory() as root:
            with self.assertRaises(ValueError):
                workloads.fetch(row, root, opener=lambda *a, **k: self.fail('unexpected download'))
            receipt = workloads.fetch(row, root, import_unpinned=True, opener=lambda *a, **k: io.BytesIO(payload))
            self.assertEqual(receipt['status'], 'candidate-unreviewed')
            self.assertEqual(receipt['sha256'], hashlib.sha256(payload).hexdigest())
            self.assertIsNone(row['source']['sha256'])

    def test_size_and_digest_failures_remove_partial_download(self):
        row, payload = self.fixture_row()
        for damaged in (payload+b'oversize', payload[:-1], payload.replace(b'Ciuki',b'CIUKI')):
            with self.subTest(damaged=damaged), tempfile.TemporaryDirectory() as root:
                with self.assertRaises(ValueError):
                    workloads.fetch(row, root, opener=lambda *a, **k: io.BytesIO(damaged))
                self.assertFalse(list(Path(root).rglob('*.part')))
                self.assertFalse(list(Path(root).rglob('*.receipt.json')))

    def test_owner_local_is_never_fetched_or_written(self):
        row = self.manifest['workloads'][12]
        with tempfile.TemporaryDirectory() as root:
            with self.assertRaisesRegex(ValueError, 'owner-local'):
                workloads.fetch(row, root, opener=lambda *a, **k: self.fail('owner-local fetched'))
            self.assertEqual(list(Path(root).iterdir()), [])

    def test_download_symlink_escape_refused(self):
        row, payload = self.fixture_row()
        with tempfile.TemporaryDirectory() as root, tempfile.TemporaryDirectory() as outside:
            base = Path(root)/workloads.DOWNLOAD_DIRECTORY
            base.parent.mkdir(parents=True)
            base.symlink_to(outside, target_is_directory=True)
            with self.assertRaises(ValueError):
                workloads.fetch(row, root, opener=lambda *a, **k: io.BytesIO(payload))
            self.assertEqual(list(Path(outside).iterdir()), [])

    def test_existing_partial_is_preserved_and_http_redirect_refused(self):
        row, payload = self.fixture_row()
        with tempfile.TemporaryDirectory() as root:
            part = Path(root)/workloads.DOWNLOAD_DIRECTORY/row['id']/'fixture.bin.part'
            part.parent.mkdir(parents=True)
            part.write_bytes(b'another import in progress')
            with self.assertRaises(FileExistsError):
                workloads.fetch(row, root, opener=lambda *a, **k: self.fail('unexpected download'))
            self.assertEqual(part.read_bytes(), b'another import in progress')
        class Redirect(io.BytesIO):
            def geturl(self): return 'http://fixture.invalid/payload'
        with tempfile.TemporaryDirectory() as root:
            with self.assertRaisesRegex(ValueError, 'redirect'):
                workloads.fetch(row, root, opener=lambda *a, **k: Redirect(payload))
            self.assertFalse(list(Path(root).rglob('*.part')))


if __name__ == '__main__':
    unittest.main()
