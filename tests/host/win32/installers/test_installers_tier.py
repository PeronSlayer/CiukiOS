"""f5-15C: the QEMU `installers` tier pieces that need no emulator.

The guest operator program (compiler and C parser round trip, window
matching), the runner's media (installer CD, original disc and BIN/CUE
derivation, run parameter disk), the independent host inventory (FAT walk,
CRG1 registry hives, CiukiOS shell links, exact deltas and manifest
comparisons), the serial schema of the kernel observer and the runner's
pre-launch rules (Win16 deferral, calibration waivers, calibration and
acceptance writes). All payloads here are authored bytes.
SPDX-License-Identifier: LGPL-2.1-or-later
"""
import copy
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'scripts/test'))
from scripts.win32 import operator_program, workloads  # noqa: E402
import win32_evidence  # noqa: E402
import win32_fixtures  # noqa: E402
import win32_inventory  # noqa: E402
import win32_runner  # noqa: E402
from evidence import EvidenceNotRun  # noqa: E402

OUT = ROOT / 'build/host/win32/installers-tier'
FAT_TOOLS = all(shutil.which(t) for t in ('mkfs.fat', 'mcopy', 'mmd', 'mdel', 'mtype'))
ALIASES = {'diablo-shareware': 'win32-diablo-shareware', 'starcraft-shareware': 'win32-sc', 'aoe-trial': 'win32-aoe-trial',
           'half-life-uplink': 'win32-half-life-uplink', 'ut-demo-348': 'win32-ut-demo-348',
           'worms-armageddon-demo': 'win32-wa', 'heroes3-demo': 'win32-heroes3-demo', 'rct-demo': 'win32-rct-demo',
           'jazz2-shareware': 'win32-jazz2-shareware', 'ta-demo': 'win32-ta-demo'}


def parser_binary():
    OUT.mkdir(parents=True, exist_ok=True)
    binary = OUT / 'title-program'
    subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', str(ROOT / 'tests/host/win32/installers/title_program_test.c'),
                    '-o', str(binary)], check=True)
    return binary


class OperatorProgramTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = parser_binary()

    def parse(self, text):
        with tempfile.NamedTemporaryFile('w', dir=OUT, suffix='.txt', delete=False) as f:
            f.write(text)
        try:
            out = subprocess.run([self.binary, f.name], capture_output=True, text=True, timeout=20)
        finally:
            Path(f.name).unlink()
        return out.returncode, json.loads(out.stdout)

    def test_builtin_parser_matcher_and_inventory_policy(self):
        out = subprocess.run([self.binary], capture_output=True, text=True, timeout=20)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertIn('PASS title program checks=', out.stdout)

    def test_every_operator_record_compiles_and_parses_in_both_modes(self):
        for workload in ALIASES:
            record, guest = operator_program.load(workload)
            for mode in ('install', 'full'):
                with self.subTest(workload=workload, mode=mode):
                    code, parsed = self.parse(operator_program.compile_program(workload, guest, mode))
                    self.assertEqual(code, 0, parsed)
                    self.assertEqual((parsed['workload'], parsed['entry'], parsed['arguments']),
                                     (workload, guest['entry'], guest['arguments']))
                    self.assertEqual(parsed['uninstall'], int(mode == 'full' and guest['uninstall']['enabled']))
                    self.assertEqual((parsed['timeout'], parsed['defaults'], parsed['restart_accept']),
                                     (guest['timeout_seconds'], 1, 1))
                    self.assertEqual(parsed['install_steps'] + parsed['uninstall_steps'], len(parsed['steps']))

    def test_every_action_round_trips_through_the_guest_parser(self):
        guest = copy.deepcopy(operator_program.load('ta-demo')[1])
        guest['arguments'] = '/S /D=C:\\Games\\TA Demo'
        guest['steps'] = [
            {'segment': 'install', 'action': 'wait', 'title': 'Total Annihilation*', 'timeout_seconds': 90},
            {'segment': 'install', 'action': 'button', 'title': 'Setup*', 'button': 'I &Agree', 'settle_ms': 800},
            {'segment': 'install', 'action': 'expect', 'class': '#32770', 'text': 'C:\\Program Files\\*'},
            {'segment': 'install', 'action': 'text', 'value': 'C:\\Program Files\\Cavedog TA', 'replace': True},
            {'segment': 'install', 'action': 'keys', 'keys': ['alt+n', 'shift+tab', 'space']},
            {'segment': 'install', 'action': 'click', 'title': 'Setup*', 'x': 320, 'y': 410, 'repeat': 2},
            {'segment': 'install', 'action': 'gone', 'title': 'Setup*', 'optional': True},
            {'segment': 'uninstall', 'action': 'default', 'title': 'Uninstall*'}]
        code, parsed = self.parse(operator_program.compile_program('ta-demo', guest, 'full'))
        self.assertEqual(code, 0, parsed)
        self.assertEqual(parsed['arguments'], '/S /D=C:\\Games\\TA Demo')
        steps = parsed['steps']
        self.assertEqual([s['action'] for s in steps], [6, 2, 8, 4, 3, 5, 7, 1])
        self.assertEqual(steps[0]['timeout'], 90)
        self.assertEqual((steps[1]['button'], steps[1]['settle']), ('I &Agree', 800))
        self.assertEqual((steps[2]['class'], steps[2]['text']), ('#32770', 'C:\\Program Files\\*'))
        self.assertEqual((steps[3]['value'], steps[3]['flags']), ('C:\\Program Files\\Cavedog TA', 2))
        self.assertEqual(steps[4]['keys'], [0x31 | 0x1000, 0x0f | 0x2000, 0x39])
        self.assertEqual((steps[5]['x'], steps[5]['y'], steps[5]['repeat']), (320, 410, 2))
        self.assertEqual(steps[6]['flags'], 1)
        self.assertEqual((parsed['install_steps'], parsed['uninstall_steps']), (7, 1))
        code, parsed = self.parse(operator_program.compile_program('ta-demo', guest, 'install'))
        self.assertEqual((code, parsed['uninstall'], parsed['uninstall_steps']), (0, 0, 0))

    def test_malformed_guest_blocks_are_refused(self):
        _, guest = operator_program.load('aoe-trial')
        mutations = [
            lambda g: g.update(choices='custom'), lambda g: g.update(entry='C:\\setup.exe'),
            lambda g: g.update(entry='..\\x.exe'), lambda g: g.update(status='done'), lambda g: g.update(extra=1),
            lambda g: g.update(timeout_seconds=601), lambda g: g['defaults'].update(idle_ms=10),
            lambda g: g.update(restart='maybe'),
            lambda g: g['steps'].append({'segment': 'install', 'action': 'button'}),
            lambda g: g['steps'].append({'segment': 'install', 'action': 'click', 'x': 1, 'y': 1}),
            lambda g: g['steps'].append({'segment': 'install', 'action': 'keys', 'keys': ['N']}),
            lambda g: g['steps'].append({'segment': 'install', 'action': 'text', 'value': 'tab\there'}),
            lambda g: g['steps'].extend([{'segment': 'uninstall', 'action': 'default'}, {'segment': 'install', 'action': 'default'}]),
            lambda g: (g['uninstall'].update(enabled=False), g['steps'].append({'segment': 'uninstall', 'action': 'default'})),
            lambda g: g['steps'].extend([{'segment': 'install', 'action': 'default'}] * 65),
        ]
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                g = copy.deepcopy(guest)
                mutation(g)
                with self.assertRaises(ValueError):
                    operator_program.compile_program('aoe-trial', g)

    def test_records_stay_proposed_until_the_lead_qualifies_them(self):
        for workload in ALIASES:
            record, guest = operator_program.load(workload)
            self.assertEqual(guest['status'], 'proposed')
            self.assertEqual(record['kind'], 'hardware-operator')


class DriverTests(unittest.TestCase):
    def test_driver_against_mocked_desktop_w32srv_and_processes(self):
        """The real apps/desktop/installer_title.c; the checkpoint page is
        mapped at TITLE_RESULT_VA as the kernel probe maps it in the guest."""
        OUT.mkdir(parents=True, exist_ok=True)
        binary = OUT / 'title-driver'
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-include', str(ROOT / 'tests/host/desktop/replay_base.h'),
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-I' + str(ROOT / 'apps/desktop'), '-I' + str(ROOT / 'sdk/win32/include'),
                        '-I' + str(ROOT / 'src/kernel/include'), '-idirafter', str(ROOT / 'sdk/sysroot-overlay/include'),
                        str(ROOT / 'tests/host/desktop/installer_title_test.c'), str(ROOT / 'apps/desktop/protocol.c'),
                        '-o', str(binary)], check=True)
        with tempfile.TemporaryDirectory(dir=OUT) as mounts:
            out = subprocess.run([binary], capture_output=True, text=True, timeout=60,
                                 env={'TITLE_TEST_DIR': mounts, 'ASAN_OPTIONS': 'detect_leaks=0', 'PATH': '/usr/bin:/bin'})
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertIn('PASS installers-tier driver:', out.stdout)


def crg1(hive, sequence, keys):
    """A committed w32srv hive slot (apps/w32srv/storage.c serialise())."""
    def enc(s):
        return ''.join('\\x%02x' % ord(c) if ord(c) < 32 or c in '\\[]=:' else c for c in s)
    text = ''
    for path, values in keys:
        text += '[' + '\\x5c'.join(enc(p) for p in path.split('\\')) + ']\n'
        for name, (vtype, data) in values.items():
            text += enc(name) + '=%08x:' % vtype + data.hex() + '\n'
    body = text.encode('ascii')
    header = bytearray(40)
    header[:4] = b'CRG1'
    struct.pack_into('<IIIII', header, 4, 1, hive, sequence & 0xffffffff, sequence >> 32, len(body))
    struct.pack_into('<I', header, 24, zlib.crc32(body))
    struct.pack_into('<I', header, 28, zlib.crc32(bytes(header[:28])))
    struct.pack_into('<I', header, 32, win32_inventory.CRG_COMMITTED)
    return bytes(header) + body


def shell_link(fields):
    data = bytearray(40)
    data[:8] = b'CIUKLNK3'
    data[8] = 1
    body = b''
    for i, f in enumerate(fields):
        raw = f.encode('utf-8')
        struct.pack_into('<I', data, 16 + i * 4, len(raw))
        body += raw
    return bytes(data) + body


@unittest.skipUnless(FAT_TOOLS, 'mtools/mkfs.fat absent')
class InventoryTests(unittest.TestCase):
    def setUp(self):
        OUT.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=OUT)
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)

    def tool(self, *args):
        subprocess.run([str(a) for a in args], check=True, capture_output=True, env={'MTOOLS_SKIP_CHECK': '1', 'PATH': '/usr/bin:/bin'})

    def put(self, image, path, data):
        source = self.dir / ('src-' + hashlib.sha1(path.encode()).hexdigest())
        source.write_bytes(data)
        self.tool('mcopy', '-o', '-i', image, source, '::/' + path)

    def volumes(self):
        base = self.dir / 'base.img'
        with base.open('wb') as f:
            f.truncate(16 * 1024 ** 2)
        self.tool('mkfs.fat', '--invariant', '-F', '16', '-i', '00000001', base)
        for d in ('WINDOWS', 'WINDOWS/TEMP', 'WINDOWS/Start Menu', 'WINDOWS/Start Menu/Programs', 'Program Files', 'System'):
            self.tool('mmd', '-i', base, '::/' + d)
        self.put(base, 'WINDOWS/WIN.INI', b'[windows]\r\n')
        self.put(base, 'WINDOWS/OLD.TXT', b'old')
        self.put(base, 'System/Win32.dll', b'builtin')
        base_hive = [('HKLM', {}), ('HKLM\\Software', {'Keep': (1, 'x'.encode('utf-16-le') + b'\0\0')})]
        self.put(base, 'WINDOWS/SYSTEM.CRG.0', crg1(0, 1, base_hive))
        after = self.dir / 'after.img'
        shutil.copyfile(base, after)
        self.tool('mmd', '-i', after, '::/Program Files/Long Game Folder')
        self.put(after, 'Program Files/Long Game Folder/Game Executable.exe', b'MZ' + bytes(5000))
        self.put(after, 'WINDOWS/Start Menu/Programs/Long Game.lnk',
                 shell_link(['Long Game', 'C:\\Program Files\\Long Game Folder\\Game Executable.exe', '', '', '']))
        self.put(after, 'WINDOWS/WIN.INI', b'[windows]\r\nload=game\r\n')
        self.tool('mdel', '-i', after, '::/WINDOWS/OLD.TXT')
        self.put(after, 'WINDOWS/TEMP/_ISTMP0.TMP', b'scratch')
        self.put(after, 'WINDOWS/SYSTEM.CRG.1', crg1(0, 2, base_hive + [
            ('HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\LongGame',
             {'DisplayName': (1, 'Long Game'.encode('utf-16-le') + b'\0\0')})]))
        return base, after

    def test_exact_delta_registry_links_and_exclusions(self):
        base, after = self.volumes()
        with base.open('rb') as b, after.open('rb') as a:
            delta = win32_inventory.delta(win32_inventory.FatVolume(b), win32_inventory.FatVolume(a))
        added = {(r['path'], r['kind']) for r in delta['added']}
        self.assertEqual(added, {('C:\\Program Files\\Long Game Folder', 'directory'),
                                 ('C:\\Program Files\\Long Game Folder\\Game Executable.exe', 'file'),
                                 ('C:\\WINDOWS\\Start Menu\\Programs\\Long Game.lnk', 'file')})
        exe = next(r for r in delta['added'] if r['path'].endswith('.exe'))
        self.assertEqual((exe['size'], exe['sha256']), (5002, hashlib.sha256(b'MZ' + bytes(5000)).hexdigest()))
        self.assertEqual([r['path'] for r in delta['changed']], ['C:\\WINDOWS\\WIN.INI'])
        self.assertEqual(delta['removed'], [{'path': 'C:\\WINDOWS\\OLD.TXT', 'kind': 'file'}])
        self.assertEqual([r['path'] for r in delta['temp']], ['C:\\WINDOWS\\TEMP\\_ISTMP0.TMP'])
        self.assertEqual(delta['excluded'], 1)  # SYSTEM.CRG.1 is parsed as registry instead
        self.assertEqual(delta['system_writes'], 0)
        self.assertEqual(delta['links'], [{'path': 'C:\\WINDOWS\\Start Menu\\Programs\\Long Game.lnk',
                                           'target': 'C:\\Program Files\\Long Game Folder\\Game Executable.exe'}])
        self.assertEqual(delta['menu_links'], ['C:\\WINDOWS\\Start Menu\\Programs\\Long Game.lnk'])
        self.assertEqual([k['key'] for k in delta['registry']['added_keys']],
                         ['HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\LongGame'])
        self.assertEqual(delta['registry']['removed_keys'], [])
        # Manifest from the delta validates and compares exactly; any change fails.
        manifest = win32_inventory.manifest_from_delta(delta, {'run_id': '1234abcd', 'image_sha256': 'a' * 64,
                                                               'payload_sha256': 'b' * 64})
        workloads.validate_inventory(manifest)
        self.assertTrue(win32_inventory.compare_install(manifest, delta)['match'])
        broken = copy.deepcopy(delta)
        broken['added'][1]['sha256'] = 'f' * 64
        self.assertEqual(win32_inventory.compare_install(manifest, broken)['differences'], ['files'])
        broken = copy.deepcopy(delta)
        broken['registry']['added_keys'] = []
        self.assertEqual(win32_inventory.compare_install(manifest, broken)['differences'], ['registry'])
        guest = [{'kind': 'file', 'path': r['path'], 'size': r['size'], 'sha256': r['sha256']}
                 for r in delta['added'] + delta['changed'] if r['kind'] == 'file']
        guest.append({'kind': 'link', 'path': delta['links'][0]['path'], 'target': delta['links'][0]['target']})
        self.assertTrue(win32_inventory.compare_guest(manifest, guest)['match'])
        self.assertFalse(win32_inventory.compare_guest(manifest, guest[:-2] + guest[-1:])['match'])
        # Uninstall residue: an empty delta matches an empty residue only.
        empty = {'added': [], 'changed': [], 'removed': [], 'registry': {'added_keys': [], 'removed_keys': [], 'changed_values': []}}
        self.assertTrue(win32_inventory.compare_residue(win32_inventory.residue_from_delta(empty), empty)['match'])
        self.assertFalse(win32_inventory.compare_residue(win32_inventory.residue_from_delta(empty), delta)['match'])

    def test_partition_offset_and_the_kernel_exclusion_list_agree(self):
        image = self.dir / 'disk.img'
        mbr = bytearray(512)
        struct.pack_into('<II', mbr, 454, 2048, 4096)
        mbr[510:] = b'\x55\xaa'
        image.write_bytes(bytes(mbr) + bytes(511))
        self.assertEqual(win32_inventory.partition(image), (2048 * 512, 4096 * 512))
        header = (ROOT / 'src/probes/win32_installers/title_controller.h').read_text()
        block = re.search(r'excluded\[\] = \{(.*?)\};', header, re.S).group(1)
        kernel = [s.encode().decode('unicode_escape') for s in re.findall(r'"((?:[^"\\]|\\.)*)"', block)]
        self.assertEqual(tuple(kernel), win32_inventory.EXCLUDED)
        self.assertIn('"C:\\\\WINDOWS\\\\TEMP\\\\"', header)


class FixtureMediaTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=OUT)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'tests/win32/operators').mkdir(parents=True)
        for workload in ('aoe-trial', 'starcraft-shareware', 'diablo-shareware'):
            shutil.copyfile(ROOT / 'tests/win32/operators' / (workload + '.json'), self.root / 'tests/win32/operators' / (workload + '.json'))

    def row(self, workload, name, data):
        row = copy.deepcopy(win32_fixtures.workload_row(workload))
        archive = self.root / win32_fixtures.DOWNLOADS / workload / name
        archive.parent.mkdir(parents=True, exist_ok=True)
        archive.write_bytes(data)
        row['source'].update(file=name, size=len(data), sha1=hashlib.sha1(data).hexdigest(), sha256=hashlib.sha256(data).hexdigest())
        return row

    def generate(self, row, fixture, index=0, context=None):
        from unittest.mock import patch
        with patch.object(win32_fixtures, 'workload_row', return_value=row):
            return win32_fixtures.generate(fixture, self.root / 'build/host/media', index, self.root, context)

    def test_download_medium_and_absent_or_unpinned_archive(self):
        row = self.row('aoe-trial', 'MSAoE.exe', b'MZ authored installer stand-in')
        fixture = {'generator': 'win32-workload', 'workload': 'aoe-trial', 'media': 'iso', 'source': 'download',
                   'redistribution': 'local-only'}
        manifest = self.generate(row, fixture)
        files = [{'path': 'MSAoE.exe', 'size': 30, 'sha256': hashlib.sha256(b'MZ authored installer stand-in').hexdigest()}]
        self.assertEqual(manifest['payload_sha256'], win32_fixtures.payload_identity(files))
        self.assertEqual(manifest['archive_verification']['status'], 'pinned')
        self.assertTrue(Path(manifest['path']).is_file() and manifest['sha256'] == win32_fixtures.sha(manifest['path']))
        (self.root / win32_fixtures.DOWNLOADS / 'aoe-trial' / 'MSAoE.exe').unlink()
        with self.assertRaisesRegex(win32_fixtures.WorkloadAbsent, 'fixtures_absent: .*MSAoE.exe'):
            self.generate(row, fixture, 1)
        row['source']['sha256'] = None
        with self.assertRaises(win32_fixtures.WorkloadUnqualified):
            self.generate(row, fixture, 2)

    def test_original_iso_disc_is_attached_unchanged(self):
        data = bytes(range(256)) * 64
        row = self.row('diablo-shareware', 'DIABLOSW.iso', data)
        manifest = self.generate(row, {'generator': 'win32-workload', 'workload': 'diablo-shareware', 'media': 'iso',
                                       'source': 'disc', 'redistribution': 'local-only'})
        self.assertEqual(Path(manifest['path']).read_bytes(), data)
        self.assertIsNone(manifest['derived'])
        self.assertEqual(manifest['files'][0]['path'], 'DIABLOSW.iso')

    def bin_cue(self, mode, sectors=3, corrupt=False):
        raw, data = bytearray(), bytearray()
        for i in range(sectors):
            user = bytes([(i * 7 + j) & 255 for j in range(2048)])
            sector = bytearray(2352)
            sector[:12] = win32_fixtures.CD_SYNC
            sector[15] = 2 if mode == 'MODE2/2352' else 1
            start = 24 if mode == 'MODE2/2352' else 16
            sector[start:start + 2048] = user
            if corrupt and i == 1:
                sector[0] = 1
            raw += sector
            data += user
        buffer = self.root / ('disc-%s.zip' % mode.replace('/', '-'))
        with zipfile.ZipFile(buffer, 'w') as z:
            z.writestr('Disc (USA).cue', 'FILE "Disc (USA).bin" BINARY\n  TRACK 01 %s\n    INDEX 01 00:00:00\n' % mode)
            z.writestr('Disc (USA).bin', bytes(raw))
        return buffer.read_bytes(), bytes(data)

    def test_bin_cue_data_track_is_derived_exactly(self):
        for index, mode in enumerate(('MODE2/2352', 'MODE1/2352')):
            archive, data = self.bin_cue(mode)
            row = self.row('starcraft-shareware', 'StarCraft Shareware (USA).zip', archive)
            fixture = {'generator': 'win32-workload', 'workload': 'starcraft-shareware', 'media': 'iso',
                       'source': 'disc', 'redistribution': 'local-only'}
            manifest = self.generate(row, fixture, index)
            self.assertEqual(Path(manifest['path']).read_bytes(), data)
            self.assertEqual(manifest['derived']['mode'], mode)
        archive, _ = self.bin_cue('MODE2/2352', corrupt=True)
        row = self.row('starcraft-shareware', 'StarCraft Shareware (USA).zip', archive)
        with self.assertRaisesRegex(ValueError, 'does not match its cue mode'):
            self.generate(row, fixture, 5)

    @unittest.skipUnless(FAT_TOOLS, 'mtools/mkfs.fat absent')
    def test_run_parameter_disk_carries_identity_and_program(self):
        row = copy.deepcopy(win32_fixtures.workload_row('aoe-trial'))
        fixture = {'generator': 'win32-workload', 'workload': 'aoe-trial', 'media': 'fat16', 'source': 'run',
                   'redistribution': 'local-only'}
        context = {'probe': 'win32-aoe-trial', 'tier': 'installers', 'scene': 'default-play', 'calibrate': 'install',
                   'payload_sha256': 'b' * 64, 'image_sha256': 'a' * 64}
        manifest = self.generate(row, fixture, 0, context)
        image = manifest['path']
        identity = subprocess.run(['mtype', '-i', image, '::/CIUKI-ID.TXT'], check=True, capture_output=True,
                                  env={'MTOOLS_SKIP_CHECK': '1', 'PATH': '/usr/bin:/bin'}).stdout.decode()
        self.assertIn('probe=win32-aoe-trial\r\n', identity)
        self.assertIn('tier=installers\r\n', identity)
        self.assertIn('payload_sha256=' + 'b' * 64 + '\r\n', identity)
        self.assertIn('image_sha256=' + 'a' * 64 + '\r\n', identity)
        program = subprocess.run(['mtype', '-i', image, '::/CIUKI-OP.TXT'], check=True, capture_output=True,
                                 env={'MTOOLS_SKIP_CHECK': '1', 'PATH': '/usr/bin:/bin'}).stdout.decode()
        self.assertEqual(program, manifest['program'])
        self.assertIn('uninstall 0 ', program)  # install-stage calibration
        self.assertEqual(manifest['payload_sha256'], 'b' * 64)
        for broken in ({**context, 'image_sha256': None}, {**context, 'payload_sha256': 'x'}):
            with self.assertRaises(ValueError):
                self.generate(row, fixture, 1, broken)
        with self.assertRaises(ValueError):
            win32_fixtures.validate({**fixture, 'media': 'iso'})


def installer_records(uninstall=True):
    rows = [{'event': 'BEGIN', 'image_sha256': 'a' * 64},
            {'event': 'DATA', 'case': 'identity', 'payload_sha256': 'b' * 64, 'scene': 'default-play'},
            {'event': 'DATA', 'case': 'segment', 'segment': 'install', 'op': 'begin', 'guest_ms': 10, 'icount': 0},
            {'event': 'ARM', 'segment': 'install', 'action': 'script'},
            {'event': 'DATA', 'case': 'process', 'segment': 'install', 'pid': 9, 'op': 'start', 'guest_ms': 20},
            {'event': 'DATA', 'case': 'ui', 'segment': 'install', 'step': 0, 'action': 'default', 'owner': 9,
             'hwnd': 65538, 'shot': 1, 'id': 1},
            {'event': 'DATA', 'case': 'name', 'segment': 'install', 'id': 1, 'field': 'title', 'offset': 0,
             'bytes': 5, 'hex': b'Setup'.hex()},
            {'event': 'DATA', 'case': 'name', 'segment': 'install', 'id': 1, 'field': 'class', 'offset': 0,
             'bytes': 6, 'hex': b'#32770'.hex()},
            {'event': 'DATA', 'case': 'process', 'segment': 'install', 'pid': 9, 'op': 'exit', 'guest_ms': 90, 'code': 0},
            {'event': 'DATA', 'case': 'installed', 'segment': 'install', 'id': 2, 'kind': 'file', 'size': 12, 'sha256': 'c' * 64},
            {'event': 'DATA', 'case': 'name', 'segment': 'install', 'id': 2, 'field': 'path', 'offset': 0,
             'bytes': 8, 'hex': b'GAME.EXE'.hex()},
            {'event': 'DATA', 'case': 'inventory', 'segment': 'install', 'files': 1, 'directories': 0, 'links': 0,
             'changed': 0, 'removed': 0, 'temp': 0, 'excluded': 3, 'bytes': 12},
            {'event': 'DATA', 'case': 'install', 'segment': 'install', 'default_choices': 1, 'system_writes': 0,
             'menu_entry': 1, 'uninstall_registered': int(uninstall)},
            {'event': 'DATA', 'case': 'segment', 'segment': 'install', 'op': 'end', 'guest_ms': 100, 'icount': 0}]
    if uninstall:
        rows += [{'event': 'DATA', 'case': 'segment', 'segment': 'uninstall', 'op': 'begin', 'guest_ms': 110, 'icount': 0},
                 {'event': 'DATA', 'case': 'process', 'segment': 'uninstall', 'pid': 11, 'op': 'start', 'guest_ms': 120},
                 {'event': 'DATA', 'case': 'process', 'segment': 'uninstall', 'pid': 11, 'op': 'exit', 'guest_ms': 150, 'code': 0},
                 {'event': 'DATA', 'case': 'residue', 'segment': 'uninstall', 'files': 0, 'directories': 0, 'links': 0},
                 {'event': 'DATA', 'case': 'uninstall', 'segment': 'uninstall', 'key_removed': 1, 'menu_removed': 1},
                 {'event': 'DATA', 'case': 'segment', 'segment': 'uninstall', 'op': 'end', 'guest_ms': 160, 'icount': 0}]
    rows += [{'event': 'DATA', 'case': 'ledger_summary', 'clean': 1, 'driver_failure': 0}, {'event': 'END', 'status': 'PASS'}]
    return rows


def wire(record, seq, probe='win32-aoe-trial'):
    fields = ' '.join('%s=%s' % (k, v) for k, v in record.items() if k != 'event')
    return ('CIUKI_TEST v=1 run=1234abcd seq=%06d probe=%s event=%s%s\n'
            % (seq, probe, record['event'], ' ' + fields if fields else '')).encode()


class EvidenceTests(unittest.TestCase):
    def test_installers_tier_stream_parses_and_judges(self):
        for uninstall in (False, True):
            with self.subTest(uninstall=uninstall):
                parser = win32_evidence.Parser('1234abcd', 'win32-aoe-trial', 'a' * 64, 'b' * 64, 'default-play')
                for seq, record in enumerate(installer_records(uninstall), 1):
                    parser.feed(wire(record, seq))
                for line in (b'[win32] start pid=9 exe=MSAoE.exe\n', b'[win32] exit pid=9 code=00000000\n'):
                    parser.observe_line(line)
                self.assertEqual(parser.name('1', 'title'), 'Setup')
                self.assertEqual(parser.name('1', 'class'), '#32770')
                self.assertEqual(parser.installed_inventory()[0]['path'], 'GAME.EXE')
                host = {'installed_files': True, 'install_screens': True, 'desktop_screen': True}
                if uninstall:
                    host['uninstalled_files'] = True
                verdict = parser.verdict(host, uninstall=uninstall, tier='installers')
                self.assertEqual(verdict['outcome'], 'pass', verdict)

    def test_ui_and_summary_records_are_bounded_by_the_schema(self):
        parser = win32_evidence.Parser('1234abcd', 'win32-aoe-trial', 'a' * 64, 'b' * 64, 'default-play')
        rows = installer_records(False)
        for seq, record in enumerate(rows[:5], 1):
            parser.feed(wire(record, seq))
        for bad in ({**rows[5], 'action': 'dance'}, {**rows[5], 'shot': 2}, {k: v for k, v in rows[5].items() if k != 'owner'},
                    {'event': 'DATA', 'case': 'residue', 'segment': 'install', 'files': 0, 'directories': 0, 'links': 0}):
            with self.subTest(bad=bad), self.assertRaises(win32_evidence.EvidenceError):
                copy.deepcopy(parser).feed(wire(bad, 6))

    def test_kernel_observer_formats_match_the_schema_and_fit_a_record(self):
        source = (ROOT / 'src/kernel/probes/installer_title_probe.inc').read_text()
        formats = re.findall(r'rec_emit\(title_obs\.name, "DATA", "(case=[^"]+)"', source)
        formats += re.findall(r'rec_emit\(name, "DATA", "(case=[^"]+)"', source)
        cases = set()
        for fmt in formats:
            keys = [k for k, _ in (token.split('=', 1) for token in fmt.split(' '))]
            case = fmt.split(' ', 1)[0].split('=', 1)[1]
            cases.add(case)
            required, optional = win32_evidence.SCHEMAS[case]
            self.assertTrue(required <= set(keys) - {'case'} <= required | optional, fmt)
            # Worst case: the longest alias and the widest value each field can
            # take: ids/sequence are bounded by the 16,384-record cap, offsets by
            # FS_PATH_BYTES (781), 32-bit counters by ten digits, digests and
            # name chunks by their fixed hex widths.
            width = {'id': 5, 'offset': 3, 'bytes': 2, 'step': 2, 'owner': 5, 'pid': 5, 'shot': 1, 'clean': 1,
                     'segment': 9, 'action': 7, 'field': 6, 'hex': 80, 'sha256': 64, 'payload_sha256': 64, 'scene': 31,
                     'op': 5, 'kind': 9, 'code': 11, 'error': 11, 'default_choices': 1, 'menu_entry': 1,
                     'uninstall_registered': 1, 'key_removed': 1, 'menu_removed': 1,
                     # Delta counts are bounded by the entries of the 512 MiB C:
                     # volume (131,072 clusters), its byte total by the volume.
                     'files': 6, 'directories': 6, 'links': 6, 'changed': 6, 'removed': 6, 'temp': 6, 'excluded': 6,
                     'guest_ms': 10, 'icount': 1, 'driver_failure': 2}
            line = 'CIUKI_TEST v=1 run=12345678 seq=000001 probe=win32-half-life-uplink event=DATA'
            for token in fmt.split(' '):
                key, value = token.split('=', 1)
                if key == 'bytes' and value == '%llu':
                    size = 10  # the inventory byte total of the 512 MiB volume
                else:
                    size = width.get(key, 20 if value == '%llu' else 10)
                line += ' ' + key + '=' + ('x' * size if '%' in value else value)
            length = len(line)
            self.assertLessEqual(length, 240, fmt)
        self.assertLessEqual({'ui', 'inventory', 'residue', 'installed', 'install', 'uninstall', 'process', 'name',
                              'identity', 'segment', 'ledger_summary', 'snapshot'}, cases)


class RunnerRulesTests(unittest.TestCase):
    def cases(self):
        return {c['win32']['workload']: c for c in json.loads((ROOT / 'tests/suites/f5-installers.json').read_text())['cases']}

    def test_win16_is_deferred_and_calibration_waives_only_its_outputs(self):
        cases = self.cases()
        with self.assertRaises(win32_runner.Blocked) as ctx:
            win32_runner.precheck(cases['worms-armageddon-demo'])
        self.assertEqual(ctx.exception.kind, 'deferred_win16')
        self.assertIn('deferred (Win16)', str(ctx.exception))
        for workload, case in cases.items():
            if workload == 'worms-armageddon-demo':
                continue
            win32_runner.validate_case(case)
            with self.assertRaises(win32_runner.Blocked) as ctx:
                win32_runner.precheck(case)
            kinds = {b['kind'] for b in ctx.exception.blockers}
            self.assertLessEqual({'operator_program_pending', 'manifest_pending', 'screen_pins_pending'}, kinds)
            self.assertNotIn('probe_not_registered', kinds)
            calibration = dict(case, _win32_calibrate='install')
            win32_runner.validate_case(calibration)
            win32_runner.precheck(calibration)
        with self.assertRaises(win32_runner.res.Refusal):
            win32_runner.validate_case(dict(cases['aoe-trial'], _win32_calibrate='everything'))
        broken = copy.deepcopy(cases['aoe-trial'])
        broken['fixtures'].pop()
        with self.assertRaises(win32_runner.res.Refusal):
            win32_runner.validate_case(broken)

    def test_calibration_writes_then_lead_acceptance(self):
        from unittest.mock import patch
        sys.path.insert(0, str(ROOT / 'scripts/win32/titles'))
        import driver
        with tempfile.TemporaryDirectory(dir=OUT) as name:
            root = Path(name)
            for relative in ('tests/win32/workloads.json', 'tests/suites/f5-installers.json', 'tests/win32/operators/aoe-trial.json'):
                (root / relative).parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(ROOT / relative, root / relative)
            case = self.cases()['aoe-trial']
            row = win32_fixtures.workload_row('aoe-trial')
            delta = {'added': [{'path': 'C:\\Program Files\\AoE\\EMPIRES.EXE', 'kind': 'file', 'size': 9, 'sha256': 'c' * 64}],
                     'changed': [], 'removed': [], 'excluded': 2, 'temp': [], 'system_writes': 0,
                     'links': [], 'menu_links': [], 'registry': {'added_keys': [], 'removed_keys': [], 'changed_values': []}}
            records = [{'case': 'install', 'uninstall_registered': '1'}]
            frames = [{'name': 'install-welcome', 'region_sha256': 'd' * 64, 'calibration': True}]
            inventory = [{'kind': 'file', 'path': 'C:\\Program Files\\AoE\\EMPIRES.EXE', 'size': 9, 'sha256': 'c' * 64}]
            manifest = win32_runner.calibration(case, row, '1234abcd', 'a' * 64, 'b' * 64, delta, inventory, records, frames, 'install')
            self.assertEqual(manifest['status'], 'calibrated')
            self.assertTrue(manifest['calibration']['guest_inventory_agrees'])
            win32_runner.write_calibration('aoe-trial', manifest, root)
            stored = workloads.load(root / 'tests/win32/workloads.json')
            stored_row = next(r for r in stored['workloads'] if r['id'] == 'aoe-trial')
            self.assertEqual(stored_row['installed_files']['entries'][0]['name'], 'C:\\Program Files\\AoE\\EMPIRES.EXE')
            with patch.object(win32_fixtures, 'workload_row', return_value=stored_row):
                with self.assertRaisesRegex(ValueError, 'both calibration runs'):
                    driver.accept('aoe-trial', case, root)
                full = win32_runner.calibration(case, stored_row, '5678abcd', 'a' * 64, 'b' * 64,
                                                {**delta, 'added': []}, inventory, records, [], 'full')
            win32_runner.write_calibration('aoe-trial', full, root)
            accepted = driver.accept('aoe-trial', case, root)
            self.assertEqual(accepted, {'manifest': 'verified', 'operator': 'qualified', 'screens': ['install-welcome'],
                                        'uninstall': True})
            final = json.loads((root / 'tests/suites/f5-installers.json').read_text())
            target = next(c for c in final['cases'] if c['id'] == case['id'])
            self.assertEqual(target['win32']['screens']['install-welcome']['sha256'], 'd' * 64)
            self.assertTrue(target['win32']['uninstall'])
            stored_row = next(r for r in workloads.load(root / 'tests/win32/workloads.json')['workloads'] if r['id'] == 'aoe-trial')
            self.assertEqual(stored_row['installed_files']['status'], 'verified')
            self.assertEqual(stored_row['installed_files']['uninstall']['residue']['files'], [])

    def test_installer_host_checks_need_a_verified_manifest_and_the_host_delta(self):
        manifest = {'status': 'verified', 'entries': [{'name': 'C:\\G\\GAME.EXE', 'size': 9, 'sha256': 'c' * 64}],
                    'registry': None, 'uninstall': {'registered': True, 'residue': win32_inventory.residue_from_delta(
                        {'added': [], 'changed': [], 'removed': [], 'registry': None})}}
        guest = [{'kind': 'file', 'path': 'C:\\G\\GAME.EXE', 'size': 9, 'sha256': 'c' * 64}]
        host = {'added': [{'path': 'C:\\G\\GAME.EXE', 'kind': 'file', 'size': 9, 'sha256': 'c' * 64}],
                'changed': [], 'removed': [], 'links': [], 'registry': {'added_keys': [], 'removed_keys': [], 'changed_values': []}}
        self.assertTrue(win32_runner.installer_checks(manifest, guest, host, False)['installed_files'])
        clean = {**host, 'added': []}
        checks = win32_runner.installer_checks(manifest, guest, clean, True)
        self.assertTrue(checks['installed_files'] and checks['uninstalled_files'])
        self.assertFalse(win32_runner.installer_checks(manifest, guest, host, True)['uninstalled_files'])
        self.assertNotIn('installed_files', win32_runner.installer_checks({**manifest, 'status': 'calibrated'}, guest, host, False))
        self.assertNotIn('installed_files', win32_runner.installer_checks(manifest, guest, {'error': 'no FUSE'}, False))


if __name__ == '__main__':
    unittest.main()
