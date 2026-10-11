"""f3-06 host tests: local DOS workload fixtures, runner not_run path, suite
records of probes/f3_probes_workloads.c, payload guards.

Synthetic game files only: the owner's commercial payloads are never read
by these tests. Fixtures below are parser inputs, not guest or QEMU evidence.
"""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/test'))
import run as runner  # noqa: E402
import dos_fixtures as fx  # noqa: E402
from evidence import EvidenceError, EvidenceNotRun  # noqa: E402
from loader_model import F3_PROBES  # noqa: E402

TOOLS = all(shutil.which(t) for t in ('mkfs.fat', 'fsck.fat', 'mcopy', 'mdir', 'nasm'))
SUITE = runner.load(ROOT / 'tests/suites/f3-workloads.json')
HEX = 'ab' * 32


def builder():
    spec = importlib.util.spec_from_file_location('build_dos_image_f306', ROOT / 'scripts/build_dos_image.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def case(name):
    return next(c for c in SUITE['cases'] if c['id'] == name)


def parse(probe, rows, status='PASS'):
    p = runner.Parser('12345678', probe)
    records = [('BEGIN', {'version': 1, 'public': 0})] + rows + [('END', {'status': status, 'error': 0})]
    for seq, (event, fields) in enumerate(records, 1):
        text = ' '.join(f'{k}={v}' for k, v in fields.items())
        line = f'CIUKI_TEST v=1 run=12345678 seq={seq:06d} probe={probe} event={event} {text}'
        assert len(line) <= 240, line
        p.feed((line + '\n').encode())
    return p


def tail(vms, request=33554432):
    rows = [('DATA', dict(group='admission', free=90000000, reserved=0, resident=0, outstanding=0, request=request,
                          headroom=33554432, admitted=1)) for _ in range(vms)]
    rows += [('DATA', dict(group='vm_cleanup', vm=1, generation=i + 1, data_loss=0, error=0)) for i in range(vms)]
    rows.append(('DATA', dict(case='cleanup', vms=0, tasks=0, pages=0, handles=0, devices=0, restored=1)))
    return rows


def dos4gw_rows():
    rows = [('DATA', dict(case='identity', build='0123abcd', loops=20)),
            ('DATA', dict(group='dos_payload', path='/DPMI4GW.EXE', size=40000, sha256=HEX)),
            ('DATA', dict(group='dos_payload', path='/SYSTEM/DOS4GW.EXE', size=259183, sha256=HEX)),
            ('DATA', dict(case='extender', path='/SYSTEM/DOS4GW.EXE', kind='external', version='1.97',
                          client='/DPMI4GW.EXE', redistribution='open_question'))]
    rows += [('DATA', dict(case='loop', loop=i, **{'pass': 1}, entries=1, terminations=1, faults=0, stale=0, released=1,
                           ms=1500, restored=1, error=0)) for i in range(1, 21)]
    rows.append(('DATA', dict(case='loops', completed=20, required=20, restored=1)))
    return rows + tail(21)


def doom_rows(sound):
    audio = (dict(resets=1, blocks=400, frames=900000, edges=400, services=400, opl=5000, dma_faults=0, audible=0) if sound else
             dict(resets=0, blocks=0, frames=0, edges=0, services=0, opl=0, dma_faults=0, audible=0))
    rows = [('DATA', dict(case='identity', build='0123abcd', loops=5, sound=int(sound))),
            ('DATA', dict(case='fixture', status='present', drive='D', base='CIUKIGAM.IMG', size=134217728)),
            ('DATA', dict(group='workload_file', path='/DOOM/DOOM.EXE', size=709905, sha256=HEX, redistribution='local-only')),
            ('DATA', dict(group='workload_file', path='/DOOM/DOOM.WAD', size=11159840, sha256=HEX, redistribution='local-only')),
            ('DATA', dict(group='workload_file', path='/DOOM/CIUKI.CFG', size=300, sha256=HEX, redistribution='free')),
            ('DATA', dict(group='dos_payload', path='/RUNREC.COM', size=1328, sha256=HEX)),
            ('DATA', dict(case='extender', path='/DOOM/DOOM.EXE', kind='bound_dos4gw_professional', files=3))]
    for i in range(1, 6):
        rows += [('DATA', dict(case='loop', loop=i, dpmi_ms=900, launch_ms=4000, run_ms=200000, gametics=1710,
                               realtics=9000, timed=1, status=1, rc=1, type=0, error=0)),
                 ('DATA', dict(case='video', loop=i, graphics=1, width=320, height=200, flips=1720, faults=40, pm_faults=40,
                               maps=6900, flushes=7000, mode=3)),
                 ('DATA', dict(case='dpmi', loop=i, entries=1, terminations=1, exceptions=0, irq_pm=9000, faults=0, stale=0,
                               released=1)),
                 ('DATA', dict(case='audio', loop=i, **audio)),
                 ('DATA', dict(case='loop_cleanup', loop=i, restored=1))]
    rows.append(('DATA', dict(case='loops', completed=5, required=5)))
    return rows + tail(6)


def wolf_rows():
    rows = [('DATA', dict(case='identity', build='0123abcd', loops=5)),
            ('DATA', dict(case='fixture', status='present', drive='D', base='CIUKIGAM.IMG', size=134217728))]
    rows += [('DATA', dict(group='workload_file', path=f'/WOLF3D/{n}', size=1000, sha256=HEX, redistribution='local-only'))
             for n in ('WOLF3D.EXE', 'VSWAP.WL6', 'VGAGRAPH.WL6', 'GAMEMAPS.WL6')]
    rows.append(('DATA', dict(case='extender', path='/WOLF3D/WOLF3D.EXE', kind='real_mode', files=4)))
    for i in range(1, 6):
        rows += [('DATA', dict(case='frame', loop=i, stage='signon', mode=19, width=320, height=200, digest=HEX)),
                 ('DATA', dict(case='frame', loop=i, stage=1, mode=19, width=320, height=200, digest='cd' * 32)),
                 ('DATA', dict(case='play', loop=i, playing=1, flips=40, faults=10, maps=300, digest=HEX)),
                 ('DATA', dict(case='loop', loop=i, launch_ms=5000, run_ms=60000, screens=3, status=1, rc=0, type=0,
                               xms_calls=12, ems_calls=30, error=0)),
                 ('DATA', dict(case='loop_cleanup', loop=i, restored=1))]
    rows.append(('DATA', dict(case='loops', completed=5, required=5)))
    return rows + tail(6)


def starve_rows():
    return [('DATA', dict(case='identity', build='0123abcd')),
            ('DATA', dict(group='admission', free=90000000, reserved=0, resident=0, outstanding=0, request=16777216,
                          headroom=33554432, admitted=1)),
            ('DATA', dict(case='phase', phase='reference', **{'pass': 1}, ms=1200, irq0=22, irq0_per_s=18, heartbeat=900000,
                          keys=16, key_ms=170)),
            ('DATA', dict(group='admission', free=70000000, reserved=16777216, resident=9000000, outstanding=7777216,
                          request=8388608, headroom=33554432, admitted=1)),
            ('DATA', dict(case='phase', phase='loaded', **{'pass': 1}, ms=2000, irq0=36, irq0_per_s=18, heartbeat=700000,
                          keys=16, key_ms=180)),
            ('DATA', dict(case='rates', timer_pct=98, heartbeat_pct=46, keys_ref=16, keys_loaded=16, key_ms_loaded=180,
                          cli_vm_running=1, cli_vm_irq0=0)),
            ('DATA', dict(group='vm_cleanup', vm=2, generation=2, data_loss=0, error=0)),
            ('DATA', dict(group='vm_cleanup', vm=1, generation=1, data_loss=0, error=0)),
            ('DATA', dict(case='cleanup', vms=0, tasks=0, pages=0, handles=0, devices=0, restored=1))]


def mutate(rows, where, field, value):
    rows = copy.deepcopy(rows)
    for event, fields in rows:
        if all(str(fields.get(k)) == str(v) for k, v in where.items()):
            fields[field] = value
            return rows
    raise AssertionError(f'no row {where}')


class Workloads(unittest.TestCase):
    def test_probe_names_in_every_selector_table(self):
        names = ('dos4gw', 'doom', 'doom-sound', 'wolf3d', 'vm-starve')
        self.assertEqual(F3_PROBES[-5:], names)
        for name in names:
            self.assertEqual(runner.selector(f'f3:{name} run=12345678')['probe'], name)
        selector_c = (ROOT / 'src/kernel/probes/selector.c').read_text()
        menu = (ROOT / 'src/boot/ciukldr/menu.inc').read_text()
        source = (ROOT / 'src/kernel/probes/f3_probes_workloads.c').read_text()
        build = (ROOT / 'scripts/build_kernel.py').read_text()
        self.assertIn('"probes_f3_probes_workloads": 5', build)
        for name in names:
            self.assertIn(f'"{name}"', selector_c)
            self.assertIn(f"'{name}',0", menu)
            self.assertIn(f'CIUKI_F3_PROBE("{name}"', source)
        # Registration order in the translation unit is the sweep order.
        order = [source.index(f'CIUKI_F3_PROBE("{n}"') for n in names]
        self.assertEqual(order, sorted(order))
        with self.assertRaises(ValueError):
            runner.selector('f3:doom run=12345678 safe=1', 'fw_cfg', True)

    def test_suite_cases_and_records(self):
        self.assertEqual({c['probe'] for c in SUITE['cases']}, {'dos4gw', 'doom', 'doom-sound', 'wolf3d', 'vm-starve'})
        self.assertIn('f3-vm', SUITE['prerequisites'])
        for c in SUITE['cases']:
            runner.selector(c['selector'].format(run_id='12345678'))
            if c['probe'] in ('doom', 'doom-sound', 'wolf3d'):
                self.assertEqual([f['generator'] for f in c['fixtures']], ['dos-local'])
                self.assertEqual(c['fixtures'][0]['redistribution'], 'local-only')
                self.assertEqual(runner.fixture_disks(c), [(0, 1)])
            else:
                self.assertNotIn('fixtures', c)
        for name, probe, rows in (('dos4gw-qemu-t23', 'dos4gw', dos4gw_rows()), ('dos4gw-qemu-min128', 'dos4gw', dos4gw_rows()),
                                  ('doom-qemu-t23', 'doom', doom_rows(False)),
                                  ('doom-sound-qemu-t23', 'doom-sound', doom_rows(True)),
                                  ('wolf3d-qemu-t23', 'wolf3d', wolf_rows()),
                                  ('vm-starve-qemu-t23', 'vm-starve', starve_rows())):
            with self.subTest(case=name):
                self.assertTrue(parse(probe, rows).check(case(name)['expected']))
                with self.assertRaises(EvidenceError):
                    parse(probe, rows, 'FAIL').check(case(name)['expected'])
        broken = [
            ('dos4gw-qemu-t23', 'dos4gw', dos4gw_rows(), {'case': 'loop', 'loop': 7}, 'restored', 0),
            ('dos4gw-qemu-t23', 'dos4gw', dos4gw_rows(), {'case': 'loop', 'loop': 3}, 'terminations', 0),
            ('dos4gw-qemu-t23', 'dos4gw', dos4gw_rows(), {'case': 'loops'}, 'completed', 19),
            ('doom-qemu-t23', 'doom', doom_rows(False), {'case': 'audio', 'loop': 2}, 'opl', 3),
            ('doom-qemu-t23', 'doom', doom_rows(False), {'case': 'loop', 'loop': 4}, 'timed', 0),
            ('doom-qemu-t23', 'doom', doom_rows(False), {'case': 'loop', 'loop': 1}, 'rc', 0),
            ('doom-qemu-t23', 'doom', doom_rows(False), {'case': 'video', 'loop': 5}, 'pm_faults', 0),
            ('doom-qemu-t23', 'doom', doom_rows(False), {'case': 'dpmi', 'loop': 5}, 'released', 0),
            ('doom-qemu-t23', 'doom', doom_rows(False), {'group': 'workload_file', 'path': '/DOOM/DOOM.EXE'}, 'redistribution', 'free'),
            ('doom-sound-qemu-t23', 'doom-sound', doom_rows(True), {'case': 'audio', 'loop': 3}, 'resets', 0),
            ('doom-sound-qemu-t23', 'doom-sound', doom_rows(True), {'case': 'audio', 'loop': 3}, 'audible', 1),
            ('wolf3d-qemu-t23', 'wolf3d', wolf_rows(), {'case': 'play', 'loop': 2}, 'playing', 0),
            ('wolf3d-qemu-t23', 'wolf3d', wolf_rows(), {'case': 'loop', 'loop': 2}, 'xms_calls', 0),
            ('wolf3d-qemu-t23', 'wolf3d', wolf_rows(), {'case': 'frame', 'loop': 1, 'stage': 'signon'}, 'width', 640),
            ('vm-starve-qemu-t23', 'vm-starve', starve_rows(), {'case': 'rates'}, 'cli_vm_irq0', 1),
            ('vm-starve-qemu-t23', 'vm-starve', starve_rows(), {'case': 'rates'}, 'timer_pct', 20),
            ('vm-starve-qemu-t23', 'vm-starve', starve_rows(), {'case': 'phase', 'phase': 'loaded'}, 'keys', 12),
        ]
        for name, probe, rows, where, field, value in broken:
            with self.subTest(case=name, field=field), self.assertRaises(EvidenceError):
                parse(probe, mutate(rows, where, field, value)).check(case(name)['expected'])
        # A baseline-mismatch diagnostic fails the DOS/4GW row even with good loops.
        rows = dos4gw_rows()
        rows.insert(10, ('DATA', dict(case='baseline', loop=5, pages='10/9', tasks='4/4', heap='1/1', vm_pages='0/0', handles='0/0')))
        with self.assertRaises(EvidenceError):
            parse('dos4gw', rows).check(case('dos4gw-qemu-t23')['expected'])

    def test_longest_records_fit_the_envelope(self):
        # Worst cases of the probe's format strings (record grammar and 240 bytes).
        big = 4294967295
        rows = [('DATA', dict(group='workload_file', path='/WOLF3D/VGAGRAPH.WL6', size=big, sha256=HEX, redistribution='local-only')),
                ('DATA', dict(case='loop', loop=5, dpmi_ms=big, launch_ms=big, run_ms=big, gametics=big, realtics=big, timed=1,
                              status=1, rc=255, type=255, error=-2147483648)),
                ('DATA', dict(case='video', loop=5, graphics=1, width=big, height=big, flips=big, faults=big, pm_faults=big,
                              maps=big, flushes=big, mode=255)),
                ('DATA', dict(case='audio', loop=5, resets=big, blocks=big, frames=big, edges=big, services=big, opl=big,
                              dma_faults=big, audible=big)),
                ('DATA', dict(case='dpmi', loop=5, entries=big, terminations=big, exceptions=big, irq_pm=big, faults=big,
                              stale=big, released=1)),
                ('DATA', dict(case='frame', loop=5, stage='signon', mode=255, width=640, height=480, digest=HEX)),
                ('DATA', dict(group='screen', row=24, text='X' * 80))]
        parse('doom-sound', rows, 'FAIL')
        rows = [('DATA', dict(case='loop', loop=20, **{'pass': 1}, entries=big, terminations=big, faults=big, stale=big,
                              released=1, ms=big, restored=1, error=-2147483648)),
                ('DATA', dict(case='extender', path='/SYSTEM/DOS4GW.EXE', kind='external', version='1.97',
                              client='/DPMI4GW.EXE', redistribution='open_question')),
                ('DATA', dict(group='dos_payload', path='/SYSTEM/DOS4GW.EXE', size=big, sha256=HEX))]
        parse('dos4gw', rows, 'FAIL')
        rows = [('DATA', dict(case='phase', phase='reference', **{'pass': 1}, ms=big, irq0=big,
                              irq0_per_s=big, heartbeat=big, keys=big, key_ms=big)),
                ('DATA', dict(case='rates', timer_pct=big, heartbeat_pct=big, keys_ref=big, keys_loaded=big,
                              key_ms_loaded=big, cli_vm_running=1, cli_vm_irq0=big))]
        parse('vm-starve', rows, 'FAIL')
        rows = [('DATA', dict(case='loop', loop=5, launch_ms=big, run_ms=big, screens=255, status=1, rc=255, type=255,
                              xms_calls=big, ems_calls=big, error=-2147483648)),
                ('DATA', dict(case='play', loop=5, playing=1, flips=big, faults=big, maps=big, digest=HEX)),
                ('DATA', dict(case='fixture', status='not_run', reason='fixtures_absent'))]
        parse('wolf3d', rows, 'FAIL')

    def test_fixtures_absent_is_not_run(self):
        for probe in ('doom', 'doom-sound', 'wolf3d'):
            p = runner.Parser('12345678', probe)
            for seq, row in enumerate(('event=BEGIN version=1', 'event=DATA case=fixture status=not_run reason=fixtures_absent',
                                       'event=END status=NOT_RUN reason=fixtures_absent'), 1):
                p.feed(f'CIUKI_TEST v=1 run=12345678 seq={seq:06d} probe={probe} {row}\n'.encode())
            self.assertEqual(p.outcome, 'not_run')
        for probe in ('dos4gw', 'vm-starve'):
            p = runner.Parser('12345678', probe)
            p.feed(f'CIUKI_TEST v=1 run=12345678 seq=000001 probe={probe} event=BEGIN\n'.encode())
            with self.assertRaises(EvidenceError):
                p.feed(f'CIUKI_TEST v=1 run=12345678 seq=000002 probe={probe} event=END status=NOT_RUN reason=fixtures_absent\n'.encode())
        # The runner reports not_run before QEMU when the declared directory is absent.
        saved = copy.deepcopy(fx.WORKLOADS)
        try:
            fx.WORKLOADS['doom']['source'] = 'third_party/absent-f306'
            declared = dict(case('doom-qemu-t23'), fixtures=[dict(case('doom-qemu-t23')['fixtures'][0], source='third_party/absent-f306')])
            with self.assertRaises(fx.FixturesAbsent) as raised:
                runner.prepare_fixtures(None, declared, ROOT / 'build/host')
            self.assertIsInstance(raised.exception, EvidenceNotRun)
            self.assertTrue(str(raised.exception).startswith('fixtures_absent'))
        finally:
            fx.WORKLOADS.clear(); fx.WORKLOADS.update(saved)
        with self.assertRaises(ValueError):
            fx.generate(dict(case('doom-qemu-t23')['fixtures'][0], redistribution='free'), ROOT / 'build/host')
        with self.assertRaises(ValueError):
            fx.generate(dict(case('doom-qemu-t23')['fixtures'][0], workload='quake'), ROOT / 'build/host')


@unittest.skipUnless(TOOLS, 'mkfs.fat, fsck.fat, mtools and nasm required')
class SyntheticFixture(unittest.TestCase):
    """A fake local DOOM directory and a fake DOS VM payload in build/host."""

    @classmethod
    def setUpClass(cls):
        cls.root = ROOT / 'build/host' / f'f306-{secrets.token_hex(4)}'
        cls.root.mkdir(parents=True)
        dos = builder()
        work = cls.root / 'work'
        work.mkdir()
        boot = dos.assemble('src/boot/full_boot.asm', work / 'boot.bin', {'FAT_TOTAL_SECTORS': dos.SECTORS})
        loader = dos.assemble('src/boot/full_stage1_loader.asm', work / 'loader.bin', dos.GEOMETRY)
        cls.system = {'/SYSTEM/CIUKIDOS.SYS': b'CIUKIDOS' * 300, '/SYSTEM/SHELL.COM': b'\xc3' * 40,
                      '/COMMAND.COM': b'\x90' * 5000, '/AUTOEXEC.BAT': b'\\DOSREADY.COM\r\n', '/RUNREC.COM': b'\xcd\x20' * 600}
        image = cls.root / 'build/f0/dos/DOSBOOT.IMG'
        image.parent.mkdir(parents=True)
        dos.write_fat16(image, boot, loader, cls.system)
        manifest = {'image_sha256': fx.sha(image), 'payloads': [
            {'path': k, 'size': len(v), 'sha256': hashlib.sha256(v).hexdigest()} for k, v in sorted(cls.system.items())]}
        image.with_suffix('.json').write_text(json.dumps(manifest))
        game = cls.root / 'third_party/Doom'
        (game / 'GUS').mkdir(parents=True)
        cls.exe = (b'MZ' + b'\0' * 200 + b'DOS/4GW Professional Protected Mode Run-time\0\0Copyright (c) Rational Systems, '
                   b'Inc. 1990-1993\0\0Nov 23 1993 20:36:03\0\x001.95\0\0' + bytes(range(256)) * 40)
        (game / 'DOOM.EXE').write_bytes(cls.exe)
        (game / 'DOOM.WAD').write_bytes(b'IWAD' + bytes(i * 7 & 255 for i in range(300000)))
        (game / 'readme.txt').write_bytes(b'synthetic\r\n' * 20)
        (game / 'DOOM (1993).ba1').write_bytes(b'not 8.3')
        (game / 'GUS' / 'INNER.DAT').write_bytes(b'subdirectory')
        cls.out = cls.root / 'build/host/run'
        cls.fixture = {'generator': 'dos-local', 'workload': 'doom', 'source': 'third_party/Doom', 'target': '/DOOM',
                       'redistribution': 'local-only'}
        cls.manifest = fx.generate(cls.fixture, cls.out, 0, root=cls.root)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.root, ignore_errors=True)

    def test_manifest_and_independent_readback(self):
        m = self.manifest
        files = {f['path']: f for f in m['files']}
        self.assertEqual(set(files), {'/DOOM/DOOM.EXE', '/DOOM/DOOM.WAD', '/DOOM/README.TXT', '/DOOM/CIUKI.CFG'})
        self.assertEqual(files['/DOOM/DOOM.EXE']['extender'], 'dos4gw-professional-1.95-bound')
        self.assertEqual(files['/DOOM/DOOM.EXE']['sha256'], hashlib.sha256(self.exe).hexdigest())
        self.assertEqual((files['/DOOM/CIUKI.CFG']['role'], files['/DOOM/CIUKI.CFG']['redistribution']), ('config', 'free'))
        self.assertTrue(all(f['redistribution'] == 'local-only' for p, f in files.items() if p != '/DOOM/CIUKI.CFG'))
        self.assertEqual(sorted(m['skipped']), ['DOOM (1993).ba1', 'GUS'])
        outer = Path(m['path'])
        self.assertEqual(outer.parent, self.out.resolve())
        self.assertEqual(outer.stat().st_size, fx.OUTER_BYTES)
        self.assertLess(outer.stat().st_blocks * 512, 32 * 1024 * 1024)       # sparse: no 128 MiB copy
        self.assertFalse((self.out / 'fixture-0.vm').exists())
        self.assertEqual(fx.sha(outer), m['sha256'])
        listing = subprocess.run(['mdir', '-i', str(outer), '::/'], capture_output=True, text=True, check=True).stdout
        self.assertIn('CIUKIGAM', listing)
        base = self.root / 'work/base.img'
        with base.open('wb') as stream:
            subprocess.run(['mcopy', '-i', str(outer), '::/CIUKIGAM.IMG', '-'], stdout=stream, check=True)
        self.assertEqual(fx.sha(base), m['base_sha256'])
        self.assertEqual(base.stat().st_size, 262144 * 512)
        subprocess.run(['fsck.fat', '-n', str(base)], check=True, capture_output=True)
        for path, data in (('/DOOM/DOOM.EXE', self.exe), ('/RUNREC.COM', self.system['/RUNREC.COM']),
                           ('/DOOM/CIUKI.CFG', fx.DOOM_CONFIG)):
            got = subprocess.run(['mcopy', '-i', str(base), '::' + path, '-'], capture_output=True, check=True).stdout
            self.assertEqual(got, data, path)
        cfg = fx.DOOM_CONFIG.decode()
        for line in ('snd_sfxdevice 3', 'snd_musicdevice 3', 'snd_sbport 544', 'snd_sbirq 7', 'snd_sbdma 1', 'use_mouse 0'):
            self.assertIn(line + '\r\n', cfg)
        base.unlink()

    def test_guest_hash_comparison(self):
        records = [{'event': 'DATA', 'group': 'workload_file', 'path': f['path'], 'size': str(f['size']), 'sha256': f['sha256']}
                   for f in self.manifest['files']]
        self.assertTrue(fx.verify_records(self.manifest, records))
        for change in ('sha', 'missing', 'extra', 'duplicate'):
            altered = copy.deepcopy(records)
            if change == 'sha': altered[0]['sha256'] = '0' * 64
            if change == 'missing': altered.pop()
            if change == 'extra': altered.append(dict(altered[0], path='/DOOM/OTHER.EXE'))
            if change == 'duplicate': altered.append(dict(altered[0]))
            with self.subTest(change=change), self.assertRaises(ValueError):
                fx.verify_records(self.manifest, altered)

    def test_refusals(self):
        for forbidden in ('build/f0/run', 'build/releases/x', 'work'):
            with self.subTest(output=forbidden), self.assertRaises(ValueError):
                fx.generate(self.fixture, self.root / forbidden, 1, root=self.root)
        with self.assertRaises(ValueError):                                     # existing output
            fx.generate(self.fixture, self.out, 0, root=self.root)
        wad = self.root / 'third_party/Doom/DOOM.WAD'
        wad.rename(wad.with_suffix('.BAK'))
        try:
            with self.assertRaises(fx.FixturesAbsent):
                fx.generate(self.fixture, self.out, 2, root=self.root)
        finally:
            wad.with_suffix('.BAK').rename(wad)
        image = self.root / 'build/f0/dos/DOSBOOT.IMG'
        with self.assertRaises(ValueError):
            fx.dos_system(image, expected_sha256='0' * 64)
        dos = builder()
        with self.assertRaises(SystemExit):
            dos.refuse_local_payloads({'/GAMES/DOOM.EXE': '1' * 64}, self.root)
        with self.assertRaises(SystemExit):
            dos.refuse_local_payloads({'/X/COPY.BIN': hashlib.sha256(self.exe).hexdigest()}, self.root)
        dos.refuse_local_payloads({'/RUNREC.COM': hashlib.sha256(self.system['/RUNREC.COM']).hexdigest()}, self.root)

    def test_fat16_writer_directories(self):
        dos = builder()
        with self.assertRaises(ValueError):
            dos.write_fat16(self.root / 'work/bad.img', b'\0' * 510 + b'\x55\xaa', b'', {'/A/B/C.TXT': b'x'}, directories=['/A'])
        # Default arguments keep the canonical payload layout (SYSTEM first).
        source = (ROOT / 'scripts/build_dos_image.py').read_text()
        self.assertIn('write_fat16(candidate, boot, loader, files)', source)
        self.assertIn('"/RUNREC.COM"', source)
        self.assertIn('"/CLILOOP.COM"', source)


if __name__ == '__main__':
    unittest.main()
