"""f3-53 host evidence only. Synthetic DOS material is never executed."""
import hashlib
import copy
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/test'))
import dos_titles as titles
import dos_fixtures
import probe_tables
from loader_model import selector
from evidence import EvidenceError, EvidenceNotRun

FIXTURES = ROOT / 'tests/dos-titles/fixtures'


def sample():
    return json.loads((FIXTURES / 'synthetic.json').read_text())['titles'][0]


def installer_sample():
    t = sample()
    t['install']['automation'] = [{'directory': 'SYNTH', 'executable': 'SETUP.EXE', 'args': [],
        'timeout_ms': 5000, 'tree_sha256': None,
        'input': [{'at_ms': 100, 'code': 28, 'down': True}, {'at_ms': 180, 'code': 28, 'down': False}]}]
    return t


class SchemaTests(unittest.TestCase):
    def test_licence_boundaries_and_quake_metadata(self):
        rows = titles.load(ROOT / 'tests/dos-titles/tracked.json', True)['titles']
        pins = json.loads((ROOT / 'config/dos-titles.json').read_text())['titles']
        self.assertEqual({t['id'] for t in rows}, {p['id'] for p in pins})
        for t in rows:
            expected = 'bundled' if t['id'] in ('doom', 'quake') else 'test-media'
            self.assertEqual(t['licence']['class'], expected)
            self.assertEqual(next(p for p in pins if p['id'] == t['id'])['licence']['class'], expected)
            self.assertNotIn('verified', t['licence'])
        quake = next(t for t in rows if t['id'] == 'quake')
        self.assertEqual((quake['audio']['expectation'], quake['memory']['dpmi'], quake['memory']['dpmi_fallback']), ('digital', 'native', 'cwsdpmi'))
        for extra in ({'kind': 'redistributable'}, {'verified': True}):
            t = sample(); t['licence'].update(extra)
            with self.assertRaises(ValueError): titles.validate({'schema_version': 1, 'titles': [t]})
    def test_six_proposed_profiles_and_pending_readiness(self):
        data = titles.load(ROOT / 'tests/dos-titles/tracked.json', True)
        self.assertGreaterEqual(len(data['titles']), 6)
        pins = json.loads((ROOT / 'config/dos-titles.json').read_text())
        for t in data['titles']:
            with self.assertRaises(EvidenceNotRun): titles.ready(t, pins)

    def test_invalid_schema_and_paths(self):
        changes = [('id', '../x'), ('id', 'A'), ('year', True), ('lan', 'modem'),
                   ('executable', '../GAME.EXE'), ('args', ['>C:\\FILE']), ('input', []), ('extra', 1)]
        for key, value in changes:
            t = sample();t[key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                titles.validate({'schema_version': 1, 'titles': [t]})
        t = sample()
        with self.assertRaises(ValueError): titles.validate({'schema_version': 1, 'titles': [t, t]})
        t['licence']['class'] = 'owner-copy'
        with self.assertRaises(ValueError): titles.validate({'schema_version': 1, 'titles': [t]}, True)
        t = sample();t['input'][0]['down'] = False
        with self.assertRaises(ValueError): titles.validate({'schema_version': 1, 'titles': [t]})
        t = sample();t['checkpoints'][0]['value'] = 'no hash'
        with self.assertRaises(ValueError): titles.validate({'schema_version': 1, 'titles': [t]})

    def test_no_implicit_private_read(self):
        with patch.object(titles, 'load', wraps=titles.load) as loader:
            titles.manifests()
        self.assertEqual(len(loader.call_args_list), 1)
        self.assertEqual(loader.call_args.args[0], ROOT / 'tests/dos-titles/tracked.json')

    def test_tables_and_selector_suffix(self):
        self.assertEqual(probe_tables.missing(3, 'title'), [])
        self.assertEqual(selector('f3:title run=12345678 title=synthetic')['title'], 'synthetic')
        for suffix in ('', ' title=', ' title=../a', ' title=x title=y', ' title=x safe=1'):
            with self.assertRaises(ValueError): selector('f3:title run=12345678' + suffix)
        source = (ROOT / 'src/kernel/probes/f3_probes_titles.c').read_text()
        self.assertIn('CIUKI_F3_PROBE("title",title_run)', source)
        self.assertNotIn('panic(', source)


class SerialTests(unittest.TestCase):
    def setUp(self):
        self.raw = (FIXTURES / 'serial.txt').read_bytes()

    def parse(self, raw=None, title=None):
        return titles.parse_serial(self.raw if raw is None else raw, '12345678', title or sample())

    def test_framed_fixture(self):
        r = self.parse()
        self.assertEqual((r['menu_ms'], r['gameplay_ms'], r['lan']), (5000, 30000, 'pending f6-11'))
        self.assertEqual(len(r['cadence_diagnostics']), 1)

    def test_maximum_record_envelopes(self):
        # Production formats, longest legal ID, UINT32 seq/metrics and UINT64
        # counters: ensure no producer can trigger record_overflow at 240.
        import re
        source = (ROOT / 'src/kernel/probes/f3_probes_titles.c').read_text()
        formats = re.findall(r'rec_emit\("title","DATA","(group=dos_title[^"\n]+)"', source)
        self.assertEqual(len(formats), 14)
        for fmt in formats:
            strings = iter(['x' * 23, 'a' * 64])
            def value(match):
                if match[0] == '%s': return next(strings, 'a' * 64)
                return str(2**64 - 1) if match[0] == '%llu' else str(2**32 - 1)
            # Checkpoint case names have a separate bounded string argument.
            if 'case=%s' in fmt: strings = iter(['x' * 23, 'gameplay', 'a' * 64])
            line = 'CIUKI_TEST v=1 run=ffffffff seq=4294967295 probe=title event=DATA ' + re.sub(r'%llu|%[us]', value, fmt)
            self.assertLessEqual(len(line), 240, line)

    def test_music_requires_activity_across_gameplay(self):
        for before, after in ((b'key_writes=30', b'key_writes=2'), (b'windows=7', b'windows=3')):
            with self.assertRaises(EvidenceError): self.parse(self.raw.replace(before, after))

    def test_subset_does_not_qualify_the_twenty_title_goal(self):
        out = ROOT / 'build/host';out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=out, prefix='title-summary-') as temp:
            titles.write_summary(temp, [self.parse()])
            data = json.loads((Path(temp) / 'summary.json').read_text())
            self.assertEqual(data['qualified_title_count'], 1)
            self.assertFalse(data['twenty_title_goal_met'])
            self.assertIn('IRQ max us', (Path(temp) / 'summary.md').read_text())

    def test_missing_duplicate_partial_stale_and_fault(self):
        lines = self.raw.splitlines(keepends=True)
        variants = [b''.join(lines[:5] + lines[6:]), self.raw + lines[-1], self.raw[:-1],
                    self.raw.replace(b'run=12345678', b'run=87654321'),
                    self.raw.replace(b'title=synthetic', b'title=other'),
                    self.raw + b'Ciuki VMM panic fault\n']
        for raw in variants:
            with self.subTest(raw=raw[:50]), self.assertRaises(EvidenceError): self.parse(raw)

    def test_disk_only_diagnostic_sequence_gaps_are_allowed(self):
        # output.c deliberately shares its sequence with non-UART records.
        lines = []
        for line in self.raw.splitlines():
            if line.startswith(b'CIUKI_TEST '):
                import re
                line = re.sub(rb'seq=([0-9]+)', lambda m: b'seq=' + ('%06d' % (int(m[1]) * 2)).encode(), line)
            lines.append(line)
        self.assertEqual(self.parse(b'\n'.join(lines) + b'\n')['outcome'], 'pass')

    def test_failure_thresholds_and_unproved_success(self):
        replacements = [(b'underruns=0', b'underruns=1'), (b'irq_latency_max_us=20000', b'irq_latency_max_us=20001'),
                        (b'duration_ms=30000', b'duration_ms=29999'), (b'frame_changes=20', b'frame_changes=0'),
                        (b'attached=1', b'attached=0'), (b'irq=5', b'irq=7'), (b'key_writes=30', b'key_writes=0'),
                        (b'delivered=4', b'delivered=3'), (b'clean=1', b'clean=0'), (b'restored=1', b'restored=0'),
                        (b'faults=0', b'faults=1'), (b'case=menu ms=5000', b'case=menu ms=60001')]
        for before, after in replacements:
            self.assertIn(before, self.raw)
            with self.subTest(after=after), self.assertRaises(EvidenceError): self.parse(self.raw.replace(before, after))
        t = sample();t['checkpoints'][0]['value'] = 'b' * 64
        with self.assertRaises(EvidenceError): self.parse(title=t)


class ModelTests(unittest.TestCase):
    def model(self, sanitizer):
        out = ROOT / 'build/host';out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=out, prefix='title-model-') as temp:
            temp = Path(temp);wire = temp / 'plan.bin';wire.write_bytes(titles.plan(sample()))
            extended = temp / 'capture.bin';extended.write_bytes(titles.plan(installer_sample(), True))
            binary = temp / ('model-' + sanitizer)
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=' + sanitizer, '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                            '-I', str(ROOT / 'src/kernel/include'), str(ROOT / 'tests/host/dos_titles_test.c'),
                            str(ROOT / 'src/kernel/probes/selector.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary), str(wire), str(extended)], check=True)

    def test_production_model_ubsan(self):
        self.model('undefined')

    def test_production_model_asan(self):
        self.model('address,undefined')

    def test_actual_audio_counters_ubsan(self):
        out = ROOT / 'build/host';out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=out, prefix='title-audio-') as temp:
            binary = Path(temp) / 'audio'
            subprocess.run(['clang', '-std=c17', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-parameter', '-fsanitize=undefined', '-ffunction-sections', '-fdata-sections',
                            '-Wl,--gc-sections', '-I', str(ROOT / 'src/kernel/include'),
                            str(ROOT / 'tests/host/dos_title_audio_test.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_production_installer_and_tree_ubsan(self):
        out = ROOT / 'build/host';out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=out, prefix='title-install-') as temp:
            binary = Path(temp) / 'install'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                            '-I', str(ROOT / 'src/kernel/include'), str(ROOT / 'tests/host/dos_title_install_test.c'),
                            str(ROOT / 'src/kernel/lib/sha256.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary), titles.tree_hash({'Z.TXT': b'last', 'DIR/A.DAT': b'first'})], check=True)

    def test_target_controller_compile(self):
        out = ROOT / 'build/host';out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=out, prefix='title-target-') as temp:
            temp = Path(temp)
            subprocess.run(['clang', '--target=i686-unknown-elf', '-march=pentiumpro', '-ffreestanding',
                            '-fno-builtin', '-mno-sse', '-mno-sse2', '-mno-80387', '-std=c17', '-O2',
                            '-Wall', '-Wextra', '-Werror', '-Wframe-larger-than=2048',
                            '-I', str(ROOT / 'src/kernel/include'), '-c',
                            str(ROOT / 'src/kernel/probes/f3_probes_titles.c'), '-o', str(temp / 'controller.o')], check=True)
            subprocess.run(['clang', '--target=i686-unknown-elf', '-march=pentiumpro', '-ffreestanding',
                            '-fno-builtin', '-mno-sse', '-mno-sse2', '-mno-80387', '-std=c17', '-O2',
                            '-Wall', '-Wextra', '-Werror', '-Wframe-larger-than=2048',
                            '-I', str(ROOT / 'src/kernel/include'), '-c',
                            str(ROOT / 'src/kernel/vm/dosvm_vga.c'), '-o', str(temp / 'presenter.o')], check=True)


class DiskTests(unittest.TestCase):
    def test_synthetic_disk_layout_and_readback(self):
        for tool in ('mkfs.fat', 'fsck.fat', 'mcopy'):
            self.assertIsNotNone(shutil.which(tool), tool + ' required for layout evidence')
        out = ROOT / 'build/host';out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=out, prefix='title-disk-') as temp:
            temp = Path(temp);source = temp / 'source';source.mkdir()
            binary = b'MZ synthetic fixture, never executed\r\n';(source / 'GAME.EXE').write_bytes(binary)
            t = sample();t['install']['source'] = str(source)
            t['install']['files'] = [{'path': 'GAME.EXE', 'sha256': hashlib.sha256(binary).hexdigest()}]
            dos = dos_fixtures._build_dos();boot = bytearray(512)
            boot[:3] = b'\xeb\x3c\x90';boot[3:11] = b'CIUKIOS '
            struct.pack_into('<HBHBHHBHHHII', boot, 11, 512, dos.SPC, dos.RESERVED, 2, 512, 0, 0xf8, dos.FAT_SECTORS, 32, 64, 0, dos.SECTORS)
            boot[36] = 0x80;boot[38] = 0x29;boot[43:54] = b'CIUKIOSFULL';boot[54:62] = b'FAT16   ';boot[510:] = b'\x55\xaa'
            system = {'/RUNREC.COM': b'synthetic helper never executed'}
            image = temp / 'DOSBOOT.IMG'
            dos.write_fat16(image, boot, b'', system)
            image.with_suffix('.json').write_text(json.dumps({'image_sha256': dos_fixtures.sha(image), 'payloads': [
                {'path': p, 'size': len(d), 'sha256': hashlib.sha256(d).hexdigest()} for p, d in system.items()]}))
            fixture = titles.generate({'generator': titles.GENERATOR, 'title': t}, temp / 'run', dos_image=image)
            self.assertEqual(fixture['redistribution'], 'private-test-only')
            self.assertEqual(fixture['payloads'][0]['path'], '/SYNTH/GAME.EXE')
            self.assertEqual(dos_fixtures.run(['mcopy', '-i', fixture['path'], '::/DTITLE.PLN', '-']).stdout, titles.plan(t))
            self.assertFalse((temp / 'run/fixture-0.vm').exists())
            captured = titles.generate({'generator': titles.GENERATOR, 'title': t, 'calibrate': True}, temp / 'capture', dos_image=image)
            wire = dos_fixtures.run(['mcopy', '-i', captured['path'], '::/DTITLE.PLN', '-']).stdout
            self.assertEqual(wire, titles.plan(t, True))
            self.assertEqual((wire[:8], len(wire)), (b'D53PLAN2', titles.PLAN_V2_SIZE))
            captures = temp / 'captures'
            ppm = b'P6\n640 480\n255\n' + bytes(640 * 480 * 3)
            text = bytes(4000)
            for name in titles.CHECKS:
                for ext, data in (('ppm', ppm), ('txt', text)):
                    asset = temp / (name + '.' + ext); asset.write_bytes(data)
                    dos_fixtures.run(['mcopy', '-n', '-i', captured['path'], asset, '::/' + asset.name.upper()])
            titles.extract_captures(captured, captures)
            checkpoints = [{'name': name, 'digest': hashlib.sha256(bytes(640 * 480 * 4)).hexdigest()} for name in titles.CHECKS]
            self.assertEqual(len(titles.capture_artifacts(captures, checkpoints)), 6)
            badplan = temp / 'wrong-plan'; badplan.write_bytes(b'changed')
            dos_fixtures.run(['mcopy', '-o', '-i', captured['path'], badplan, '::/DTITLE.PLN'])
            with self.assertRaisesRegex(EvidenceError, 'changed original plan'):
                titles.extract_captures(captured, captures)
            private = copy.deepcopy(t); private_archive = temp / 'private.zip'
            with zipfile.ZipFile(private_archive, 'w') as z: z.writestr('GAME.EXE', binary)
            private['install'].update(layout='zip', source=str(private_archive), archive_pin={'size': private_archive.stat().st_size, 'sha256': dos_fixtures.sha(private_archive)})
            generated = titles.generate({'generator': titles.GENERATOR, 'title': private}, temp / 'private-run', dos_image=image)
            self.assertEqual(generated['payloads'], fixture['payloads'])
            t['install']['files'][0]['sha256'] = '0' * 64
            with self.assertRaises(ValueError): titles.files(t, {'titles': []})

    def test_zip_traversal_symlink_and_pin_failures(self):
        out = ROOT / 'build/host';out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=out, prefix='title-zip-') as temp:
            temp = Path(temp);archive = temp / 'game.zip';t = sample()
            t['install'].update(layout='zip', source=str(archive))
            for path in ('../ESCAPE.EXE', '/GAME.EXE', 'longfilename.exe'):
                with zipfile.ZipFile(archive, 'w') as z: z.writestr(path, b'MZ')
                pins = {'titles': [{'id': 'synthetic', 'sha256': dos_fixtures.sha(archive), 'size': archive.stat().st_size}]}
                t['install']['archive_pin'] = {k: pins['titles'][0][k] for k in ('sha256', 'size')}
                with self.subTest(path=path), self.assertRaises(ValueError): titles.files(t, pins)
            with zipfile.ZipFile(archive, 'w') as z: z.writestr('GAME.EXE', b'MZ')
            pins['titles'][0].update(sha256=dos_fixtures.sha(archive), size=archive.stat().st_size)
            t['install']['archive_pin'] = {k: pins['titles'][0][k] for k in ('sha256', 'size')}
            self.assertEqual(titles.files(t, pins), {'/SYNTH/GAME.EXE': b'MZ'})
            t['install']['archive_pin']['sha256'] = '0' * 64
            with self.assertRaises(ValueError): titles.files(t, pins)
            with zipfile.ZipFile(archive, 'w') as z:
                link = zipfile.ZipInfo('GAME.EXE');link.external_attr = 0o120777 << 16
                z.writestr(link, 'TARGET')
            pins['titles'][0].update(sha256=dos_fixtures.sha(archive), size=archive.stat().st_size)
            t['install']['archive_pin'] = {k: pins['titles'][0][k] for k in ('sha256', 'size')}
            with self.assertRaises(ValueError): titles.files(t, pins)
            with self.assertRaises(ValueError): dos_fixtures.check_output(ROOT / 'build/f0', ROOT)


class RunnerTests(unittest.TestCase):
    def test_pending_profile_never_starts_a_process_or_creates_an_overlay(self):
        import run as runner
        from unittest.mock import Mock
        out = ROOT / 'build/host';out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=out, prefix='title-runner-') as temp:
            temp = Path(temp);image = temp / 'image.raw';image.write_bytes(b'immutable synthetic canonical fixture')
            fw = temp / 'bios.bin';fw.write_bytes(b'firmware fixture')
            profile = runner.load(ROOT / 'tests/profiles/qemu-t23.json')
            t = titles.load(ROOT / 'tests/dos-titles/tracked.json', True)['titles'][0]
            host = Mock(spec=runner.Host)
            host.preflight.return_value = 3 * 1024**3
            host.unowned_qemu_count.return_value = host.own_qemu_count.return_value = 0
            with patch.object(runner, 'git_identity', return_value={'revision': 'host-fixture', 'dirty': False}), \
                    patch.object(runner.subprocess, 'run', side_effect=AssertionError('unexpected process')):
                r, directory = runner.run_case(temp, 'dos-titles', titles.case(t), profile, image, sys.executable, fw, host=host)
            self.assertEqual(r['outcome'], 'not_run', r['reason'])
            self.assertIn('checkpoint_calibration_pending', r['reason'])
            host.launch.assert_not_called()
            self.assertFalse((directory / 'run.qcow2').exists())
            self.assertEqual(image.read_bytes(), b'immutable synthetic canonical fixture')


class AmendmentTests(unittest.TestCase):
    def setUp(self):
        out = ROOT / 'build/host';out.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=out, prefix='title-amendment-')
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)

    def transcript(self, t, capture):
        """Synthetic mode/stage evidence; never claimed as title qualification."""
        raw = (FIXTURES / 'serial.txt').read_text()
        rows = []
        for line in raw.splitlines():
            for cp in t['checkpoints']:
                if 'case=' + cp['name'] + ' ' in line:
                    digest = cp['value'] if cp['value'] is not None and cp['kind'] == 'sha256' else getattr(self, 'frame_digest', 'a' * 64)
                    line = line.replace('digest=' + 'a' * 64, 'digest=' + digest)
            if 'case=plan ' in line:
                line = line[:line.index('sha256=')] + 'sha256=' + hashlib.sha256(titles.plan(t, capture)).hexdigest()
            if capture:
                for cp in t['checkpoints']:
                    if cp['value'] is None and 'case=' + cp['name'] + ' ' in line: line = line.replace('matched=1', 'matched=0')
            rows.append(line)
            if 'case=plan ' in line:
                rows.append('CIUKI_TEST v=1 run=12345678 seq=0 probe=title event=DATA group=dos_title title=synthetic case=mode calibration=' + str(int(capture)))
                for i, s in enumerate(t['install'].get('automation', [])):
                    rows.append('CIUKI_TEST v=1 run=12345678 seq=0 probe=title event=DATA group=dos_title title=synthetic case=install stage={} ms=900 delivered={} rc=0 files=2'.format(i, len(s['input'])))
                    rows.append('CIUKI_TEST v=1 run=12345678 seq=0 probe=title event=DATA group=dos_title title=synthetic case=install-tree stage={} sha256={}'.format(i, s['tree_sha256'] or 'b' * 64))
        import re
        seq = 0
        for i, line in enumerate(rows):
            if line.startswith('CIUKI_TEST '):
                seq += 1
                rows[i] = re.sub(r'seq=[0-9]+', 'seq=%06d' % seq, line)
        return ('\n'.join(rows) + '\n').encode()

    def artifacts(self, t):
        self.frame_digest = hashlib.sha256(bytes(640 * 480 * 4)).hexdigest()
        directory = self.path / 'captures'; directory.mkdir(exist_ok=True)
        for cp in t['checkpoints']:
            (directory / (cp['name'] + '.ppm')).write_bytes(b'P6\n640 480\n255\n' + bytes(640 * 480 * 3))
            (directory / (cp['name'] + '.txt')).write_bytes(bytes(4000))
            if cp['value'] is not None: cp['value'] = self.frame_digest
        return directory

    def test_supplied_archive_pins_licences_complete_trees_and_original_media(self):
        pins = json.loads((ROOT / 'config/dos-titles.json').read_text())
        rows = titles.load(ROOT / 'tests/dos-titles/tracked.json', True)['titles']
        for ident in ('rott', 'keen4', 'duke3d'):
            pin = next(p for p in pins['titles'] if p['id'] == ident)
            archive = ROOT / 'build/downloads/dos-titles' / pin['archive']
            if not archive.is_file(): self.skipTest('lead-supplied title archive absent: ' + str(archive))
            self.assertEqual((archive.stat().st_size, dos_fixtures.sha(archive)), (pin['size'], pin['sha256']))
            outer, inner = titles.unpack_installer(pin, archive)
            self.assertEqual(titles.tree_hash(inner), pin['installed_tree_sha256'])
            self.assertEqual([{'path': n, 'size': len(d), 'sha256': hashlib.sha256(d).hexdigest()} for n, d in sorted(inner.items())], pin['installed_files'])
            notice = inner['LICENSE.TXT' if ident == 'duke3d' else 'VENDOR.DOC']
            self.assertEqual(hashlib.sha256(notice).hexdigest(), pin['licence']['sha256'])
            self.assertIn(b'without charge' if ident == 'duke3d' else b'without charging', notice)
            self.assertIn(b'without modification', notice)
            t = next(t for t in rows if t['id'] == ident)
            self.assertEqual(t['licence']['class'], 'test-media')
            self.assertEqual(t['audio']['irq'], 5)
            self.assertEqual(titles.files(t, pins), {'/MEDIA/' + n: d for n, d in outer.items()})
            self.assertNotIn('/' + t['install']['target'] + '/' + t['executable'], titles.files(t, pins))
            titles.ready(t, pins, True)
            with self.assertRaises(EvidenceNotRun): titles.ready(t, pins)

    def test_installer_schema_rejects_unsafe_unbounded_and_unbalanced_stages(self):
        titles.validate({'schema_version': 1, 'titles': [installer_sample()]})
        for field, value in [('directory', '../X'), ('directory', 'ONE/TWO'), ('executable', 'A/B.EXE'),
                             ('args', ['>FILE']), ('timeout_ms', True), ('timeout_ms', 300001),
                             ('tree_sha256', 'bad'), ('input', [])]:
            t = installer_sample();t['install']['automation'][0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError): titles.validate({'schema_version': 1, 'titles': [t]})
        for change in ('duplicate', 'held', 'timeout', 'order'):
            t = installer_sample();s = t['install']['automation'][0]
            if change == 'duplicate': s['input'][1]['down'] = True
            if change == 'held': s['input'].pop()
            if change == 'timeout': s['input'][-1]['at_ms'] = s['timeout_ms']
            if change == 'order': s['input'].reverse()
            with self.subTest(change=change), self.assertRaises(ValueError): titles.validate({'schema_version': 1, 'titles': [t]})

    def test_tree_hash_independent_encoding_and_order(self):
        files = {'Z.TXT': b'last', 'DIR/A.DAT': b'first'}
        expected = hashlib.sha256(b'DIR/A.DAT\0' + struct.pack('<I', 5) + hashlib.sha256(b'first').digest() +
                                  b'Z.TXT\0' + struct.pack('<I', 4) + hashlib.sha256(b'last').digest()).hexdigest()
        self.assertEqual(titles.tree_hash(files), expected)
        self.assertEqual(titles.tree_hash(dict(reversed(list(files.items())))), expected)

    def test_private_archive_pin_schema_runtime_and_binding(self):
        t = sample(); archive = self.path / 'owner.zip'
        with zipfile.ZipFile(archive, 'w') as z: z.writestr('GAME.EXE', b'MZ host fixture')
        t['install'].update(layout='zip', source=str(archive), archive_pin={'size': archive.stat().st_size, 'sha256': dos_fixtures.sha(archive)})
        titles.validate({'schema_version': 1, 'titles': [t]})
        titles.ready(t, {'titles': []})
        self.assertEqual(titles.files(t, {'titles': []}), {'/SYNTH/GAME.EXE': b'MZ host fixture'})
        original = titles.definition_hash(t)
        t['install']['archive_pin']['sha256'] = 'f' * 64
        self.assertNotEqual(titles.definition_hash(t), original)
        with self.assertRaises(ValueError): titles.files(t, {'titles': []})
        t['install']['archive_pin']['size'] = True
        with self.assertRaises(ValueError): titles.validate({'schema_version': 1, 'titles': [t]})

    def test_calibration_cannot_replace_known_hash_or_text_expectations(self):
        t = installer_sample(); captures = self.artifacts(t)
        t['checkpoints'][1].update(kind='text', value='MAIN MENU')
        text = bytearray(4000); text[0:18:2] = b'MAIN MENU'
        (captures / 'menu.txt').write_bytes(text)
        r = titles.parse_serial(self.transcript(t, True), '12345678', t, True)
        path = self.path / 'calibration.json'
        titles.save_calibration(path, t, r, 't23', 'c' * 64, '12345678', captures)
        applied = titles.apply_calibration(path, t, 't23', 'c' * 64, False)
        self.assertEqual(applied['checkpoints'], t['checkpoints'])
        wrong = copy.deepcopy(t); wrong['checkpoints'][0]['value'] = 'b' * 64
        with self.assertRaises(EvidenceError): titles.parse_serial(self.transcript(t, True), '12345678', wrong, True)
        data = json.loads(path.read_text()); data['profiles']['t23']['checkpoints'][0]['digest'] = 'b' * 64
        path.write_text(json.dumps(data))
        with self.assertRaises(ValueError): titles.apply_calibration(path, t, 't23', 'c' * 64, False)
        # The raw text frame is also checked, independently of matched=1.
        (captures / 'menu.txt').write_bytes(bytes(4000))
        with self.assertRaises(EvidenceError): titles.save_calibration(self.path / 'other.json', t, r, 't23', 'c' * 64, '12345678', captures)

    def test_review_artifacts_required_intact_and_not_overwritten(self):
        t = installer_sample(); captures = self.artifacts(t)
        r = titles.parse_serial(self.transcript(t, True), '12345678', t, True)
        path = self.path / 'calibration.json'
        titles.save_calibration(path, t, r, 't23', 'c' * 64, '12345678', captures)
        (path.parent / 't23/12345678/boot.ppm').write_bytes(b'changed')
        with self.assertRaises(EvidenceError): titles.accept_calibration(path, t, 't23', 'c' * 64, 'lead')
        (captures / 'boot.ppm').write_bytes(b'changed')
        with self.assertRaises(EvidenceError): titles.save_calibration(path, t, r, 't23', 'c' * 64, '87654321', captures)

    def test_capture_verify_roundtrip_separate_profiles_and_stale_bindings(self):
        t = installer_sample()
        t['checkpoints'][1]['value'] = None
        captures = self.artifacts(t)
        raw = self.transcript(t, True)
        r = titles.parse_serial(raw, '12345678', t, True)
        self.assertEqual(r['outcome'], 'calibrated')
        path = titles.calibration_path(self.path / 'manifest.json', t)
        self.assertEqual(path, self.path / 'synthetic/calibration.json')
        for profile in ('qemu-t23', 't23'):
            titles.save_calibration(path, t, r, profile, 'c' * 64, '12345678', captures)
        self.assertEqual(set(json.loads(path.read_text())['profiles']), {'qemu-t23', 't23'})
        with self.assertRaisesRegex(EvidenceNotRun, 'calibration_acceptance_pending'):
            titles.apply_calibration(path, t, 't23', 'c' * 64)
        titles.accept_calibration(path, t, 't23', 'c' * 64, 'lead-fixture')
        record = json.loads(path.read_text())['profiles']['t23']
        self.assertEqual((record['status'], record['accepted_by']), ('verified', 'lead-fixture'))
        self.assertIn('+00:00', record['accepted_at'])
        applied = titles.apply_calibration(path, t, 't23', 'c' * 64)
        self.assertEqual(applied['install']['automation'][0]['tree_sha256'], 'b' * 64)
        self.assertEqual(applied['checkpoints'][0]['value'], self.frame_digest)
        self.assertEqual(applied['checkpoints'][0]['at_ms'], t['checkpoints'][0]['at_ms'])
        self.assertEqual(applied['checkpoints'][1]['at_ms'], t['checkpoints'][1]['at_ms'])
        self.assertEqual(applied['checkpoints'][0]['deadline_ms'], t['checkpoints'][0]['deadline_ms'])
        verified = titles.parse_serial(self.transcript(applied, False), '12345678', applied)
        self.assertEqual(verified['outcome'], 'pass')
        with self.assertRaises(EvidenceError): titles.parse_serial(raw, '12345678', applied)
        changed = copy.deepcopy(t);changed['install']['automation'][0]['input'][0]['at_ms'] += 1
        for title, profile, image in ((changed, 't23', 'c' * 64), (t, 't23', 'd' * 64)):
            with self.assertRaises(ValueError): titles.apply_calibration(path, title, profile, image)
        with self.assertRaises(EvidenceNotRun): titles.apply_calibration(path, t, 'other', 'c' * 64)
        # Recapture uses checked expectations from the previous capture and
        # records its actual plan, without turning capture into verification.
        recaptured = titles.parse_serial(self.transcript(applied, True), '12345678', applied, True)
        titles.save_calibration(path, t, recaptured, 't23', 'c' * 64, '87654321', captures)
        saved = json.loads(path.read_text())['profiles']['t23']
        self.assertEqual(saved['plan_sha256'], hashlib.sha256(titles.plan(applied, True)).hexdigest())
        with self.assertRaises(EvidenceNotRun): titles.apply_calibration(path, t, 't23', 'c' * 64)
        titles.write_summary(self.path, [r])
        self.assertEqual(json.loads((self.path / 'summary.json').read_text())['qualified_title_count'], 0)

    def test_install_missing_duplicate_timeout_bad_tree_and_mode_never_pass(self):
        t = installer_sample();t['install']['automation'][0]['tree_sha256'] = 'b' * 64
        raw = self.transcript(t, False)
        self.assertEqual(titles.parse_serial(raw, '12345678', t)['outcome'], 'pass')
        for before, after in [(b'stage=0', b'stage=1'), (b'ms=900', b'ms=5001'), (b'delivered=2', b'delivered=1'),
                              (b'rc=0 files=2', b'rc=1 files=2'), (b'files=2', b'files=0'),
                              (b'sha256=' + b'b' * 64, b'sha256=' + b'c' * 64),
                              (b'calibration=0', b'calibration=1')]:
            with self.subTest(after=after), self.assertRaises(EvidenceError): titles.parse_serial(raw.replace(before, after), '12345678', t)
        lines = raw.splitlines(keepends=True);index = next(i for i, line in enumerate(lines) if b'case=install ' in line)
        with self.assertRaises(EvidenceError): titles.parse_serial(b''.join(lines[:index] + lines[index+1:]), '12345678', t)
        with self.assertRaises(EvidenceError): titles.parse_serial(b''.join(lines[:index] + [lines[index]] + lines[index:]), '12345678', t)

    def test_physical_calibration_and_verification_cli_never_launch_processes(self):
        t = installer_sample();manifest = self.path / 'manifest.json'
        captures = self.artifacts(t)
        manifest.write_text(json.dumps({'schema_version': 1, 'titles': [t]}))
        serial = self.path / 'serial.txt';serial.write_bytes(self.transcript(t, True))
        common = ['--manifest', str(manifest), '--serial', str(serial), '--run-id', '12345678', '--profile', 't23', '--image-sha256', 'c' * 64, '--captures', str(captures)]
        with patch.object(subprocess, 'Popen', side_effect=AssertionError('host-only import must not launch')), patch('builtins.print'):
            self.assertEqual(titles.main(['--calibrate', 'synthetic', *common]), 0)
            with self.assertRaises(EvidenceNotRun): titles.main(['--verify', '--title', 'synthetic', *common])
            self.assertEqual(titles.main(['--accept-calibration', 'synthetic', '--reviewer', 'lead-fixture', *common]), 0)
            applied = titles.apply_calibration(titles.calibration_path(manifest, t), t, 't23', 'c' * 64)
            serial.write_bytes(self.transcript(applied, False))
            self.assertEqual(titles.main(['--verify', '--title', 'synthetic', *common]), 0)


if __name__ == '__main__': unittest.main()
