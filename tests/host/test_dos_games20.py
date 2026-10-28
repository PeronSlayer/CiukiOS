"""Host-only DOS game catalog, media pins and paired-run failure controls.

Every payload/serial/frame below is synthetic. No game, guest, daemon or image
build is executed; real mtools tests use small throwaway FAT volumes only.
"""
import copy
from contextlib import contextmanager
import hashlib
import io
import json
from pathlib import Path
import shutil
import struct
import re
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/test'))
import acceptance
import dos_games as games
import dos_titles
import lan_runner
import resources as res
import run as runner
from evidence import EvidenceError, EvidenceNotRun
from tests.host.test_acceptance import FakeQMP, ppm

SCRATCH = ROOT / 'build/host/games20'
SCRATCH.mkdir(parents=True, exist_ok=True)


def catalog_root(folder):
    """Copy small manifests, never game bytes or existing installed trees."""
    folder = Path(folder)
    for path in (games.CATALOG, games.PINS):
        dest = folder / path; dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes((ROOT / path).read_bytes())
    pins = json.loads((ROOT / games.PINS).read_text())['titles']
    for pin in pins:
        source = pin['licence']['source']
        if not source.startswith('third_party/'):
            candidate = 'third_party/games/' + pin['id'] + '-shareware/UPSTREAM.json'
            if (ROOT / candidate).is_file(): source = candidate
        if source.startswith('third_party/'):
            dest = folder / source; dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes((ROOT / source).read_bytes())
    return folder


def save_title(root, title):
    path = root / games.CATALOG; data = json.loads(path.read_text())
    data['titles'] = [title if t['id'] == title['id'] else t for t in data['titles']]
    path.write_text(json.dumps(data))


def ready_title(root, ident='heretic', image_sha='a' * 64):
    title, pin = games.title_for(ident, root)
    cache = root / games.CACHE; cache.mkdir(parents=True, exist_ok=True)
    archive = b'Synthetic archive, never a game'; (cache / pin['archive']).write_bytes(archive)
    pin.update(status='pinned', sha256=hashlib.sha256(archive).hexdigest(), size=len(archive))
    data = json.loads((root / games.PINS).read_text())
    data['titles'] = [pin if p['id'] == ident else p for p in data['titles']]
    (root / games.PINS).write_text(json.dumps(data))
    path = root / pin['licence']['source']; upstream = json.loads(path.read_text())
    upstream.update(status='pinned', sha256=pin['sha256'], size=pin['size']); path.write_text(json.dumps(upstream))
    directory = root / title['installed']['directory']; directory.mkdir()
    executable = b'MZ synthetic fixture'; (directory / title['executable']).write_bytes(executable)
    title['installed']['files'] = [{'path': title['executable'], 'size': len(executable), 'sha256': hashlib.sha256(executable).hexdigest()}]
    title['terms_reviewed'] = True
    frame = {'region': [0, 0, 4, 4], 'sha256': hashlib.sha256(bytes((10, 20, 30)) * 16).hexdigest()}
    title['calibration'] = {'image_sha256': image_sha, 'profile': 'qemu-t23', 'reviewer': 'synthetic host fixture',
                            'single': frame, 'host': frame, 'join': frame}
    if title['lan'] is not None:
        title['lan'] = {'host_args': '-nodes 2', 'join_args': '-nodes 2', 'host_input': ['ret'], 'join_input': ['ret'], 'host_executable': title['executable'], 'join_executable': title['executable']}
    save_title(root, title)
    return title, pin


class CatalogTests(unittest.TestCase):
    def test_twenty_original_editions_coverage_and_static_case_wiring(self):
        titles, pins = games.load()
        tracked = dos_titles.load(ROOT / 'tests/dos-titles/tracked.json', True)['titles']
        self.assertEqual({t['id'] for t in tracked}, {t['id'] for t in titles})
        self.assertEqual(len(titles), 20)
        self.assertEqual({p['status'] for p in pins['titles']} - {'pinned', 'lead-pins'}, set())
        for pin in pins['titles'][6:]:
            self.assertEqual(pin['licence']['class'], 'test-media')
            # The lead pins each candidate with dos_games.py fetch --lead-pin.
            if pin['status'] == 'lead-pins':self.assertEqual(pin['sha256'], 'lead-pins')
            else:self.assertRegex(pin['sha256'], '^[0-9a-f]{64}$'); self.assertGreater(pin['size'], 0)
            self.assertNotIn('package', pin)  # canonical bundling path cannot ingest candidates
        self.assertTrue(all(1990 <= t['year'] <= 1998 for t in tracked))
        self.assertGreaterEqual(sum(t['lan'] is not None for t in titles), 6)
        self.assertTrue({'mode13h', 'modex', 'vesa'} <= {v for t in titles for v in t['coverage_targets']['video']})
        self.assertTrue({'general-midi', 'opl', 'digital'} <= {t['music'] for t in titles})
        suite = json.loads((ROOT / 'tests/suites/acc-t23.json').read_text())
        actual = [c for c in suite['cases'] if 'dos_game' in c or 'guests' in c]
        self.assertEqual(actual, games.acceptance_cases(titles))
        for case in actual:
            self.assertEqual(runner.case_kind(case), 'acceptance')
        listed = subprocess.run(['bash', 'scripts/test/acceptance.sh', '--list'], cwd=ROOT, capture_output=True, text=True, check=True)
        self.assertEqual({line.split()[0] for line in listed.stdout.splitlines() if not line.endswith(' cases valid')}, {c['id'] for c in suite['cases']})

    def test_downgraded_predicates_and_malformed_pairs_are_refused(self):
        title = games.load()[0][0]
        for change in ('sound', 'fullscreen', 'cleanup', 'gameplay', 'exit'):
            case = games.acceptance_case(title)
            case['steps'] = [s for s in case['steps'] if s.get('check') != change]
            with self.subTest(change=change), self.assertRaises(res.Refusal):
                acceptance.validate_case(case)
        pair = next(c for c in games.acceptance_cases([title]) if 'guests' in c)
        for count in (0, 1, 3, 4):
            bad = copy.deepcopy(pair); bad['guests'] = [bad['guests'][0]] * count
            with self.assertRaises(res.Refusal): acceptance.validate_case(bad)
        bad = copy.deepcopy(pair); bad['guests'][1] = bad['guests'][0]
        with self.assertRaises(res.Refusal): acceptance.validate_case(bad)

    def test_manifest_corruption_upstream_mismatch_and_paths(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            root = catalog_root(tmp); title, pin = games.title_for('heretic', root)
            title['installed']['files'] = [{'path': '../ESCAPE.EXE', 'sha256': 'a' * 64, 'size': 1}]
            save_title(root, title)
            with self.assertRaises(ValueError): games.load(root)
            root = catalog_root(tmp)
            path = root / pin['licence']['source']; upstream = json.loads(path.read_text()); upstream['sha256'] = 'f' * 64
            path.write_text(json.dumps(upstream))
            with self.assertRaises(ValueError): games.load(root)

    def test_existing_archive_name_is_checked_against_installed_provenance(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            root = catalog_root(tmp)
            path = root / games.PINS; pins = json.loads(path.read_text())
            pin = next(p for p in pins['titles'] if p['id'] == 'duke3d')
            self.assertEqual(pin['archive'], '3dduke13.zip')
            pin['archive'] = 'wrong.zip'
            pin['url'] = pin['url'].rsplit('/', 1)[0] + '/wrong.zip'
            path.write_text(json.dumps(pins))
            with self.assertRaisesRegex(ValueError, 'installed package pin mismatch'): games.load(root)

    def test_pending_archive_terms_tree_and_calibration_do_not_pass(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            root = catalog_root(tmp)
            spec = {'title': 'heretic', 'role': 'single'}
            with self.assertRaisesRegex(EvidenceNotRun, 'archive_pin_pending'): games.precheck(spec, root)
            title, pin = ready_title(root)
            self.assertEqual(games.precheck(spec, root, 'a' * 64)[0]['id'], 'heretic')
            with self.assertRaisesRegex(ValueError, 'image mismatch'): games.precheck(spec, root, 'b' * 64)
            for field, value, message in [('terms_reviewed', False, 'terms_review'), ('installed', {**title['installed'], 'files': []}, 'tree_pending'), ('calibration', None, 'calibration_pending')]:
                bad = copy.deepcopy(title); bad[field] = value; save_title(root, bad)
                with self.assertRaisesRegex(EvidenceNotRun, message): games.precheck(spec, root)
                save_title(root, title)
            source = root / title['installed']['directory'] / title['executable']; source.write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError, 'installed pin mismatch'): games.precheck(spec, root)

    def test_symlinked_payload_and_oversized_installed_tree_refused(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            root = catalog_root(tmp); title, pin = ready_title(root)
            source = root / title['installed']['directory'] / title['executable']
            source.unlink(); source.symlink_to(ROOT / 'AGENTS.md')
            with self.assertRaisesRegex(ValueError, 'escape'): games.payload(title, pin, root)
            files = [{'path': 'BIG.EXE', 'size': games.MAX_BYTES + 1, 'sha256': 'a' * 64}]
            with self.assertRaises(ValueError): games.validate_files(files)

    def test_explicit_fetch_pinning_and_bad_download_never_replaces_cache(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            root = catalog_root(tmp); title, pin = games.title_for('heretic', root)
            with patch.object(games.urllib.request, 'urlopen', return_value=io.BytesIO(b'archive')) as opened:
                with self.assertRaises(EvidenceNotRun): games.fetch(title, pin, root)
                opened.assert_not_called()
                result = games.fetch(title, pin, root, lead_pin=True)
                request = opened.call_args.args[0]
                self.assertEqual((request.full_url, request.get_header('User-agent')), (pin['url'], 'CiukiOS-dos-games/1'))
            self.assertEqual(result['sha256'], hashlib.sha256(b'archive').hexdigest())
            title, pin = games.title_for('heretic', root)
            with patch.object(games.urllib.request, 'urlopen', return_value=io.BytesIO(b'bad')):
                with self.assertRaisesRegex(ValueError, 'pin mismatch'): games.fetch(title, pin, root)
            cache = root / games.CACHE
            self.assertEqual((cache / pin['archive']).read_bytes(), b'archive')
            self.assertFalse(list(cache.glob('*.download')))
            self.assertFalse(title['terms_reviewed'])

    def test_shortcut_uses_real_codec_and_network_option(self):
        from win32_inventory import link_target
        title = games.load()[0][0]
        # Independent decoder from the installer inventory reads CIUKLNK3.
        self.assertEqual(link_target(games.shortcut(title, 'single')), '/Programs/dosbox')
        self.assertNotIn(b'--network', games.shortcut(title, 'single'))
        self.assertIn(b'--network=ipx', games.shortcut(title, 'join'))
        self.assertIn(b'G20RUN.BAT', games.shortcut(title, 'single'))
        self.assertNotIn(b'IPX', games.batch(title, 'single'))


class FrameTests(unittest.TestCase):
    def test_mouse_actions_use_balanced_real_qmp_events(self):
        action = {'mouse': {'dx': 40, 'dy': -12, 'button': 'left'}, 'hold': 0.01}
        acceptance.validate_action(action, 'mouse fixture')
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            session = acceptance.Session(games.acceptance_case(games.load()[0][0]), '12345678', Path(tmp))
            session.launched = 0
            class InputQMP:
                def __init__(self): self.inputs = []
                def input_events(self, events): self.inputs.append(events)
            qmp = InputQMP()
            session._perform(session._expand([action])[0], qmp, 0)
            self.assertEqual(qmp.inputs, [
                [{'type': 'rel', 'data': {'axis': 'x', 'value': 40}}, {'type': 'rel', 'data': {'axis': 'y', 'value': -12}}],
                [{'type': 'btn', 'data': {'button': 'left', 'down': True}}],
                [{'type': 'btn', 'data': {'button': 'left', 'down': False}}]])
        for value in ({'dx': True}, {'dx': 32768}, {'button': 'wheel-up'}, {'absolute': 1}):
            with self.assertRaises(res.Refusal): acceptance.validate_action({'mouse': value}, 'bad mouse')

    def test_calibration_is_applied_and_a_wrong_frame_fails(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            root = catalog_root(tmp); title, _ = ready_title(root)
            case = games.acceptance_case(title)
            session = acceptance.Session(case, '12345678', root, root, read_ppm=runner.read_ppm)
            session.precheck()
            step = next(s for s in session.steps if s['name'] == 'gameplay')
            self.assertIn('sha256', step['screen'])
            session.index = session.steps.index(step)
            session._check_screen(step, FakeQMP([ppm((10, 20, 30), 256, 300)]))
            with self.assertRaisesRegex(acceptance.StepFailure, 'does not match'):
                session._check_screen(step, FakeQMP([ppm((1, 2, 3), 256, 300)]))
            step['screen']['region'] = [254, 0, 4, 4]
            with self.assertRaisesRegex(acceptance.StepFailure, 'outside screen'):
                session._check_screen(step, FakeQMP([ppm((10, 20, 30), 256, 300)]))

    def test_sound_pattern_rejects_detection_and_wrong_vm(self):
        title = next(t for t in games.load()[0] if t['id'] == 'keen4')
        pattern = next(s['wait'] for s in games.acceptance_case(title)['steps'] if s.get('name') == 'opl')
        compiled = re.compile(pattern.replace('<<game_vm>>', '2').replace('<<game_gen>>', '9'))
        self.assertTrue(compiled.fullmatch('[vm-audio] vm=2 gen=9 opl_writes=50 key_writes=3'))
        self.assertFalse(compiled.fullmatch('[vm-audio] vm=2 gen=8 opl_writes=50 key_writes=3'))
        self.assertFalse(compiled.fullmatch('[vm-audio] vm=2 gen=9 opl_writes=50 key_writes=0'))
        title = next(t for t in games.load()[0] if t['id'] == 'doom')
        pattern = next(s['wait'] for s in games.acceptance_case(title)['steps'] if s.get('name') == 'digital')
        compiled = re.compile(pattern.replace('<<game_vm>>', '2'))
        # play_dma is the 8237 channel, not the PCM bit depth.
        for channel in (1, 5):
            self.assertTrue(compiled.fullmatch('[vm-audio] vm=2 play_dma=%d hz=11025 units=511' % channel))
        self.assertFalse(compiled.fullmatch('[vm-audio] vm=2 play_dma=8 hz=11025 units=511'))


class PairTests(unittest.TestCase):
    def pair(self):
        return next(c for c in games.acceptance_cases(games.load()[0]) if c['id'] == 'acc-ipx-heretic')

    def test_network_is_one_isolated_bus_with_distinct_identities(self):
        pair = self.pair(); networks = lan_runner.pair_networks()
        self.assertEqual(networks[0]['group'], networks[1]['group'])
        self.assertEqual(networks[0]['port'], networks[1]['port'])
        self.assertNotEqual(networks[0]['mac'], networks[1]['mac'])
        self.assertNotEqual(networks[0]['identity'], networks[1]['identity'])
        for guest, network in zip(pair['guests'], networks):
            lan_runner.validate_network(network)
            args = acceptance.qemu_args({**guest, '_lan': network})
            self.assertTrue(any(a.startswith('socket,id=ciuknet0,mcast=239.255.') for a in args))
            self.assertFalse(any(a.startswith('user,') for a in args))
        for key, value in [('group', '127.0.0.1'), ('port', True), ('identity', '0' * 32), ('mac', '52:54:00:00:00:01,hostfwd=bad')]:
            with self.assertRaises(res.Refusal): lan_runner.validate_network({**networks[0], key: value})

    def run_pair(self, root, boot):
        root = catalog_root(root)
        image = root / 'canonical.raw'; image.write_bytes(b'canonical fixture')
        host = type('Host', (), {'preflight': lambda self, p: 8 * res.GIB})()
        with patch.object(games, 'precheck', return_value=None):
            return lan_runner.run_pair(root, 'fixture', self.pair(), {}, image, 'fake', 'firmware', host, False, 'unused', boot)

    def test_failure_cancels_peer_and_waits_for_both_cleanups(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            root = Path(tmp); entered = threading.Event(); cleaned = []
            def boot(*args, cancel_event=None, **kwargs):
                role = args[2]['dos_game']['role']
                if role == 'host':
                    self.assertTrue(entered.wait(5))
                else:
                    entered.set(); self.assertTrue(cancel_event.wait(5))
                cleaned.append(role)
                return {'outcome': 'fail', 'cleanup': {'clean': True}}, root
            result, directory = self.run_pair(root, boot)
            self.assertEqual(result['outcome'], 'fail')
            self.assertEqual(set(cleaned), {'host', 'join'})
            self.assertTrue(result['cleanup']['clean'])
            self.assertEqual(json.loads((directory / 'result.json').read_text())['outcome'], 'fail')

    def test_exception_also_cancels_peer(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            root = Path(tmp); entered = threading.Event(); cleaned = []
            def boot(*args, cancel_event=None, **kwargs):
                role = args[2]['dos_game']['role']
                if role == 'host':
                    self.assertTrue(entered.wait(5)); raise OSError('synthetic launch failure')
                entered.set(); self.assertTrue(cancel_event.wait(5)); cleaned.append(role)
                return {'outcome': 'fail', 'cleanup': {'clean': True}}, root
            result, _ = self.run_pair(root, boot)
            self.assertEqual(result['outcome'], 'fail'); self.assertEqual(cleaned, ['join'])
            self.assertIn('synthetic launch failure', result['reason'])
            self.assertFalse(result['cleanup']['clean'])  # thrown child's ownership is not fabricated

    def test_both_serial_logs_must_prove_cross_peer_traffic(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            root = Path(tmp)
            def boot(*args, cancel_event=None, **kwargs):
                role = args[2]['dos_game']['role']; folder = root / role; folder.mkdir()
                me, peer = ('020000000001', '020000000002') if role == 'host' else ('020000000002', '020000000001')
                (folder / 'serial.log').write_text('[dos-ipx] vm=1 gen=2 event=tx count=1 src=%s dst=ffffffffffff\n[dos-ipx] vm=1 gen=2 event=rx count=1 bytes=80 src=%s dst=ffffffffffff\n' % (me, peer))
                return {'outcome': 'pass', 'cleanup': {'clean': True}, 'acceptance': {'variables': {'game_vm': '1', 'game_gen': '2'}}}, folder
            result, _ = self.run_pair(root, boot)
            self.assertEqual(result['outcome'], 'pass')
            path = root / 'join/serial.log'; path.write_text(path.read_text().replace('src=020000000001', 'src=020000000002'))
            with self.assertRaises(EvidenceError): lan_runner.cross_peer(result['guests'])

    def test_pending_media_is_not_run_without_starting_any_guest(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            root = catalog_root(tmp); image = root / 'image'; image.write_bytes(b'canonical')
            with patch.object(games, 'precheck', side_effect=EvidenceNotRun('lead-pins')), patch.object(runner.Host, 'launch') as launch:
                result, _ = lan_runner.run_pair(root, 'fixture', self.pair(), {}, image, 'unused', 'unused', runner.Host(), False, 'unused', lambda *a: self.fail('guest started'))
            self.assertEqual(result['outcome'], 'not_run'); self.assertTrue(result['cleanup']['clean']); launch.assert_not_called()


@unittest.skipUnless(shutil.which('mkfs.fat') and shutil.which('mcopy') and shutil.which('fsck.fat'), 'mtools/dosfstools required')
class StagingTests(unittest.TestCase):
    def test_nested_extent_follows_fat_and_refuses_fragmentation(self):
        from win32_inventory import FatVolume
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            root = Path(tmp); volume = root / 'toy.fat'
            with volume.open('wb') as stream: stream.truncate(16 * 1024**2)
            subprocess.run(['mkfs.fat', '--invariant', '-F', '16', str(volume)], check=True, stdout=subprocess.DEVNULL)
            nested = bytearray(16384); nested[450] = 6; nested[510:512] = b'\x55\xaa'
            struct.pack_into('<II', nested, 454, 1, 31)
            games.copy_files(volume, {'/SYSTEM/DOS/UNIFIED.IMG': bytes(nested)}, root)
            offset, size = games.nested_extent(volume)
            self.assertEqual(size, 31 * 512)
            with volume.open('rb') as stream:
                stream.seek(offset - 512); self.assertEqual(stream.read(512), nested[:512])
            with volume.open('r+b') as stream:
                fat = FatVolume(stream)
                first = next(e[2] for e in fat.entries(0) if e[0] == 'SYSTEM')
                first = next(e[2] for e in fat.entries(first) if e[0] == 'DOS')
                first = next(e[2] for e in fat.entries(first) if e[0] == 'UNIFIED.IMG')
                chain = fat.chain(first); self.assertGreaterEqual(len(chain), 4)
                for cluster, following in ((chain[0], chain[2]), (chain[2], chain[1]), (chain[1], chain[3])):
                    for table in range(fat.fats):
                        stream.seek(fat.reserved * fat.sector + table * len(fat.fat) + cluster * 2)
                        stream.write(struct.pack('<H', following))
            with self.assertRaisesRegex(ValueError, 'contiguous'): games.nested_extent(volume)

    def test_mtools_stages_shortcut_and_dos_tree_with_byte_readback(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            root = Path(tmp); volume = root / 'toy.fat'
            with volume.open('wb') as stream: stream.truncate(16 * 1024**2)
            subprocess.run(['mkfs.fat', '--invariant', '-F', '16', str(volume)], check=True, stdout=subprocess.DEVNULL)
            title = games.load()[0][0]
            records = games.copy_files(volume, {'/DOOM/G20RUN.BAT': games.batch(title, 'single'),
                        '/Windows/Start Menu/Programs/Games/Trial.lnk': games.shortcut(title, 'single')}, root)
            self.assertEqual(len(records), 2)
            subprocess.run(['fsck.fat', '-n', str(volume)], check=True, stdout=subprocess.DEVNULL)
            data = subprocess.check_output(['mtype', '-i', str(volume), '::/Windows/Start Menu/Programs/Games/Trial.lnk'])
            self.assertEqual(data, games.shortcut(title, 'single'))

    def test_export_rejects_canonical_paths_before_a_daemon(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            with patch.object(runner.subprocess, 'Popen') as popen:
                with self.assertRaises(res.Refusal):
                    with runner.Host().export_writable(ROOT / 'build/f0/ciukios.img', Path(tmp), 512, 512): pass
                popen.assert_not_called()


if __name__ == '__main__': unittest.main()
