"""F5-17B canned runner/session checks. No emulator or systemd is invoked."""
import copy
import hashlib
import io
import json
from contextlib import redirect_stdout
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/test'))
sys.path.insert(0, str(ROOT / 'tests/host'))
import run as runner
import win32_runner as wr
import win32_session as ws
from evidence import EvidenceError
import test_runner as common
from test_win32_evidence import canned, wire, IMAGE, PAYLOAD


# f5-15C: the independent overlay delta matching the canned guest inventory.
HOST_DELTA = {'added': [{'path': 'GAME.EXE', 'kind': 'file', 'size': 12, 'sha256': 'c' * 64}], 'changed': [],
              'removed': [], 'excluded': 0, 'temp': [], 'system_writes': 0, 'links': [], 'menu_links': [],
              'registry': {'added_keys': [], 'removed_keys': [], 'changed_values': []}}


def frame(n):
    return (320, 300, bytes([n, n, n]) * (320 * 300))


def pin(n):
    return hashlib.sha256(bytes([n, n, n]) * 4).hexdigest()


class Qmp:
    def __init__(self, directory):
        self.directory, self.value, self.events = directory, 1, []
    def command(self, command, args):
        assert command == 'screendump'
        Path(args['filename']).write_bytes(b'P6\n320 300\n255\n' + frame(self.value)[2])
    def input_events(self, events):
        self.events.append(events)


class TierTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.directory = self.root / 'run'
        self.directory.mkdir()
        self.script_dir = self.root / wr.SCRIPTS
        self.script_dir.mkdir(parents=True)
        self.row = copy.deepcopy(wr.win32_fixtures.workload_row('diablo-shareware'))
        self.row['installed_files'] = {'status': 'verified', 'entries': [{'name': 'GAME.EXE', 'size': 12, 'sha256': 'c' * 64}]}
        p = patch.object(wr, 'kernel_probes', return_value={'win32-diablo-shareware'})
        p.start(); self.addCleanup(p.stop)
        p = patch.object(wr.win32_fixtures, 'workload_row', return_value=self.row)
        p.start(); self.addCleanup(p.stop)
        # f5-15C: the installers tier validates the title's guest operator
        # program; the canned case runs a qualified copy of the real record.
        operator = json.loads((ROOT / 'tests/win32/operators/diablo-shareware.json').read_text())
        operator['guest']['status'] = 'qualified'
        (self.root / 'tests/win32/operators').mkdir(parents=True)
        (self.root / 'tests/win32/operators/diablo-shareware.json').write_text(json.dumps(operator))

    def case(self, tier):
        case = copy.deepcopy(json.loads((ROOT / ('tests/suites/f5-' + tier + '.json')).read_text())['cases'][0])
        case['win32']['screens'] = {}
        case['win32']['edition'] = 'authored-canned-fixture'
        case['win32']['checkpoints'] = []
        case['win32'].pop('progress', None)
        segments = ['install'] if tier == 'installers' else ['start', 'play', 'focus', 'exit']
        script = {'schema_version': 1, 'clock': 'host-monotonic-ms', 'workload': 'diablo-shareware',
                  'scene': 'default-play', 'timeout_ms': 590000, 'segments': [], 'status': 'reference-qualified'}
        for i, segment in enumerate(segments):
            screen = segment + '-scene'
            case['win32']['screens'][screen] = {'region': [0, 0, 2, 2], 'sha256': pin(i + 1),
                'role': {'install': 'install', 'start': 'menu', 'play': 'play', 'focus': 'focus', 'exit': 'desktop'}[segment]}
            case['win32']['checkpoints'].append({'name': screen, 'screen': screen,
                'after': {'event': 'ARM', 'segment': segment}})
            # One bounded mouse movement acknowledges each script segment.
            script['segments'].append({'name': segment, 'anchor': {'type': 'screen', 'check': screen}, 'steps': [
                {'input_id': i + 1, 'at_ms': 0, 'measure_latency': segment == 'play',
                 'events': [{'type': 'rel', 'data': {'axis': 'x', 'value': 1}}]}]})
        case['win32']['script'] = wr.SCRIPTS + '/fixture.json'
        (self.script_dir / 'fixture.json').write_text(json.dumps(script))
        if tier == 'titles':
            case['win32']['launch']['path'] = 'GAME.EXE'
            # Two independent, same-region play checks are needed for progress.
            case['win32']['screens']['play-end'] = {'region': [0, 0, 2, 2], 'sha256': pin(5), 'role': 'play'}
            case['win32']['checkpoints'].append({'name': 'play-end', 'screen': 'play-end',
                'after': {'case': 'progress', 'checkpoint': 'scene-end'}})
            case['win32']['progress'] = {'checkpoints': ['play-scene', 'play-end'], 'must_differ': True}
        else:
            case['win32']['screens']['desktop'] = {'region': [0, 0, 2, 2], 'sha256': pin(2), 'role': 'desktop'}
            case['win32']['checkpoints'].append({'name': 'desktop', 'screen': 'desktop',
                'after': {'case': 'segment', 'segment': 'install', 'op': 'end'}})
        return case

    def session(self, case):
        wr.validate_case(case, self.root)
        return wr.Session(case, '1234abcd', IMAGE, {'manifest': [{'generator': 'win32-workload',
            'payload_sha256': PAYLOAD}]}, self.directory, self.root, read_frame=runner.read_ppm)

    def records(self, tier):
        if tier == 'installers':
            return [
                {'event': 'BEGIN', 'image_sha256': IMAGE},
                {'event': 'DATA', 'case': 'identity', 'payload_sha256': PAYLOAD, 'scene': 'default-play'},
                {'event': 'DATA', 'case': 'segment', 'segment': 'install', 'op': 'begin', 'guest_ms': 0, 'icount': 0},
                {'event': 'ARM', 'segment': 'install', 'action': 'script'},
                {'event': 'DATA', 'case': 'process', 'segment': 'install', 'pid': 7, 'op': 'start', 'guest_ms': 0},
                {'event': 'DATA', 'case': 'install', 'segment': 'install', 'default_choices': 1,
                 'system_writes': 0, 'menu_entry': 1, 'uninstall_registered': 0},
                {'event': 'DATA', 'case': 'installed', 'segment': 'install', 'id': 1, 'kind': 'file', 'size': 12, 'sha256': 'c' * 64},
                {'event': 'DATA', 'case': 'name', 'segment': 'install', 'id': 1, 'field': 'path', 'offset': 0,
                 'bytes': 8, 'hex': b'GAME.EXE'.hex()},
                {'event': 'DATA', 'case': 'process', 'segment': 'install', 'pid': 7, 'op': 'exit', 'guest_ms': 100, 'code': 0},
                {'event': 'DATA', 'case': 'segment', 'segment': 'install', 'op': 'end', 'guest_ms': 100, 'icount': 10000},
                {'event': 'END', 'status': 'PASS'}]
        rows = [r for r in canned() if r.get('segment') not in ('install', 'uninstall')]
        for segment in ('start', 'focus', 'exit'):
            at = next(i for i, r in enumerate(rows) if r.get('case') == 'segment' and r.get('segment') == segment and r['op'] == 'begin')
            rows.insert(at + 1, {'event': 'ARM', 'segment': segment, 'action': 'script'})
        at = next(i for i, r in enumerate(rows) if r.get('case') == 'loader')
        rows[at:at] = [
            {'event': 'DATA', 'case': 'launch', 'segment': 'start', 'pid': 7, 'kind': 'fixture-tree', 'id': 1},
            {'event': 'DATA', 'case': 'name', 'segment': 'start', 'id': 1, 'field': 'path', 'offset': 0,
             'bytes': 8, 'hex': b'GAME.EXE'.hex()}]
        next(r for r in rows if r.get('case') == 'input')['input_id'] = 2
        at = next(i for i, r in enumerate(rows) if r.get('case') == 'audio')
        rows.insert(at, {'event': 'DATA', 'case': 'audio_open', 'segment': 'play', 'pid': 7,
                         'stream_id': 1, 'rate': 44100, 'channels': 2})
        return rows

    def drive(self, tier, repeated=False):
        case = self.case(tier)
        if repeated:
            case['win32']['screens']['play-end']['sha256'] = pin(2)
        session, qmp = self.session(case), Qmp(self.directory)
        now = 0
        for seq, row in enumerate(self.records(tier), 1):
            now += 1
            if row.get('event') == 'ARM':
                qmp.value = {'install': 1, 'start': 1, 'play': 2, 'focus': 3, 'exit': 4}[row['segment']]
            if row.get('checkpoint') == 'scene-end':
                qmp.value = 2 if repeated else 5
            if tier == 'installers' and row.get('case') == 'segment' and row.get('op') == 'end':
                qmp.value = 2
            raw = wire(row, seq)
            session.observe(raw, session.parser.feed(raw), now)
            session.tick(now, qmp)
        for line in (b'[win32] start exe=fixture pid=7\n', b'[win32] exit pid=7 code=00000000\n'):
            session.observe(line, None, now)
        if tier == 'installers':
            session.host_inventory = HOST_DELTA
        result = {'outcome': 'pass', 'timeout': {'occurred': False}, 'audio': {'failures': []}}
        verdict = session.finish(result)
        return session, result, verdict

    def test_both_tiers_with_canned_records_frames_and_predicates(self):
        for tier in ('installers', 'titles'):
            with self.subTest(tier=tier):
                s, result, verdict = self.drive(tier)
                self.assertEqual(verdict['outcome'], 'pass', verdict)
                s.parser.check(s.case['expected'])
                self.assertTrue(s.complete)
                self.assertEqual(result['win32']['image_sha256'], IMAGE)
                self.assertEqual(len(result['win32']['record_host_times']), len(s.parser.records))
                self.assertTrue(all(r['qmp_acknowledged'] for r in s.player.observed))

    def test_repeated_presents_and_unchanged_region_fail(self):
        s, result, verdict = self.drive('titles', repeated=True)
        self.assertEqual(verdict['outcome'], 'fail')
        self.assertIn('game_progress', verdict['failures'])

    def test_wrong_scene_and_deadline(self):
        for tier in ('installers', 'titles'):
            s = self.session(self.case(tier)); qmp = Qmp(self.directory); qmp.value = 99
            wait = s.wait_for(next(iter(s.screens)), 0, 'script')
            wait['timeout_ms'] = 1000
            s.tick(0, qmp)
            with self.assertRaisesRegex(EvidenceError, 'wrong scene'):
                s.tick(1000, qmp)
            with self.assertRaises(TimeoutError):
                s.player.tick(590000, qmp)
            verdict = s.parser.verdict({}, tier=tier, timeout=True)
            self.assertEqual(verdict['reason'], 'host_timeout')

    def test_pending_cases_and_probe_length_are_not_run(self):
        for tier in ('installers', 'titles'):
            case = json.loads((ROOT / ('tests/suites/f5-' + tier + '.json')).read_text())['cases'][0]
            wr.validate_case(case)
            with self.assertRaises(wr.Blocked) as ctx:
                wr.precheck(case)
            self.assertIn('screen_pins_pending', [b['kind'] for b in ctx.exception.blockers])
        case = self.case('installers'); case['probe'] = 'win32-worms-armageddon-demo'
        blockers = wr.blockers_of(case, {}, set())
        self.assertEqual(blockers[0]['kind'], 'probe_name_exceeds_kernel_buffer')

    def test_missing_checkpoint_audio_open_and_malformed_exit_never_pass(self):
        s, result, _ = self.drive('titles')
        s.parser.records = [r for r in s.parser.records if r.get('case') != 'audio_open']
        self.assertIn('audio_stream_open', s.finish(result)['missing'])
        s.parser.kernel['exit'][0]['code'] = 'invalid'
        self.assertIn('kernel_exit_code', s.finish(result)['failures'])
        s, result, _ = self.drive('installers')
        s.triggered.remove('desktop')
        self.assertEqual(s.finish(result)['outcome'], 'not_run')
        self.assertIn('checkpoint_desktop', s.finish(result)['missing'])
        s, result, _ = self.drive('titles')
        s.win32['launch']['path'] = 'WRONG.EXE'
        self.assertIn('launch_identity', s.finish(result)['failures'])
        s, result, _ = self.drive('titles')
        s.parser.records = [r for r in s.parser.records if r.get('case') != 'name']
        self.assertIn('launch_path', s.finish(result)['missing'])

    def test_guest_not_run_before_begin_needs_no_input_arm(self):
        s = self.session(self.case('installers'))
        raw = wire({'event': 'NOT_RUN', 'reason': 'installer_absent'}, 1)
        s.observe(raw, s.parser.feed(raw), 0)
        s.tick(590000, Qmp(self.directory))
        result = {'outcome': 'not_run', 'timeout': {'occurred': False}}
        self.assertEqual(s.finish(result)['outcome'], 'not_run')

    def test_inventory_compares_paths_sizes_hashes_and_extra_files(self):
        row = self.row
        good = {'kind': 'file', 'path': 'C:/Fixture/GAME.EXE', 'size': 12, 'sha256': 'c' * 64}
        self.assertTrue(wr.compare_inventory(row, [good])['match'])
        for rows in ([{**good, 'size': 13}], [{**good, 'sha256': 'd' * 64}],
                     [good, {**good, 'path': 'C:/Other/GAME.EXE'}], [good, {**good, 'path': 'EXTRA.DLL'}]):
            self.assertFalse(wr.compare_inventory(row, rows)['match'])
        row['installed_files']['entries'][0]['name'] = 'C:/Expected/GAME.EXE'
        self.assertFalse(wr.compare_inventory(row, [good])['match'])

    def test_screen_anchor_persists_through_key_release(self):
        script = {'schema_version': 1, 'clock': 'host-monotonic-ms', 'workload': 'diablo-shareware',
                  'scene': 'default-play', 'timeout_ms': 10000, 'segments': [{'name': 'install', 'steps': [
                  {'input_id': 1, 'at_ms': 500, 'after_screen': 'welcome', 'events': [
                      {'type': 'key', 'data': {'down': True, 'key': {'type': 'qcode', 'data': 'ret'}}}]},
                  {'input_id': 2, 'at_ms': 560, 'events': [
                      {'type': 'key', 'data': {'down': False, 'key': {'type': 'qcode', 'data': 'ret'}}}]}]}]}
        p = ws.InputPlayer(script, 'f5:win32-diablo-shareware run=1234abcd', started_at_ms=0,
                           validated_fw_cfg=True, screen_time=lambda name: 1000)
        identity = {'run': '1234abcd', 'probe': 'win32-diablo-shareware'}
        p.observe({**identity, 'event': 'BEGIN'}, 0)
        p.observe({**identity, 'event': 'ARM', 'segment': 'install', 'action': 'script'}, 0)
        q = Qmp(self.directory)
        self.assertIsNone(p.tick(1499, q))
        p.tick(1500, q)
        self.assertIsNone(p.tick(1559, q))
        receipt = p.tick(1560, q)
        self.assertEqual(receipt['scheduled_host_ms'], 1560)
        self.assertEqual(receipt['anchor'], 'screen:welcome')


class RunnerIntegrationTests(unittest.TestCase):
    setUp = common.RunnerTests.setUp
    tearDown = common.RunnerTests.tearDown
    run_fake = common.RunnerTests.run_fake

    def test_blocked_case_never_creates_overlay_or_launches(self):
        self.case = json.loads((ROOT / 'tests/suites/f5-installers.json').read_text())['cases'][0]
        result, directory = self.run_fake(records=[])
        self.assertEqual(result['outcome'], 'not_run', result['reason'])
        self.assertFalse((directory / 'run.qcow2').exists())
        self.assertFalse((directory / 'fake-arguments.json').exists())
        kinds = [b['kind'] for b in result['win32']['blockers']]
        # f5-15C registered the probe; the operator program and the manifest are pending calibration.
        self.assertNotIn('probe_not_registered', kinds)
        self.assertIn('operator_program_pending', kinds)
        self.assertIn('manifest_pending', kinds)

    def test_fixture_connector_arguments(self):
        case = json.loads((ROOT / 'tests/suites/f5-titles.json').read_text())['cases'][0]
        self.assertEqual(runner.fixture_disks(case), [(0, 1)])
        args, request = runner.qemu_args('fake', self.profile, case, '1234abcd', self.root / 'run.qcow2', self.firmware)
        self.assertIn('file=fixture-0.img,format=raw,if=ide,index=1,cache=writeback', args)
        self.assertTrue(any('fixture-1.iso' in a and 'readonly=on' in a for a in args))
        self.assertIn('ide-cd,drive=ciuki-optical,bus=ide.1,unit=1', args)

    def test_all_authored_title_cases_and_exact_case_selection(self):
        sys.path.insert(0, str(ROOT / 'scripts/win32/titles'))
        import driver
        expected = {r['id'] for r in json.loads((ROOT / 'tests/win32/workloads.json').read_text())['workloads'] if r['gate'] == 'F5'}
        for tier in ('installers', 'titles'):
            suite = runner.load_suite('f5-' + tier)
            self.assertEqual({c['win32']['workload'] for c in suite['cases']}, expected)
            for case in suite['cases']:
                _, selected, plan = driver.plan(case['win32']['workload'], tier)
                self.assertEqual(selected['id'], case['id'])
                self.assertEqual(plan['command'][-2:], ['--case', case['id']])
                with self.assertRaises(wr.Blocked): wr.precheck(case)
            chosen = runner.select_cases(suite, [suite['cases'][0]['id']])
            self.assertEqual(len(chosen['cases']), 1)
            with self.assertRaises(runner.res.Refusal): runner.select_cases(suite, ['unknown'])
        output = io.StringIO()
        with patch.object(driver.subprocess, 'run') as launch, redirect_stdout(output):
            self.assertEqual(driver.main('diablo-shareware', ['--run']), 0)
        launch.assert_not_called()
        self.assertEqual(json.loads(output.getvalue())['status'], 'not_run')

    def test_installer_canned_serial_and_frames_through_real_collector(self):
        fixture = TierTests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        self.case = fixture.case('installers')
        self.case['timeout'] = 4
        script_path = fixture.script_dir / 'fixture.json'
        script = json.loads(script_path.read_text()); script['timeout_ms'] = 3500
        script_path.write_text(json.dumps(script))
        (fixture.root / 'scripts/test').mkdir(parents=True)
        (fixture.root / 'scripts/test/scope_exec.py').write_bytes((ROOT / 'scripts/test/scope_exec.py').read_bytes())
        (fixture.root / 'config').mkdir()
        (fixture.root / 'config/toolchain.json').write_bytes((ROOT / 'config/toolchain.json').read_bytes())
        records = fixture.records('installers')
        records[0]['image_sha256'] = runner.sha(self.image)
        screens = []
        for value in (1, 2):
            path = self.root / f'canned-{value}.ppm'
            path.write_bytes(b'P6\n320 300\n255\n' + frame(value)[2]); screens.append(str(path))
        def prepare(*args):
            path = args[2] / 'fixture-0.iso'
            path.write_bytes(b'authored fixture')
            return {'manifest': [{'generator': 'win32-workload', 'payload_sha256': PAYLOAD,
                                 'media': 'iso', 'path': str(path), 'sha256': runner.sha(path)}]}
        with patch.object(runner, 'ROOT', fixture.root), patch.object(runner, 'prepare_fixtures', side_effect=prepare), \
                patch.object(runner.win32_inventory, 'overlay_delta', return_value=HOST_DELTA):
            result, directory = self.run_fake(records=records[:4], screens=screens, after_inputs={
                '1': [*records[4:], '[win32] start exe=fixture pid=7', '[win32] exit pid=7 code=00000000']})
        self.assertEqual(result['outcome'], 'pass', result['reason'])
        self.assertEqual(result['win32']['verdict']['outcome'], 'pass')
        self.assertEqual(result['win32']['installed_inventory'][0]['path'], 'GAME.EXE')
        self.assertEqual(set(p.name for p in directory.iterdir()), {'serial.log', 'result.json'})


def dry_run_report(path):
    """Export synthetic parsed evidence without launching a guest or fake child."""
    report = {'status': 'dry run; QEMU run pending by the lead',
              'provenance': 'authored records and solid-colour frames; no original game bytes', 'tiers': {}}
    for tier in ('installers', 'titles'):
        fixture = TierTests()
        try:
            fixture.setUp()
            session, result, verdict = fixture.drive(tier)
            session.parser.check(session.case['expected'])
            if verdict['outcome'] != 'pass':
                raise AssertionError(verdict)
            report['tiers'][tier] = {'verdict': verdict, 'records': session.parser.records, 'evidence': result['win32']}
        finally:
            fixture.doCleanups()
    Path(path).write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--dry-run-report':
        dry_run_report(sys.argv[2])
    else:
        unittest.main()
