"""f3-53 host evidence only. Synthetic DOS material is never executed."""
import hashlib
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


class SchemaTests(unittest.TestCase):
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
        t['licence']['kind'] = 'owner-copy'
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
                        (b'attached=1', b'attached=0'), (b'irq=5', b'irq=7'), (b'opl_writes=100', b'opl_writes=0'),
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
            binary = temp / ('model-' + sanitizer)
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=' + sanitizer, '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                            '-I', str(ROOT / 'src/kernel/include'), str(ROOT / 'tests/host/dos_titles_test.c'),
                            str(ROOT / 'src/kernel/probes/selector.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary), str(wire)], check=True)

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

    def test_target_controller_compile(self):
        out = ROOT / 'build/host';out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=out, prefix='title-target-') as temp:
            temp = Path(temp)
            subprocess.run(['clang', '--target=i686-unknown-elf', '-march=pentiumpro', '-ffreestanding',
                            '-fno-builtin', '-mno-sse', '-mno-sse2', '-mno-80387', '-std=c17', '-O2',
                            '-Wall', '-Wextra', '-Werror', '-Wframe-larger-than=2048',
                            '-I', str(ROOT / 'src/kernel/include'), '-c',
                            str(ROOT / 'src/kernel/probes/f3_probes_titles.c'), '-o', str(temp / 'controller.o')], check=True)


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
                with self.subTest(path=path), self.assertRaises(ValueError): titles.files(t, pins)
            with zipfile.ZipFile(archive, 'w') as z: z.writestr('GAME.EXE', b'MZ')
            pins['titles'][0].update(sha256=dos_fixtures.sha(archive), size=archive.stat().st_size)
            self.assertEqual(titles.files(t, pins), {'/SYNTH/GAME.EXE': b'MZ'})
            pins['titles'][0]['sha256'] = '0' * 64
            with self.assertRaises(ValueError): titles.files(t, pins)
            with zipfile.ZipFile(archive, 'w') as z:
                link = zipfile.ZipInfo('GAME.EXE');link.external_attr = 0o120777 << 16
                z.writestr(link, 'TARGET')
            pins['titles'][0].update(sha256=dos_fixtures.sha(archive), size=archive.stat().st_size)
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


if __name__ == '__main__': unittest.main()
