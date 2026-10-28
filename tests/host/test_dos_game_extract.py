"""fx-games20c host checks: tiny synthetic media only, no guest/image build."""
import copy
from contextlib import redirect_stdout
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/test'))
import acceptance
import dos_game_extract as extract
import dos_games as games
from evidence import EvidenceNotRun

SCRATCH = ROOT / 'build/host/games20c'
SCRATCH.mkdir(parents=True, exist_ok=True)


def zip_bytes(rows, prefix=b''):
    stream = io.BytesIO(prefix)
    stream.seek(0, 2)
    with zipfile.ZipFile(stream, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        for name, body in rows:
            archive.writestr(name, body)
    return stream.getvalue()


def shr_bytes():
    # The public DCL format vector used by zlib blast.c, never game data.
    packed = bytes.fromhex('00048224258f807f')
    entry = bytearray(0xA8)
    entry[:9] = b'GAME.EXE\0'
    struct.pack_into('<I', entry, 0x88, len(packed))
    return bytes(0x3A) + entry + packed


def lha_bytes():
    # Single-symbol LH5 trees produce three ASCII As.
    bits = ''.join(format(v, f'0{w}b') for v, w in [(3, 16), (0, 5), (0, 5), (0, 9), (65, 9), (0, 4), (0, 4)])
    bits += '0' * (-len(bits) % 8)
    body = int(bits, 2).to_bytes(len(bits) // 8, 'big')
    header = bytearray(22)
    header[2:7] = b'-lh5-'
    struct.pack_into('<II', header, 7, len(body), 3)
    name = b'GAME.EXE'
    header[21] = len(name)
    header += name + struct.pack('<H', extract.decoder().lha_crc16(b'AAA'))
    header[0] = len(header) - 2
    header[1] = sum(header[2:]) & 255
    return bytes(header) + body + b'\0'


class ExtractionTests(unittest.TestCase):
    def fixture(self, root, archive=None, recipe=None):
        title = {'id': 'sample', 'executable': 'GAME.EXE',
                 'installed': {'directory': games.CACHE + '/sample-installed', 'files': []}}
        archive = archive if archive is not None else zip_bytes([('GAME.EXE', b'MZ synthetic'), ('DATA.BIN', b'data'), ('README.TXT', b'document')])
        pin = {'archive': 'sample.zip', 'status': 'pinned', 'sha256': hashlib.sha256(archive).hexdigest(), 'size': len(archive)}
        plan = {'extract': recipe or {'format': 'zip'}, 'comparison': {'config': [], 'reference_only': [], 'installer_only': []}}
        cache = Path(root) / games.CACHE
        cache.mkdir(parents=True, exist_ok=True)
        (cache / pin['archive']).write_bytes(archive)
        return title, pin, plan

    def test_all_container_formats_preserve_payload_and_documents(self):
        payload = [('GAME.EXE', b'synthetic'), ('README.TXT', b'original doc')]
        inner = zip_bytes(payload, b'MZ synthetic executable prefix')
        cases = [({'format': 'zip'}, zip_bytes(payload), dict(payload)),
                 ({'format': 'zip-sfx', 'member': 'GAME.SHR'}, zip_bytes([('INSTALL.EXE', b'MZ installer'), ('GAME.SHR', inner)]), dict(payload)),
                 ({'format': 'zip-split', 'parts': ['PART.1', 'PART.2']}, zip_bytes([('PART.1', inner[:19]), ('PART.2', inner[19:])]), dict(payload)),
                 ({'format': 'apogee-shr', 'member': 'GAME.SHR'}, zip_bytes([('GAME.SHR', shr_bytes())]), {'GAME.EXE': b'AIAIAIAIAIAIA'}),
                 ({'format': 'lha-sfx', 'member': 'RESOURCE.1', 'offset': 2}, zip_bytes([('RESOURCE.1', b'MZ' + lha_bytes())]), {'GAME.EXE': b'AAA'})]
        for recipe, data, expected in cases:
            with self.subTest(recipe=recipe):
                outer, actual = extract.unpack(data, recipe)
                self.assertTrue(outer)
                self.assertEqual(actual, expected)

    def test_zip_traversal_links_duplicates_special_files_and_collisions(self):
        for name in ('../X.EXE', '/X.EXE', 'C:/X.EXE', 'SUB\\X.EXE', 'SUB/../../X', 'LONGFILENAME.EXE'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                extract.zip_members(zip_bytes([(name, b'x')]))
        for rows in ([('GAME.EXE', b'a'), ('game.exe', b'b')], [('SUB', b'a'), ('SUB/GAME.EXE', b'b')], [('SUB', b'a'), ('SUB/', b'')]):
            with self.subTest(rows=rows), self.assertRaises(ValueError): extract.zip_members(zip_bytes(rows))
        for mode in (0o120777, 0o010600, 0o060600):
            info = zipfile.ZipInfo('GAME.EXE'); info.create_system = 3; info.external_attr = mode << 16
            with self.subTest(mode=mode), self.assertRaises(ValueError): extract.zip_members(zip_bytes([(info, b'elsewhere')]))
        # FAT creators may store unrelated data in the high attribute word.
        info = zipfile.ZipInfo('GAME.EXE'); info.create_system = 0; info.external_attr = 0o120777 << 16
        self.assertEqual(extract.zip_members(zip_bytes([(info, b'MZ')])), {'GAME.EXE': b'MZ'})

    def test_zip_sizes_crc_nul_and_count_checked(self):
        data = zip_bytes([('GAME.EXE', b'MZ synthetic')])
        with patch.object(extract, 'MAX_BYTES', 4), self.assertRaisesRegex(ValueError, 'budget'): extract.zip_members(data)
        with patch.object(extract, 'MAX_FILES', 1), self.assertRaisesRegex(ValueError, 'count'):
            extract.zip_members(zip_bytes([('GAME.EXE', b'a'), ('DATA.BIN', b'b')]))
        # Change stored bytes, leaving the central directory CRC unmodified.
        stream = io.BytesIO()
        with zipfile.ZipFile(stream, 'w') as z: z.writestr('GAME.EXE', b'HELLO')
        damaged = stream.getvalue().replace(b'HELLO', b'JELLO')
        with self.assertRaisesRegex(ValueError, 'CRC'): extract.zip_members(damaged)
        nul = stream.getvalue().replace(b'GAME.EXE', b'GA\0E.EXE')
        with self.assertRaisesRegex(ValueError, 'unsafe ZIP'): extract.zip_members(nul)

    def test_shr_truncation_trailing_and_aggregate_bounds(self):
        with self.assertRaises(ValueError): extract.shr_members(shr_bytes()[:-1])
        with patch.object(extract, 'MAX_BYTES', 3), self.assertRaisesRegex(ValueError, 'large'): extract.shr_members(shr_bytes())
        with self.assertRaisesRegex(ValueError, 'missing container'):
            extract.unpack(zip_bytes([('GAME.EXE', b'MZ')]), {'format': 'zip-sfx', 'member': 'MISSING.SHR'})
        with self.assertRaises(ValueError): extract.unpack(b'not a zip', {'format': 'zip'})

    def test_reference_manifest_never_creates_or_pins_installed_tree(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            root = Path(tmp); title, pin, plan = self.fixture(root)
            result = extract.extract(title, pin, plan, root)
            target = Path(result['directory'])
            manifest = json.loads((target / 'manifest.json').read_text())
            self.assertEqual(manifest['kind'], 'extraction-reference-only')
            self.assertEqual(manifest['archive_sha256'], pin['sha256'])
            self.assertEqual((target / 'payload/README.TXT').read_bytes(), b'document')
            self.assertFalse((root / title['installed']['directory']).exists())
            self.assertEqual(title['installed']['files'], [])
            with self.assertRaisesRegex(ValueError, 'already exists'): extract.extract(title, pin, plan, root)

    def test_bad_pin_external_input_and_sow_exclusion(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            root = Path(tmp); title, pin, plan = self.fixture(root)
            cache = root / games.CACHE
            readonly = root / 'input'; cache.rename(readonly)
            self.assertEqual(extract.reference_payload(title, pin, plan, root, readonly)[1]['DATA.BIN'], b'data')
            pin['sha256'] = '0' * 64
            with self.assertRaisesRegex(ValueError, 'pin mismatch'): extract.extract(title, pin, plan, root, readonly)
            self.assertFalse((root / extract.REFERENCE).exists())
            plan['extract'] = None
            with self.assertRaisesRegex(EvidenceNotRun, 'segmented_sow'): extract.extract(title, pin, plan, root, readonly)

    def test_reference_output_symlink_and_installed_parent_escape(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            root = Path(tmp); title, pin, plan = self.fixture(root)
            (root / extract.REFERENCE).symlink_to(SCRATCH, target_is_directory=True)
            with self.assertRaisesRegex(ValueError, 'escape'): extract.extract(title, pin, plan, root)
            cache = root / games.CACHE
            cache.rename(root / 'outside-cache'); cache.symlink_to(root / 'outside-cache', target_is_directory=True)
            with self.assertRaisesRegex(ValueError, 'escape'): extract.scan_installed(title, root)

    def test_comparison_pins_every_data_file_and_explicit_config_exceptions(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            root = Path(tmp); title, pin, plan = self.fixture(root)
            _, payload = extract.reference_payload(title, pin, plan, root)
            directory = root / title['installed']['directory']; directory.mkdir()
            for name, body in payload.items(): (directory / name).write_bytes(body)
            title['installed']['files'] = extract.scan_installed(title, root)
            self.assertEqual(extract.compare_installed(title, pin, plan, root)['status'], 'PASS')
            (directory / 'GAME.CFG').write_bytes(b'IRQ5')
            with self.assertRaisesRegex(ValueError, 'pins'): extract.compare_installed(title, pin, plan, root)
            title['installed']['files'] = extract.scan_installed(title, root)
            result = extract.compare_installed(title, pin, plan, root)
            self.assertEqual(result['errors']['unexpected'], ['GAME.CFG'])
            plan['comparison']['config'] = ['GAME.CFG']
            self.assertEqual(extract.compare_installed(title, pin, plan, root)['status'], 'PASS')
            (directory / 'DATA.BIN').write_bytes(b'changed data')
            title['installed']['files'] = extract.scan_installed(title, root)
            result = extract.compare_installed(title, pin, plan, root)
            self.assertEqual(result['status'], 'FAIL'); self.assertEqual(result['errors']['changed'], ['DATA.BIN'])
            (directory / 'DATA.BIN').write_bytes(b'data'); (directory / 'README.TXT').unlink()
            (directory / 'INSTALL.LOG').write_bytes(b'installer log')
            title['installed']['files'] = extract.scan_installed(title, root)
            plan['comparison'].update(reference_only=['README.TXT'], installer_only=['INSTALL.LOG'])
            self.assertEqual(extract.compare_installed(title, pin, plan, root)['status'], 'PASS')
            plan['comparison']['config'].append('ABSENT.CFG')
            self.assertEqual(extract.compare_installed(title, pin, plan, root)['status'], 'FAIL')

    def test_modified_default_config_allowed_but_missing_config_fails(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            root = Path(tmp)
            data = zip_bytes([('GAME.EXE', b'MZ'), ('GAME.CFG', b'default')])
            title, pin, plan = self.fixture(root, data)
            directory = root / title['installed']['directory']; directory.mkdir()
            (directory / 'GAME.EXE').write_bytes(b'MZ'); (directory / 'GAME.CFG').write_bytes(b'IRQ5')
            title['installed']['files'] = extract.scan_installed(title, root)
            plan['comparison']['config'] = ['GAME.CFG']
            self.assertEqual(extract.compare_installed(title, pin, plan, root)['status'], 'PASS')
            (directory / 'GAME.CFG').unlink(); title['installed']['files'] = extract.scan_installed(title, root)
            result = extract.compare_installed(title, pin, plan, root)
            self.assertEqual(result['status'], 'FAIL'); self.assertEqual(result['errors']['missing'], ['GAME.CFG'])

    def test_cli_extract_pin_and_compare_use_separate_synthetic_trees(self):
        from tests.host.test_dos_games20 import catalog_root
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp, redirect_stdout(io.StringIO()):
            root = catalog_root(tmp)
            title, pin = games.title_for('doom', root)
            inner = zip_bytes([('DOOM.EXE', b'MZ synthetic'), ('DATA.BIN', b'original')])
            data = zip_bytes([('DOOMS_19.1', inner[:25]), ('DOOMS_19.2', inner[25:])])
            pins = json.loads((root / games.PINS).read_text())
            pin.update(size=len(data), sha256=hashlib.sha256(data).hexdigest())
            pins['titles'] = [pin if p['id'] == 'doom' else p for p in pins['titles']]
            (root / games.PINS).write_text(json.dumps(pins))
            path = root / pin['licence']['source']; upstream = json.loads(path.read_text())
            upstream.update(size=pin['size'], sha256=pin['sha256']); path.write_text(json.dumps(upstream))
            archive = root / games.CACHE / pin['archive']; archive.parent.mkdir(parents=True)
            archive.write_bytes(data)
            self.assertEqual(games.main(['extract', '--title', 'doom'], root), 0)
            self.assertFalse((root / title['installed']['directory']).exists())
            installed = root / title['installed']['directory']; installed.mkdir()
            (installed / 'DOOM.EXE').write_bytes(b'MZ synthetic'); (installed / 'DATA.BIN').write_bytes(b'original')
            self.assertEqual(games.main(['pin-installed', '--title', 'doom'], root), 0)
            self.assertEqual(games.main(['compare-installed', '--title', 'doom'], root), 0)
            (installed / 'DATA.BIN').write_bytes(b'corrupted')
            self.assertEqual(games.main(['pin-installed', '--title', 'doom'], root), 0)
            self.assertEqual(games.main(['compare-installed', '--title', 'doom'], root), 1)


class InstallPlanTests(unittest.TestCase):
    def test_twenty_cited_cases_eighteen_extraction_recipes_and_no_guessed_keys(self):
        titles, _ = games.load(); plans = games.installation_plans()
        self.assertEqual(len(plans), 20)
        self.assertEqual({k for k,v in plans.items() if v['extract'] is None}, {'descent','descent2'})
        for title in titles:
            plan = plans[title['id']]
            self.assertEqual(plan['target'], 'C:\\GAME\\' + title['target'])
            self.assertTrue(plan['sources'])
            case = games.installation_case(title, plan)
            acceptance.validate_case(case)
            self.assertEqual(case['pending']['requires'], [])
            self.assertTrue(all(step['status'] == 'needs-lead-calibration' and step['screen'] for step in plan['steps']))
            self.assertTrue(all('actions' not in step for step in case['steps']))
            self.assertIn('sound-setup', [step.get('name') for step in case['steps']])
            session = acceptance.Session(case, '12345678', SCRATCH)
            with self.assertRaisesRegex(EvidenceNotRun, 'needs-lead-calibration') as error: session.precheck()
            self.assertEqual(acceptance.verdict([{'case': case['id'], 'outcome': 'not_run', 'reason': str(error.exception)}])[0], 'INCOMPLETE')
            self.assertIn(b'\\GAME\\' + title['target'].encode(), games.batch(title, 'single'))
            self.assertIn(b'\\GAME\\' + title['target'].encode(), games.shortcut(title, 'single'))

    def test_invalid_install_plans_fail_closed(self):
        data = json.loads((ROOT / games.INSTALL_PLANS).read_text())
        titles, _ = games.load()
        for mutate in (lambda p: p.update(target='C:\\OTHER'),
                       lambda p: p['steps'][1].update(status='approved'),
                       lambda p: p['sources'][0].update(sha256='bad'),
                       lambda p: p['comparison']['config'].append('DOOM.EXE'),
                       lambda p: p['comparison']['config'].append('*.CFG'),
                       lambda p: p['steps'].pop(),
                       lambda p: p['extract'].update(parts=['../X', 'PART.2'])):
            bad = copy.deepcopy(data); mutate(bad['titles'][0])
            with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
                path = Path(tmp) / games.INSTALL_PLANS; path.parent.mkdir(parents=True)
                path.write_text(json.dumps(bad))
                with self.assertRaises(ValueError): games.installation_plans(tmp, titles)

    def test_write_cases_preserves_unrelated_bytes_order_and_is_idempotent(self):
        titles, _ = games.load()
        first = games.acceptance_case(titles[0])
        first['title'] = 'outdated generated title'
        unrelated = ' {"id": "unrelated", "steps": [{"text": "retain inline format"}]}'
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            path = Path(tmp) / 'suite.json'
            path.write_text('{"cases": [\n' + unrelated + ',\n' + json.dumps(first) + '\n]}\n')
            games.write_cases(path, titles)
            once = path.read_text()
            self.assertIn(unrelated, once)
            cases = json.loads(once)['cases']
            self.assertEqual(cases[0]['id'], 'unrelated')
            self.assertEqual(cases[1], games.acceptance_case(titles[0]))
            self.assertEqual(len({c['id'] for c in cases}), len(cases))
            games.write_cases(path, titles)
            self.assertEqual(path.read_text(), once)


if __name__ == '__main__':
    unittest.main()
