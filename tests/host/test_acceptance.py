"""fx-accept: the acceptance runner (scripts/test/acceptance.py), its run.py
integration, the acc-t23 suite, the site snapshots of the HTTPS fixture, the
ISO generators and the guest record formats the suite matches. Host evidence
only: no QEMU (tests/host/fake_qemu.py stands in), no image.
"""
import copy
import hashlib
import http.client
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/test'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import acceptance  # noqa: E402
import https_fixture  # noqa: E402
import iso_fixtures  # noqa: E402
import loader_model  # noqa: E402
import resources as res  # noqa: E402
import run as runner  # noqa: E402
from evidence import EvidenceNotRun  # noqa: E402
from test_normal_boot import frame as desktop_frame  # noqa: E402
from test_runner import FakeHost  # noqa: E402

D = '[user 7] [desktop]'
SCRATCH = ROOT / 'build/host/acceptance'
SCRATCH.mkdir(parents=True, exist_ok=True)
HAVE_XORRISO = shutil.which('xorriso') is not None


def struct_unpack(fmt, data, offset):
    return struct.unpack_from(fmt, data, offset)


def ppm(colour, width=320, height=320):
    return b'P6\n%d %d\n255\n' % (width, height) + bytes(colour) * (width * height)


def case(**fields):
    base = {'id': 'fixture', 'kind': 'acceptance', 'probe': 'acceptance', 'profile': 'qemu-t23', 'item': 1,
            'title': 'fixture', 'timeout': 60, 'expected': {'evidence_kind': 'acceptance'}, 'steps': []}
    base.update(fields)
    return base


def boot_lines(display='mode=280 1024x768x32 saved=0 safe=0', build='f5242070d0b4'):
    return ['L:SELECT_SOURCE=menu', 'Ciuki VMM F0 build %s - CiukiOS' % build, '[supervisor] bootstrap: spawning /Programs/desktop',
            '[supervisor] desktop pid=3 pgid=3', '%s display start %s' % (D, display)]


class FakeQMP:
    """The QMP surface Session uses: commands, input batches, events, screendumps."""
    def __init__(self, frames=None, on_key=None, parser=None):
        self.events = []; self.commands = []; self.inputs = []
        self.frames = list(frames or [ppm((0, 0, 0))]); self.shots = 0
        self.on_key = dict(on_key or {}); self.counts = {}; self.parser = parser

    def command(self, name, arguments=None):
        self.commands.append((name, arguments))
        if name == 'screendump':
            Path(arguments['filename']).write_bytes(self.frames[min(self.shots, len(self.frames) - 1)])
            self.shots += 1
        return {}

    def input_events(self, events):
        self.inputs.append(events)
        for event in events:
            if event['data']['down']:
                key = event['data']['key']['data']
                self.counts[key] = self.counts.get(key, 0) + 1
                for item in self.on_key.get('%s#%d' % (key, self.counts[key]), self.on_key.get(key, [])):
                    if isinstance(item, dict):
                        self.events.append(item)
                    else:
                        self.parser.feed(item.encode() + b'\n')

    def pressed(self):
        return [e['data']['key']['data'] for batch in self.inputs for e in batch if e['data']['down']]


def drive(session, qmp, until=30.0, step=0.05, lines=(), clock=None):
    """Tick the session on a virtual clock; lines are (time, text)."""
    pending = sorted(lines, key=lambda x: x[0]); now = 0.0
    session.parser.clock = clock
    while now < until and not session.done and not session.failed:
        while pending and pending[0][0] <= now:
            session.parser.feed(pending.pop(0)[1].encode() + b'\n')
        session.tick(now, qmp)
        now += step
    return now


class Base(unittest.TestCase):
    def setUp(self):
        SCRATCH.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=SCRATCH); self.dir = Path(self.temp.name)
        self.sleep = patch.object(acceptance.time, 'sleep', lambda s: None); self.sleep.start()

    def tearDown(self):
        self.sleep.stop(); self.temp.cleanup()

    def session(self, c, frames=None, on_key=None):
        acceptance.validate_case(c)
        s = acceptance.Session(c, '00000000', self.dir, ROOT, read_ppm=runner.read_ppm,
                               observe_screen=runner.observe_normal_boot_screen)
        return s, FakeQMP(frames, on_key, s.parser)


class KeysAndValidationTests(Base):
    def test_chords_and_us_layout_text(self):
        self.assertEqual(acceptance.chord('Ctrl+Esc'), ['ctrl', 'esc'])
        self.assertEqual(acceptance.chord('meta_l+r'), ['meta_l', 'r'])
        self.assertEqual(acceptance.chord('alt+ret'), ['alt', 'ret'])
        typed = acceptance.text_chords('"C:\\Q 3"')
        self.assertEqual(typed, [['shift', 'apostrophe'], ['shift', 'c'], ['shift', 'semicolon'], ['backslash'],
                                 ['shift', 'q'], ['spc'], ['3'], ['shift', 'apostrophe']])
        for bad in ('ctrl+ctrl', 'hyper', '', 'a+b+c+d+e'):
            with self.assertRaises(ValueError):
                acceptance.chord(bad)
        with self.assertRaises(ValueError):
            acceptance.text_chords('caff\u00e8')

    def test_templates_expand_to_the_loader_key_and_records(self):
        normal = acceptance.expand_steps([{'template': 'boot', 'prefix': 'b1', 'check': 'boot'}])
        self.assertEqual([s['name'] for s in normal], ['b1-loader', 'b1-kernel', 'b1-desktop', 'b1-display'])
        self.assertEqual(normal[0]['actions'], [{'keys': 'n'}])
        self.assertTrue(all(s['check'] == 'boot' for s in normal))
        self.assertIn('safe=0$', normal[3]['wait'])
        # sec-user-06: PID 1's desktop or the one LOGON's SESSION_LAUNCH published.
        for line in ('[supervisor] desktop pid=3 pgid=3', '[security] session 1 desktop pid=8 pgid=8 uid=1000 gid=545 groups=544,545'):
            self.assertRegex(line, normal[2]['wait'])
        safe = acceptance.expand_steps([{'template': 'boot-safe', 'prefix': 's1'}])
        self.assertEqual(safe[0]['actions'], [{'keys': 'f5'}])
        self.assertEqual(safe[1]['wait'][0], r'^L:STARTUP=3$')
        # Safe mode never logs on automatically: the blank CiukiUser logs on at Welcome.
        self.assertEqual([s['name'] for s in safe], ['s1-loader', 's1-kernel', 's1-welcome', 's1-typed', 's1-auth', 's1-desktop', 's1-display'])
        self.assertIn('safe=1$', safe[-1]['wait'])
        for bad in ({'template': 'boot', 'prefix': 'B1'}, {'template': 'nope', 'prefix': 'b1'},
                    {'template': 'boot', 'prefix': 'b1', 'name': 'x'}, {'template': 'boot', 'prefix': 'b1', 'timeout': 10}):
            with self.subTest(step=bad), self.assertRaises(res.Refusal):
                acceptance.expand_steps([bad])

    def test_validation_refuses_malformed_cases(self):
        good = case(steps=[{'name': 'a', 'wait': '^x(?P<v>\\d+)$'}, {'name': 'b', 'wait': '^y <<v>>$'}])
        acceptance.validate_case(good)
        bad = [
            case(steps=[{'name': 'a', 'wait': 'x'}]),                                    # not anchored
            case(steps=[{'name': 'a', 'wait': '^y <<v>>$'}]),                            # placeholder never captured
            case(steps=[{'name': 'a', 'wait_event': 'RESET'}]),                          # reset not armed
            case(steps=[{'name': 'a', 'wait_event': 'SHUTDOWN'}]),                       # shutdown not armed
            case(steps=[{'name': 'a', 'actions': [{'media': {'insert': 'x'}}]}]),       # undeclared medium
            case(steps=[{'name': 'a', 'wizard': {'keys': ['ret']}}]),                   # wizard without a wait
            case(steps=[{'name': 'a', 'wait': '^x', 'known_failure': {'note': 'n'}}]),  # no directive
            case(steps=[{'name': 'a', 'wait': '^x', 'known_failure': {'directive': 'd', 'on': 'never'}}]),
            case(steps=[{'name': 'a', 'requires': ['b']}, {'name': 'b'}]),              # later step
            case(steps=[{'name': 'a'}, {'name': 'a'}]),                                 # duplicate name
            case(steps=[{'name': 'a', 'bogus': 1}]),
            case(items={'1': ['nope']}, steps=[{'name': 'a'}]),
            case(gate='nope', steps=[{'name': 'a'}]),
            case(media={'std': {'generator': 'xorriso'}}, steps=[{'name': 'a'}]),     # media need an empty drive
            case(devices={'optical': 'iso'}, steps=[{'name': 'a'}]),
            case(timeout=10, steps=[{'name': 'a'}]),
            case(steps=[]),                                                             # nothing to do
            case(assertions=[{'name': 'x', 'pattern': '^x'}], steps=[{'name': 'a'}]),  # no bound
            case(assertions=[{'name': 'x', 'pattern': '^x <<nope>>', 'min': 1}], steps=[{'name': 'a'}]),
        ]
        for index, c in enumerate(bad):
            with self.subTest(index=index), self.assertRaises(res.Refusal):
                acceptance.validate_case(c)

    def test_qemu_arguments_of_acceptance_cases(self):
        profile = runner.load(ROOT / 'tests/profiles/qemu-t23.json')
        cd = case(devices={'optical': 'empty', 'network': 'i82559er'}, steps=[{'name': 'a'}])
        args, request = runner.qemu_args('qemu-system-i386', profile, cd, '12345678', self.dir / 'run.qcow2', self.dir / 'bios.bin')
        self.assertIsNone(request)
        self.assertNotIn('-fw_cfg', args)
        self.assertIn('-no-reboot', args); self.assertIn('-no-shutdown', args)
        self.assertEqual(args[args.index('-device', args.index('if=none,id=ciuki-optical,readonly=on')) + 1],
                         'ide-cd,drive=ciuki-optical,bus=ide.1,unit=0')
        self.assertIn('i82559er,netdev=ciuknet0,mac=52:54:00:12:34:56,romfile=', args)
        restart = case(resets=1, steps=[{'name': 'a', 'arm': 'reset'}, {'name': 'b', 'wait_event': 'RESET'}])
        args, _ = runner.qemu_args('qemu-system-i386', profile, restart, '12345678', self.dir / 'run.qcow2', self.dir / 'bios.bin')
        self.assertNotIn('-no-reboot', args); self.assertIn('-no-shutdown', args)
        self.assertEqual(runner.case_kind(restart), 'acceptance')


class SessionTests(Base):
    def boot_case(self, **fields):
        return case(steps=[{'template': 'boot', 'prefix': 'b1', 'check': 'boot', 'settle': 0.1},
                           {'name': 'b1-screen', 'check': 'boot', 'screen': {'geometry': [1024, 768], 'desktop': True}},
                           *fields.pop('more', [])], **fields)

    def test_normal_boot_presses_n_and_captures(self):
        c = self.boot_case(assertions=[{'name': 'no-error', 'check': 'boot', 'from': 'b1-loader', 'to': 'b1-screen', 'max': 0,
                                        'pattern': r'^\[user \d+\] \[desktop\] (?:notify|message) icon=(?:error|warning) '}],
                           figures=[{'name': 'display', 'pattern': r'^\[user \d+\] \[desktop\] display start ', 'reducer': 'first'}])
        s, qmp = self.session(c, frames=[desktop_frame()], on_key={'n': boot_lines()})
        drive(s, qmp, lines=[(0.0, 'L:CPU signature=00000673'), (0.1, 'SELECT_READY')])
        self.assertTrue(s.done, s.state)
        self.assertEqual(qmp.pressed(), ['n'])
        self.assertEqual(s.vars['b1_build'], 'f5242070d0b4')
        self.assertEqual((s.vars['b1_width'], s.vars['b1_height'], s.vars['b1_saved']), ('1024', '768', '0'))
        self.assertEqual([r['status'] for r in s.state], ['pass'] * 5)
        result = {'outcome': 'pass'}
        summary = s.finish(result)
        self.assertEqual(result['outcome'], 'pass')
        self.assertEqual(summary['checks'], {'boot': {'status': 'pass', 'items': 6}})
        self.assertEqual(summary['figures']['display']['text'], D + ' display start mode=280 1024x768x32 saved=0 safe=0')
        self.assertEqual(s.screens, ['screen-04-b1-screen.ppm'])
        self.assertEqual(summary['steps'][4]['screen']['failures'], [])

    def test_wrong_geometry_and_error_notice_fail_the_boot(self):
        c = self.boot_case(assertions=[{'name': 'no-error', 'check': 'boot', 'from': 'b1-loader', 'max': 0,
                                        'pattern': r'^\[user \d+\] \[desktop\] (?:notify|message) icon=(?:error|warning) '}])
        s, qmp = self.session(c, frames=[desktop_frame(640, 480)],
                              on_key={'n': boot_lines('mode=274 640x480x32 saved=0 safe=0') +
                                      [D + ' message icon=error title="Display Settings" text="x"']})
        drive(s, qmp, lines=[(0.0, 'SELECT_READY')])
        self.assertEqual(s.failed, 'step b1-screen: screen check failed: geometry 640x480 differs from declared 1024x768')
        result = {'outcome': 'pass'}
        summary = s.finish(result)
        self.assertEqual(result['outcome'], 'fail')
        self.assertEqual(summary['assertions'][0]['status'], 'fail')
        self.assertEqual(summary['assertions'][0]['count'], 1)

    def test_safe_template_and_wrong_menu_choice(self):
        c = case(steps=[{'template': 'boot-safe', 'prefix': 's1', 'settle': 0.1}])
        # sec-user-06: LOGON does not log on automatically in Safe mode.
        lines = ['L:STARTUP=3', 'L:SELECT_SOURCE=menu', 'Ciuki VMM F0 build f5242070d0b4 - CiukiOS',
                 '[supervisor] bootstrap: trusted logon pid=4', '[user 5] [logon] automatic logon not applicable safe=1',
                 '[user 10] [logon-ui] view mode=welcome view=list items=3 accounts=1 status=none']
        logon = ['[user 5] [logon] authenticated uid=1000; starting session', '[user 5] [logon] session 1 created uid=1000',
                 '[security] session 1 desktop pid=8 pgid=8 uid=1000 gid=545 groups=544,545',
                 '[user 5] [logon] session 1 running uid=1000 start_ms=700', D + ' display start mode=274 640x480x32 saved=0 safe=1']
        s, qmp = self.session(c, on_key={'f5': lines, 'ret#1': ['[user 10] [logon-ui] view mode=welcome view=typed items=5 accounts=1 status=none'],
                                         'ret#3': logon})
        drive(s, qmp, until=60, lines=[(0.0, 'SELECT_READY')])
        self.assertTrue(s.done, s.state)
        self.assertEqual(qmp.pressed()[:4], ['f5', 'up', 'up', 'ret'])
        self.assertEqual(qmp.pressed().count('ret'), 3)   # list, name (empty password follows), submit
        c = case(steps=[{'template': 'boot', 'prefix': 'b1', 'settle': 0.1}])
        s, qmp = self.session(c, on_key={'n': ['L:SAFE=1'] + boot_lines()})
        drive(s, qmp, lines=[(0.0, 'SELECT_READY')])
        self.assertIn('failure line', s.failed)

    def test_failure_kinds_known_failures_and_continue(self):
        steps = [{'name': 'enter', 'check': 'fs', 'wait': '^entered$', 'fail': '^refused$', 'timeout': 2,
                  'known_failure': {'directive': 'f3-29', 'on': 'pattern'}, 'on_fail': 'continue'},
                 {'name': 'back', 'check': 'fs', 'requires': ['enter'], 'wait': '^back$'},
                 {'name': 'after', 'check': 'main', 'wait': '^after$'}]
        s, qmp = self.session(case(steps=steps))
        drive(s, qmp, lines=[(0.0, 'refused'), (0.5, 'after')])
        self.assertTrue(s.done)
        self.assertEqual([r['status'] for r in s.state], ['known_fail', 'not_run', 'pass'])
        result = {'outcome': 'pass'}
        summary = s.finish(result)
        self.assertEqual(result['outcome'], 'pass')
        self.assertEqual(summary['checks']['fs']['status'], 'known_fail')
        self.assertEqual(acceptance.outcome_label({'outcome': 'pass', 'evidence': result}), 'PASS*')
        # A timeout is not the known refusal: it fails, and the script goes on.
        s, qmp = self.session(case(steps=steps))
        drive(s, qmp, lines=[(0.0, 'after')], until=10)
        self.assertEqual([r['status'] for r in s.state], ['fail', 'not_run', 'pass'])
        self.assertEqual(s.state[0]['failure_kind'], 'timeout')
        result = {'outcome': 'pass'}
        s.finish(result)
        self.assertEqual((result['outcome'], result['reason']), ('fail', 'acceptance checks failed: fs'))
        # A required step without on_fail stops the script.
        s, qmp = self.session(case(steps=[{'name': 'a', 'wait': '^a$', 'fail': '^bad$'}, {'name': 'b', 'wait': '^b$'}]))
        drive(s, qmp, lines=[(0.0, 'bad'), (0.1, 'a'), (0.2, 'b')])
        self.assertIn('failure line 1: bad', s.failed)
        self.assertEqual([r['status'] for r in s.state], ['fail', 'not_run'])
        # A step that is not required records an optional failure only.
        s, qmp = self.session(case(steps=[{'name': 'a', 'wait': '^a$', 'timeout': 1, 'required': False}, {'name': 'b', 'wait': '^b$'}]))
        drive(s, qmp, lines=[(0.0, 'b')], until=5)
        result = {'outcome': 'pass'}
        summary = s.finish(result)
        self.assertEqual(result['outcome'], 'pass')
        self.assertEqual(summary['checks']['main'], {'status': 'pass', 'items': 2, 'optional_failures': 1})

    def test_fallback_and_unless(self):
        steps = [{'name': 'menu', 'check': 'game', 'wait': '^start (?P<pid>\\d+)$', 'timeout': 1, 'on_fail': 'continue'},
                 {'name': 'run', 'check': 'diag', 'unless': ['menu'], 'required': False, 'actions': [{'keys': 'f1'}]},
                 {'name': 'run-start', 'check': 'diag', 'unless': ['menu'], 'required': False, 'wait': '^start (?P<pid>\\d+)$'},
                 {'name': 'frame', 'check': 'game', 'wait': '^frame <<pid>>$', 'timeout': 2,
                  'fallback': {'actions': [{'keys': 'f2'}], 'timeout': 2, 'counts_as': 'pass'}}]
        s, qmp = self.session(case(steps=steps), on_key={'f1': ['start 9'], 'f2': ['frame 9']})
        drive(s, qmp, until=10)
        self.assertTrue(s.done)
        self.assertEqual(qmp.pressed(), ['f1', 'f2'])
        self.assertEqual([r['status'] for r in s.state], ['fail', 'pass', 'pass', 'pass'])
        self.assertTrue(s.state[3]['fallback_used'])
        # The Start menu path worked: the Run fallback is skipped and not counted.
        s, qmp = self.session(case(steps=steps), on_key={'f1': ['start 9']})
        drive(s, qmp, lines=[(0.0, 'start 4'), (0.1, 'frame 4')])
        self.assertEqual([r['status'] for r in s.state], ['pass', 'skipped', 'skipped', 'pass'])
        result = {'outcome': 'pass'}
        summary = s.finish(result)
        self.assertNotIn('diag', summary['checks'])
        # A fallback that counts as a failure stops a required step.
        steps[3]['fallback']['counts_as'] = 'fail'
        s, qmp = self.session(case(steps=steps), on_key={'f2': ['frame 4']})
        drive(s, qmp, lines=[(0.0, 'start 4')], until=10)
        self.assertEqual(s.failed, 'step frame passed only through its fallback')

    def test_events_resets_and_shutdown(self):
        steps = [{'name': 'restart', 'arm': 'reset', 'actions': [{'keys': 'r'}]},
                 {'name': 'reset', 'wait_event': 'RESET', 'timeout': 5},
                 {'name': 'boot2', 'wait': '^SELECT_READY$', 'actions': [{'keys': 's'}], 'arm': 'shutdown'},
                 {'name': 'off', 'wait_event': 'SHUTDOWN', 'timeout': 5}]
        reset = {'event': 'RESET', 'data': {'guest': True, 'reason': 'guest-reset'}}
        off = {'event': 'SHUTDOWN', 'data': {'guest': True, 'reason': 'guest-shutdown'}}
        s, qmp = self.session(case(resets=1, steps=steps), on_key={'r': ['L:CPU signature=1', 'SELECT_READY', reset], 's': [off]})
        drive(s, qmp)
        self.assertTrue(s.done, s.state)
        self.assertEqual([o['event'] for o in s.observations], ['RESET', 'SHUTDOWN'])
        self.assertEqual(s.parser.boot, 1)
        for events, why in (([reset], 'unexpected guest reset'),
                            ([{'event': 'RESET', 'data': {'guest': False, 'reason': 'host-qmp-system-reset'}}], 'unexpected reset'),
                            ([{'event': 'SHUTDOWN', 'data': {'guest': True, 'reason': 'guest-reset'}}], 'unexpected shutdown')):
            s, qmp = self.session(case(steps=[{'name': 'a', 'wait': '^never$', 'timeout': 5}]))
            qmp.events.extend(events)
            drive(s, qmp, until=1)
            self.assertIn(why, s.failed)
        # Two resets for a case that declares one.
        s, qmp = self.session(case(resets=1, steps=steps), on_key={'r': [reset, reset]})
        drive(s, qmp)
        self.assertIn('unexpected guest reset', s.failed)

    def test_forbidden_lines_and_abandon(self):
        s, qmp = self.session(case(steps=[{'name': 'a', 'wait': '^never$', 'timeout': 50}], forbid='^\\[desktop\\] bad'))
        for line in ('ok', 'Ciuki VMM panic: halting.', '[desktop] bad'):
            s.parser.feed(line.encode() + b'\n')
        self.assertEqual(s.parser.failure['line'], 2)
        self.assertRaises(acceptance.EvidenceError, s.parser.check)
        s.tick(0.0, qmp)
        s.abandon('forbidden serial line 2')
        self.assertEqual(s.state[0]['status'], 'fail')
        self.assertEqual(s.failed, 'forbidden serial line 2')

    def test_wizard_presses_only_on_installer_pages_and_closes_files(self):
        focus = D + ' focus pid=%s window=%d titled=%d internal=%d app=%s'
        steps = [{'name': 'start', 'wait': '^\\[win32\\] start pid=(?P<setup>\\d+) '},
                 {'name': 'install', 'wait': '^\\[win32\\] exit pid=<<setup>> code=\\S+ reason=normal ', 'timeout': 600,
                  'wizard': {'keys': ['ret', 'y'], 'then': 'ret', 'idle': 6, 'max': 5,
                             'focus': r'^\[user \d+\] \[desktop\] focus ',
                             'press_when': r'^\[user \d+\] \[desktop\] focus pid=<<setup>> window=\d+ titled=0 internal=0 ',
                             'close_when': r'^\[user \d+\] \[desktop\] focus pid=\d+ window=\d+ titled=1 internal=0 app=files$',
                             'close_keys': ['alt+f4']}}]
        s, qmp = self.session(case(steps=steps), frames=[ppm((10, 10, 10)), ppm((20, 20, 20))],
                              on_key={'ret#1': [focus % (12, 2, 0, 0, '-')],               # welcome -> licence
                                      'y#1': [focus % (12, 3, 1, 0, '-')],                 # licence -> titled progress
                                      'alt#1': [focus % (12, 4, 0, 0, '-')],               # Files closed -> last page
                                      'ret#2': ['[win32] exit pid=12 code=00000000 reason=normal peak_commit=1']})
        lines = [(0.0, '[win32] start pid=12 exe=C:\\Q3ADemo.exe pe=1:2 shims='), (0.5, focus % (12, 1, 0, 0, '-')),
                 (40.0, focus % (14, 9, 1, 0, 'files'))]
        drive(s, qmp, until=200, lines=lines)
        self.assertTrue(s.done, s.state)
        # Enter, Y, then nothing while the titled progress window holds the
        # focus, Alt+F4 on the Files window, Enter on the last page.
        self.assertEqual(qmp.pressed(), ['ret', 'y', 'alt', 'f4', 'ret'])
        log = s.state[1]['wizard']
        self.assertEqual([e['kind'] for e in log], ['press', 'press', 'close', 'press'])
        self.assertTrue(all(e['idle'] >= 6 for e in log))
        self.assertGreaterEqual(log[2]['t'], 40)
        self.assertFalse((self.dir / acceptance.PACE_FILE).exists())

    def quake3_wizard(self):
        suite = json.loads((ROOT / 'tests/suites/acc-t23.json').read_text())
        quake = next(c for c in suite['cases'] if c['id'] == 'acc-quake3')
        return copy.deepcopy(next(s['wizard'] for s in quake['steps'] if s.get('name') == 'install'))

    def page_case(self, wizard):
        return case(steps=[{'name': 'start', 'wait': '^\\[win32\\] start pid=(?P<setup>\\d+) '},
                           {'name': 'install', 'wait': '^\\[win32\\] exit pid=<<setup>> code=\\S+ reason=normal ', 'timeout': 3000,
                            'wizard': wizard}])

    WINDOW = D + ' window pid=%d window=%d owner=%d modal=%d state=%s title=%s'
    FOCUS = D + ' focus pid=%d window=%d titled=%d internal=0 app=%s'

    def page(self, window, title, focus=True, modal=1, owner=3):
        """A DialogBox page as Installer VISE shows it (run c634b7c6): created
        untitled, hidden, titled in WM_INITDIALOG, then shown."""
        W = self.WINDOW
        lines = [W % (12, window, owner, modal, 'shown', '-'), W % (12, window, owner, modal, 'hidden', '-'),
                 W % (12, window, owner, modal, 'hidden', title), W % (12, window, owner, modal, 'shown', title)]
        return lines + ([self.FOCUS % (12, window, 1, title)] if focus else [])

    def test_page_wizard_follows_the_installer_pages_by_title(self):
        """fx-acc1 (acceptance run c634b7c6): the Welcome page stayed 3000 s
        because the focus-driven wizard pressed only while an untitled
        window held the focus; the page, titled before it was shown, never
        did (the focus was on the setup frame). The suite's own Quake III
        page specs: Enter, Y on the licence, Enter, nothing on the progress
        window, Alt+F4 on a Files window, Enter on Finished."""
        W, F = self.WINDOW, self.FOCUS
        closed = lambda window, title: W % (12, window, 3, 1, 'closed', title)
        s, qmp = self.session(self.page_case(self.quake3_wizard()), frames=[ppm((10, 10, 10)), ppm((20, 20, 20))], on_key={
            # Welcome -> licence: the page closes, the frame holds the focus, the next page appears.
            'ret#1': [closed(4, 'Welcome'), F % (12, 3, 1, 'Quake_3_Arena_Demo_Setup'), *self.page(5, 'Software_License_Agreement')],
            'y#1': [closed(5, 'Software_License_Agreement'), *self.page(6, 'Choose_Destination_Location')],
            'ret#2': [closed(6, 'Choose_Destination_Location'), *self.page(7, 'Start_Installation')],
            # The progress window: owned, modeless, titled at creation.
            'ret#3': [closed(7, 'Start_Installation'), W % (12, 8, 3, 0, 'shown', 'IDS_TITLE'), F % (12, 8, 1, 'IDS_TITLE'),
                      W % (12, 8, 3, 0, 'shown', 'Setup')],
            'alt#1': [F % (12, 3, 1, 'Quake_3_Arena_Demo_Setup'), *self.page(9, 'Finished')],
            'ret#4': ['[win32] exit pid=12 code=00000000 reason=normal peak_commit=1']})
        lines = [(0.0, '[win32] start pid=12 exe=C:\\Q3ADemo.exe pe=1:2 shims='),
                 (0.5, W % (12, 2, 0, 0, 'shown', 'IDS_TITLE')), (1.0, W % (12, 2, 0, 0, 'closed', 'IDS_TITLE')),
                 (1.5, W % (12, 3, 0, 0, 'shown', 'Quake_3_Arena_Demo_Setup')), (1.5, F % (12, 3, 1, 'Quake_3_Arena_Demo_Setup')),
                 # The welcome page; the old image left the focus on the frame (window 3), same process.
                 *[(2.0, line) for line in self.page(4, 'Welcome', focus=False)],
                 # The copy ends: the progress window closes, VISE opens a Files window over the setup.
                 (300.0, W % (12, 8, 3, 0, 'closed', 'Setup')), (300.5, F % (14, 20, 1, 'files'))]
        drive(s, qmp, until=600, lines=lines)
        self.assertTrue(s.done, (s.failed, s.state[1]))
        self.assertEqual(qmp.pressed(), ['ret', 'y', 'ret', 'ret', 'alt', 'f4', 'ret'])
        log = s.state[1]['wizard']
        self.assertEqual([(e['kind'], e['page']) for e in log],
                         [('press', 'welcome'), ('press', 'licence'), ('press', 'destination'), ('press', 'page'),
                          ('close', None), ('press', 'finished')])
        self.assertTrue(all(e['idle'] >= 6 for e in log))
        pages = [(e['page'], e['title']) for e in s.state[1]['pages']]
        self.assertIn(('welcome', 'Welcome'), pages)
        # The progress window retitled in the same turn stays one page.
        self.assertEqual([p for p in pages if p[0] == 'copy'], [('copy', 'Setup')])
        self.assertEqual(pages[-1], ('finished', 'Finished'))
        self.assertGreaterEqual(log[4]['t'], 300)   # nothing pressed while the progress window was up

    def test_page_wizard_names_the_page_it_stopped_on(self):
        s, qmp = self.session(self.page_case(self.quake3_wizard()), frames=[ppm((10, 10, 10))])
        lines = [(0.0, '[win32] start pid=12 exe=C:\\Q3ADemo.exe pe=1:2 shims='),
                 (1.0, self.WINDOW % (12, 3, 0, 0, 'shown', 'Quake_3_Arena_Demo_Setup')),
                 (1.0, self.FOCUS % (12, 3, 1, 'Quake_3_Arena_Demo_Setup')),
                 *[(2.0, line) for line in self.page(4, 'Welcome', focus=False)]]
        drive(s, qmp, until=400, lines=lines)
        self.assertTrue(s.failed)
        self.assertIn('wizard stopped on page welcome (title Welcome, window 4) for 30', s.state[1]['reason'])
        self.assertEqual(s.state[1]['failure_kind'], 'timeout')
        # One press, then the retries 60 s apart (max_retries 2).
        self.assertEqual(qmp.pressed(), ['ret', 'ret', 'ret'])
        times = [e['t'] for e in s.state[1]['wizard']]
        self.assertGreaterEqual(times[1] - times[0], 60)

    def test_page_wizard_presses_only_while_the_installer_holds_the_focus(self):
        """The desktop (pid 0) or another program holding the focus gets no
        installer key; a page past its timeout with no page at all is named
        by the page before it."""
        wizard = self.quake3_wizard()
        s, qmp = self.session(self.page_case(wizard), frames=[ppm((10, 10, 10))])
        lines = [(0.0, '[win32] start pid=12 exe=C:\\Q3ADemo.exe pe=1:2 shims='),
                 *[(2.0, line) for line in self.page(4, 'Welcome', focus=False)],
                 (2.1, self.FOCUS % (0, 0, 0, 'desktop'))]
        drive(s, qmp, until=200, lines=lines)
        self.assertEqual(qmp.pressed(), [])
        s, qmp = self.session(self.page_case(wizard), frames=[ppm((10, 10, 10))],
                              on_key={'ret#1': [self.WINDOW % (12, 4, 3, 1, 'closed', 'Welcome')]})
        lines = [(0.0, '[win32] start pid=12 exe=C:\\Q3ADemo.exe pe=1:2 shims='),
                 *[(2.0, line) for line in self.page(4, 'Welcome')]]
        drive(s, qmp, until=400, lines=lines)
        self.assertTrue(s.failed)
        self.assertIn('no installer page shown after page welcome (title Welcome, window 4)', s.state[1]['reason'])

    def test_page_wizard_validation(self):
        wizard = self.quake3_wizard()
        for change, message in (({'pid': None}, 'pid'), ({'pid': 'other'}, 'not captured'), ({'keys': ['ret']}, 'replace'),
                                ({'pages': []}, 'page specs'), ({'pages': [{'name': 'a', 'title': '('}]}, 'title expression'),
                                ({'pages': [{'name': 'a', 'title': 'x', 'keys': ['nokey']}]}, 'unknown key'),
                                ({'pages': [{'name': 'a', 'title': 'x'}, {'name': 'a', 'title': 'y'}]}, 'unique'),
                                ({'retry': 1}, 'retry'), ({'max_retries': 9}, 'max_retries')):
            spec = {**wizard, **change}
            spec = {k: v for k, v in spec.items() if v is not None}
            with self.subTest(change=change):
                with self.assertRaises(res.Refusal) as caught:
                    acceptance.validate_case(self.page_case(spec))
                self.assertIn(message, str(caught.exception))

    def test_wizard_waits_for_a_still_screen(self):
        steps = [{'name': 'install', 'wait': '^done$', 'timeout': 60,
                  'wizard': {'keys': ['ret'], 'idle': 6, 'max': 1}}]
        frames = [ppm((i, 0, 0)) for i in range(30)]      # a changing progress page for 30 polls
        s, qmp = self.session(case(steps=steps), frames=frames, on_key={'ret': ['done']})
        drive(s, qmp, until=60)
        self.assertTrue(s.done)
        self.assertGreaterEqual(s.state[0]['wizard'][0]['t'], 35)

    def test_screen_settle_waits_for_a_change_then_stillness(self):
        steps = [{'name': 'open', 'actions': [{'keys': 'meta_l+r'}], 'screen_settle': {'changed': True, 'stable': 1.5, 'timeout': 30}},
                 {'name': 'typed', 'actions': [{'text': 'ab'}]}]
        frames = [ppm((1, 1, 1))] * 3 + [ppm((2, 2, 2))] * 2 + [ppm((3, 3, 3))]
        s, qmp = self.session(case(steps=steps), frames=frames)
        end = drive(s, qmp, until=30)
        self.assertTrue(s.done)
        self.assertEqual(s.state[0]['screen_settle'], 'stable')
        self.assertEqual(qmp.pressed(), ['meta_l', 'r', 'a', 'b'])
        self.assertLess(end, 10)
        self.assertEqual(s.state[1]['stimuli'], [{'t': s.state[1]['stimuli'][0]['t'], 'typed': 2}])

    def test_windows_assertions_figures_and_perf(self):
        perf = '[dosvm] perf vm=1 gen=3 exits=%d gp=1 pf=2 db=0 timer=5 other=0 pm=0 busy_pm=%d idle_2f=0 idle_28=0 idle_16=0 idle_hlt=0'
        steps = [{'name': 'graphics', 'wait': ['^\\[dosvm-window\\] video vm=(?P<g_vm>\\d+) gen=(?P<g_gen>\\d+) vbe=0 width=320 height=200 text=0$',
                                              '^\\[dosvm\\] perf vm=<<g_vm>> gen=<<g_gen>> exits=[1-9]\\d* ']},
                 {'name': 'exit', 'wait': '^end$'}, {'name': 'later', 'wait': '^later$', 'timeout': 1, 'required': False},
                 {'name': 'never', 'requires': ['later']}]
        c = case(steps=steps,
                 assertions=[{'name': 'mode', 'pattern': '^\\[dosvm-window\\] video vm=<<g_vm>> gen=<<g_gen>> vbe=0 width=320 ',
                              'from': 'graphics', 'to': 'exit', 'min': 1, 'max': 1},
                             {'name': 'skipped-window', 'pattern': '^x', 'from': 'never', 'max': 0}],
                 figures=[{'name': 'perf', 'pattern': '^\\[dosvm\\] perf(?:_port|_cost)? vm=<<g_vm>> ', 'from': 'graphics', 'to': 'exit',
                           'reducer': 'dosvm_perf'}])
        s, qmp = self.session(c)
        lines = ['[dosvm-window] video vm=1 gen=3 vbe=0 width=720 height=400 text=1', perf % (0, 0),
                 '[dosvm-window] video vm=1 gen=3 vbe=0 width=320 height=200 text=0', perf % (900, 950),
                 '[dosvm] perf_port vm=1 port=03da exits=500 us=700', '[dosvm] perf_cost vm=1 cause=vga_io exits=500 us=900',
                 perf % (1100, 990), 'end', perf % (5, 10)]
        drive(s, qmp, lines=[(i * 0.1, line) for i, line in enumerate(lines)], until=10)
        result = {'outcome': 'pass'}
        summary = s.finish(result)
        figure = summary['figures']['perf']
        self.assertEqual((figure['samples'], figure['exits_median'], figure['busy_pm_max']), (3, 900, 990))
        self.assertEqual(figure['top_ports'], [{'port': '03da', 'exits': 500, 'us': 700}])
        self.assertEqual(figure['top_costs'], [{'cause': 'vga_io', 'exits': 500, 'us': 900}])
        self.assertEqual([a['status'] for a in summary['assertions']], ['pass', 'not_run'])
        self.assertEqual(result['outcome'], 'pass')

    def test_pending_case_is_not_run(self):
        s, _ = self.session(case(pending={'directive': 'fx-is5boot', 'requires': ['no/such/file'], 'reason': 'later'}))
        with self.assertRaises(EvidenceNotRun) as caught:
            s.precheck()
        self.assertIn('pending fx-is5boot: later (missing no/such/file)', str(caught.exception))

    @unittest.skipUnless(HAVE_XORRISO, 'xorriso required')
    def test_media_placeholders_and_qmp_media_actions(self):
        c = case(devices={'optical': 'empty'},
                 media={'std': {'generator': 'xorriso', 'level': 3, 'label': 'CIUKI_STD'},
                        'vs0': {'generator': 'xorriso', 'level': 3, 'volume_sequence': 0}},
                 steps=[{'name': 'insert', 'actions': [{'media': {'insert': 'vs0'}}]},
                        {'name': 'listed', 'wait': '^probe entries=<<media_vs0_root>>$'},
                        {'name': 'eject', 'actions': [{'media': {'eject': True}}]}])
        s, qmp = self.session(c)
        prepared = s.prepare()
        self.assertEqual([m['name'] for m in prepared], ['std', 'vs0'])
        self.assertEqual(prepared[1]['volume_sequence'], 0)
        self.assertEqual(s.vars['media_vs0_root'], str(len(prepared[1]['root_entries'])))
        self.assertEqual(prepared[1]['root_entries'], ['/DATA', '/LONG_DIRECTORY', '/README.TXT'])
        drive(s, qmp, lines=[(0.5, 'probe entries=3')])
        self.assertTrue(s.done)
        media = [c for c in qmp.commands if c[0] in ('blockdev-change-medium', 'eject')]
        self.assertEqual(media[0], ('blockdev-change-medium', {'device': 'ciuki-optical', 'filename': prepared[1]['path'],
                                                               'format': 'raw', 'read-only-mode': 'read-only'}))
        self.assertEqual(media[1], ('eject', {'device': 'ciuki-optical', 'force': True}))
        self.assertEqual(s.media_unchanged(), [])
        Path(prepared[0]['path']).write_bytes(b'changed')
        self.assertEqual(s.media_unchanged(), ['std'])


class DiskAndSummaryTests(unittest.TestCase):
    SPEC = {'check': 'disk', 'menu_link_target': 'quake3.exe',
            'files': [{'path': r'C:\Q3Ademo\quake3.exe', 'size': 3, 'sha256': 'ab'}, {'path': r'C:\Q3Ademo\demoq3\pak0.pk3', 'size': 9}]}

    def test_disk_check(self):
        delta = {'added': [{'path': r'C:\Q3ADEMO\QUAKE3.EXE', 'kind': 'file', 'size': 3, 'sha256': 'ab'},
                           {'path': r'C:\Q3Ademo\demoq3\pak0.pk3', 'kind': 'file', 'size': 9, 'sha256': 'cd'}],
                 'links': [{'path': r'C:\WINDOWS\Start Menu\Programs\Quake 3 Arena Demo\quake3.lnk', 'target': r'C:\Q3Ademo\quake3.exe'}],
                 'menu_links': [r'C:\WINDOWS\Start Menu\Programs\Quake 3 Arena Demo\quake3.lnk']}
        self.assertEqual(acceptance.disk_check(self.SPEC, delta)['status'], 'pass')
        for change in ({'menu_links': []}, {'added': delta['added'][:1]},
                       {'added': [{**delta['added'][0], 'sha256': 'zz'}, delta['added'][1]]}):
            with self.subTest(change=list(change)):
                self.assertEqual(acceptance.disk_check(self.SPEC, {**delta, **change})['status'], 'fail')
        self.assertEqual(acceptance.disk_check(self.SPEC, {'error': 'no overlay'})['status'], 'fail')

    def rows(self):
        def acc(item, checks, items=None, title='t'):
            return {'item': item, 'title': title, 'blocking': True, 'checks': {k: {'status': v, 'items': 1} for k, v in checks.items()},
                    'steps': [], 'assertions': [], 'figures': {}, **({'items': items} if items else {})}
        return [
            {'case': 'acc-desktop-cd', 'outcome': 'pass', 'result': '/nonexistent/a/result.json',
             'evidence': {'acceptance': acc(1, {'boot': 'pass', 'cd': 'pass', 'cd-empty': 'known_fail'},
                                            {'1': {'checks': ['boot'], 'status': 'pass'},
                                             '4': {'checks': ['cd', 'cd-empty'], 'status': 'known_fail'}}),
                          'image': {'sha256': 'ab' * 32}, 'build_manifest': {'revision': 'r1', 'dirty': False},
                          'timing': {'host_monotonic_elapsed_seconds': 301.2}}},
            {'case': 'acc-dos-games', 'outcome': 'fail', 'evidence': {'reason': 'acceptance checks failed: doom',
                                                                     'acceptance': acc(3, {'doom': 'fail'})}},
            {'case': 'acc-netsurf-site-1', 'item': 5, 'title': 'site', 'outcome': 'not_run',
             'evidence': {'reason': 'site snapshots absent: 0 of 2'}},
            {'case': 'acc-installshield5', 'item': 9, 'title': 'is5', 'outcome': 'not_run',
             'evidence': {'reason': 'pending fx-is5boot: later'}},
        ]

    def test_summary_page_and_verdicts(self):
        rows = self.rows()
        rows[1]['evidence']['acceptance']['figures'] = {
            'doom-window-perf': {'samples': 12, 'exits_median': 900, 'exits_max': 1500, 'busy_pm_median': 950,
                                 'top_ports': [{'port': '03da', 'exits': 4000, 'us': 9000}], 'top_costs': [{'cause': 'vga_io', 'exits': 4000, 'us': 9000}]},
            'doom-end': {'count': 1, 'lines': [{'line': 3, 'text': 'diagnostic only'}], 'page': False}}
        with tempfile.TemporaryDirectory(dir=SCRATCH) as temp:
            text = acceptance.write_summary('acc-t23', rows, Path(temp) / 'summary.txt')
            self.assertEqual((Path(temp) / 'summary.txt').read_text(), text)
        self.assertIn('image ' + 'ab' * 32 + '  build r1', text)
        self.assertIn('verdict BLOCKED: FAIL 1, NOT_RUN 2, PASS* 1', text)
        self.assertIn('blocking failures: acc-dos-games', text)
        self.assertIn('not run: acc-netsurf-site-1', text)
        self.assertIn('items: 1 PASS  3 FAIL  4 PASS*  5 NOT_RUN  9 NOT_RUN', text)
        self.assertIn('[1,4] PASS*    acc-desktop-cd - t (301 s)', text)
        self.assertIn('      item 4 PASS* (cd, cd-empty)', text)
        self.assertIn('reason: acceptance checks failed: doom', text)
        self.assertIn('doom-window-perf: samples=12 exits med=900 max=1500 busy_pm med=950 ports 03da:4000/9000us cost vga_io:9000us', text)
        self.assertNotIn('diagnostic only', text)
        self.assertIn('evidence: /nonexistent/a/serial.log', text)
        self.assertEqual(acceptance.verdict(rows[:1]), ('ACCEPTED', [], []))
        self.assertEqual(acceptance.verdict([rows[0], rows[3]])[0], 'ACCEPTED')      # a pending item does not block
        self.assertEqual(acceptance.verdict([rows[0], rows[2]])[0], 'INCOMPLETE')
        self.assertEqual(acceptance.verdict([])[0], 'INCOMPLETE')
        nonblocking = dict(rows[1], blocking=False)
        self.assertEqual(acceptance.verdict([rows[0], nonblocking])[0], 'ACCEPTED')

    def test_summarize_command(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as temp:
            path = Path(temp) / 'summary.json'
            path.write_text(json.dumps({'suite': 'acc-t23', 'cases': self.rows()}))
            with patch('sys.stdout'):
                self.assertEqual(acceptance.main(['summarize', str(path)]), 0)
            self.assertIn('verdict BLOCKED', (Path(temp) / 'summary.txt').read_text())


class SuiteTests(unittest.TestCase):
    """tests/suites/acc-t23.json: the owner's list, in order, fully declared."""
    @classmethod
    def setUpClass(cls):
        cls.suite = runner.load_suite('acc-t23')
        cls.raw = json.loads((ROOT / 'tests/suites/acc-t23.json').read_text())

    def test_runtime_session_startup_and_first_win32_predicates(self):
        case = next(c for c in self.raw['cases'] if c['id'] == 'acc-desktop-cd')
        acceptance.validate_case(case)
        assertions = {a['name']: a for a in case['assertions']}
        records = {
            'boot-logon-session': '[user 35] [logon] session 1 running uid=1000 start_ms=1224',
            'boot-startup-snapshot': '[console] startup: snapshot ready entries=2',
            'boot-first-win32': '[win32] start pid=52 exe=C:\\WINDOWS\\CDPLAYER.EXE pe=00000000:00010000 shims=',
        }
        for name, line in records.items():
            pattern = assertions[name]['pattern'].replace('<<boot_win32>>', '52')
            self.assertIsNotNone(re.search(pattern, line), name)
        self.assertIsNone(re.search(assertions['boot-startup-snapshot']['pattern'], '[console] startup: snapshot failed: 32'))
        steps = {s.get('name'): s for s in case['steps']}
        self.assertEqual(steps['b1-session-startup']['since'], 'boot')
        self.assertEqual(steps['b1-win32-command']['actions'][0]['text'], r'C:\WINDOWS\CDPLAYER.EXE')
        self.assertEqual(steps['b1-win32-window']['actions'], [{'keys': 'alt+f4'}])

    def test_accounts_autologon_keeps_the_first_boot_runtime_checks(self):
        boot = next(c for c in self.raw['cases'] if c['id'] == 'acc-desktop-cd')
        accounts = next(c for c in self.raw['cases'] if c['id'] == 'acc-accounts')
        acceptance.validate_case(accounts)
        steps = {s.get('name'): s for s in accounts['steps']}
        for step in boot['steps']:
            if step.get('name', '').startswith(('b1-session-', 'b1-win32-')):
                self.assertEqual(steps[step['name']], {**step, 'check': 'autologon'})
        assertions = {a['name']: a for a in accounts['assertions']}
        for name in ('boot-logon-session', 'boot-startup-snapshot', 'boot-first-win32'):
            self.assertEqual(assertions[name]['check'], 'autologon')
        pattern = assertions['boot-logon-session']['pattern'].replace('<<b1_session>>', '1')
        self.assertRegex('[user 35] [logon] session 1 running uid=1000 start_ms=1224', pattern)
        self.assertNotRegex('[user 35] [logon] session 2 running uid=1001 start_ms=2400', pattern)

    def test_the_owner_list_in_order(self):
        self.assertTrue(self.suite['acceptance'])
        self.assertNotIn('prerequisites', self.raw)
        ids = [c['id'] for c in self.suite['cases']]
        original = ['acc-desktop-cd', 'acc-quake3', 'acc-dos-games', 'acc-netsurf-site-1', 'acc-netsurf-site-2',
                    'acc-safe-restart-shutdown', 'acc-installshield5']
        self.assertEqual(ids[:len(original)], original)
        additions = ids[len(original):]
        self.assertIn('acc-accounts', additions)
        self.assertEqual(len([i for i in additions if i.startswith('acc-dos-')]), 20)
        self.assertGreaterEqual(len([i for i in additions if i.startswith('acc-ipx-')]), 6)
        covered = set()
        for c in self.suite['cases']:
            covered |= {int(n) for n in c.get('items', {str(c['item']): []})}
            self.assertEqual(c['profile'], 'qemu-t23')
            self.assertTrue(c['title'])
        self.assertEqual(covered, set(range(1, 19)))   # sec-user-06: items 10..18 are the account trials
        self.assertEqual(self.suite['cases'][0]['gate'], 'boot')

    def test_cases_are_valid_and_bounded(self):
        total = 0
        for c in self.suite['cases']:
            with self.subTest(case=c['id']):
                kind = runner.case_kind(c)
                total += c['timeout']
                if kind == 'acceptance':
                    if 'guests' in c:
                        self.assertEqual(len(c['guests']), 2)
                        for guest in c['guests']:
                            self.assertEqual(runner.case_kind(guest), 'acceptance')
                            self.assertEqual(guest['steps'][0]['template'], 'boot')
                        continue
                    steps = acceptance.expand_steps(c['steps'])
                    if c['id'].startswith('acc-install-'):
                        self.assertTrue(steps)
                        self.assertEqual(c['pending']['directive'], 'fx-games20c')
                        self.assertEqual(c['pending']['requires'], [])
                    else:
                        self.assertEqual(bool(steps), not c.get('pending'))
                    if steps:
                        self.assertIn(c['steps'][0].get('template'), ('boot', 'boot-safe'))
                    for step in steps:
                        for action in step.get('actions', []):
                            if 'keys' in action:
                                for k in (action['keys'] if isinstance(action['keys'], list) else [action['keys']]):
                                    acceptance.chord(k)
                else:
                    self.assertEqual(kind, 'probe')
                    request = c['selector'].format(run_id='12345678')
                    self.assertLessEqual(len(request), 64)
                    parsed = loader_model.selector(request, 'fw_cfg', True)
                    self.assertEqual((parsed['phase'], parsed['probe']), (6, 'browser'))
                    self.assertTrue(runner.browser_case(c))
                    self.assertEqual(c['device_exceptions'], {'network': 'i82559er'})
        # fx-games20 adds twenty independent games and their bounded LAN pairs.
        extra = [c for c in self.suite['cases'] if 'dos_game' in c or 'guests' in c or c['id'].startswith('acc-install-')]
        self.assertTrue(all(c['timeout'] <= 2400 for c in extra))
        self.assertLessEqual(total, 7200 * 4 + 2400 * len(extra))

    def test_no_site_host_names_in_tracked_files(self):
        # The owner's site list is private: snapshots are served by number.
        text = (ROOT / 'tests/suites/acc-t23.json').read_text()
        source = ROOT / 'build/downloads/ns-sites-2026-10-26'
        hosts = [p.name for p in source.iterdir()] if source.is_dir() else []
        for host in hosts:
            self.assertNotIn(host, text)
        for c in self.raw['cases']:
            if 'browser_sites' in c:
                self.assertEqual(set(c['browser_sites']), {'source', 'env', 'count'})
                self.assertRegex(c['selector'], r'url=https://fixture\.test/site/[12]/$')

    def test_quake3_disk_pins_match_the_host_test(self):
        quake = next(c for c in self.raw['cases'] if c['id'] == 'acc-quake3')
        exe = next(f for f in quake['disk']['files'] if f['path'].endswith('quake3.exe'))
        q3 = (ROOT / 'tests/host/win32/installers/test_quake3.py')
        if q3.is_file():
            source = q3.read_text()
            self.assertIn(exe['sha256'], source)
            self.assertIn(str(next(f for f in quake['disk']['files'] if f['path'].endswith('pak0.pk3'))['size']), source)
        # sec-user-02 (A1-5): the installer lives in the protected installer
        # store; the desktop folder holds only shortcuts to it (sec-user-06).
        bundled = (ROOT / 'scripts/q3a_demo.py').read_text()
        self.assertIn('STORE_WINDOWS = "C:\\\\Programs\\\\Installers\\\\Quake III Arena Demo"', bundled)
        self.assertIn('"Install Quake III Arena Demo.lnk": ("Install Quake III Arena Demo", "Q3ADemo.exe")', bundled)
        typed = next(a['text'] for s in quake['steps'] if s.get('name') == 'run-installer' for a in s['actions'] if 'text' in a)
        self.assertEqual(typed, '"C:\\Programs\\Installers\\Quake III Arena Demo\\Q3ADemo.exe"')

    def test_games_follow_the_catalogue(self):
        catalog = (ROOT / 'apps/desktop/catalog.h').read_text()
        titles = re.findall(r'\{"[^"]+", "([^"]+)", "\\\\[^"]+", "input-gaming"\}', catalog)
        self.assertEqual(sorted(titles), sorted(['Wolfenstein 3D', 'DOOM', 'Quake', 'Warcraft II', 'Duke Nukem 3D']))
        games = next(c for c in self.raw['cases'] if c['id'] == 'acc-dos-games')
        windows = [re.search(r'window=(.*)\$$', s['wait'])[1].replace('\\ ', ' ') for s in games['steps'] if s.get('name', '').endswith('-vm')]
        self.assertEqual(sorted(windows), sorted(titles))
        # Letters in the sorted Start Menu Games folder (customize_io.h sh_folder_scan sorts by label).
        labels = sorted(re.findall(r'\{"([^"]+)", "[^"]+", "\\\\[^"]+", "input-gaming"\}', catalog) + ['Classic Games'], key=str.casefold)
        for step in games['steps']:
            if not step.get('name', '').endswith('-launch'):
                continue
            keys = step['actions'][0]['keys']
            letters = keys[keys.index('g') + 2:-1]
            hot = 0
            for letter in letters:
                found = [i for i, label in enumerate(labels) if label[0].lower() == letter]
                hot = next((i for i in found if i > hot), found[0])
            title = step['name'].split('-')[0]
            expected = {'doom': 'DOOM', 'wolf3d': 'Wolfenstein', 'war2': 'Warcraft', 'duke3d': 'Duke', 'quake': 'Quake'}[title]
            self.assertTrue(labels[hot].startswith(expected), (title, labels[hot]))


class RecordFormatTests(unittest.TestCase):
    """The suite matches the guest's own record formats (C sources)."""
    @staticmethod
    def render(source, needle, **values):
        """The first printf format containing needle, rendered with sample values."""
        text = (ROOT / source).read_text()
        match = re.search(r'"((?:[^"\\]|\\.)*%s(?:[^"\\]|\\.)*)"' % re.escape(needle), text)
        assert match, (source, needle)
        fmt = match[1].replace('\\"', '"').replace('\\n', '')
        names = iter(values.get('order', []))
        def value(m):
            spec = m[0]
            if spec == '%%':
                return '%'
            name = next(names, None)
            if name is not None and name in values:
                return str(values[name])
            return {'s': 'x', 'c': 'D', 'x': '1a', 'd': '0'}.get(spec[-1], '1')
        return re.sub(r'%(?:%|[-0-9.]*(?:ll|z|l)?[udxsc])', value, fmt)

    def assertMatches(self, pattern, line):
        self.assertRegex(line, re.compile(acceptance.PLACEHOLDER.sub(lambda m: '(?:x|1|D|' + r'\d+' + ')', pattern)))

    def test_desktop_records(self):
        display = '[user 7] ' + self.render('apps/desktop/desktop.c', 'display start mode=',
                                             order=['mode', 'w', 'h', 'bpp', 'saved', 'safe'], w=1024, h=768, bpp=32, safe=0)
        self.assertEqual(display, '[user 7] [desktop] display start mode=1 1024x768x32 saved=1 safe=0')
        self.assertMatches(acceptance.boot_template('b1', 'boot')[3]['wait'], display)
        focus = '[user 7] ' + self.render('apps/desktop/input.c', 'focus pid=', order=['pid', 'w', 'titled', 'internal', 'app'],
                                           pid=12, titled=0, internal=0, app='files')
        self.assertEqual(focus, '[user 7] [desktop] focus pid=12 window=1 titled=0 internal=0 app=files')
        suite = json.loads((ROOT / 'tests/suites/acc-t23.json').read_text())
        wizard = next(s['wizard'] for c in suite['cases'] for s in c.get('steps', []) if 'wizard' in s)
        self.assertRegex(focus.replace('titled=0', 'titled=1'), wizard['close_when'])
        self.assertEqual(acceptance.FOCUS_RECORD.match(focus)['pid'], '12')
        # fx-acc1: the program-window record the page wizard follows.
        window = '[user 7] ' + self.render('apps/desktop/input.c', 'window pid=', order=['pid', 'w', 'owner', 'modal', 'state', 'title'],
                                            pid=12, w=4, owner=3, modal=1, state='shown', title='Welcome')
        self.assertEqual(window, '[user 7] [desktop] window pid=12 window=4 owner=3 modal=1 state=shown title=Welcome')
        record = acceptance.WINDOW_RECORD.match(window)
        self.assertEqual((record['pid'], record['window'], record['owner'], record['modal'], record['state'], record['title']),
                         ('12', '4', '3', '1', 'shown', 'Welcome'))
        states = re.findall(r'"(hidden|minimized|shown|closed)"', (ROOT / 'apps/desktop/input.c').read_text() +
                            (ROOT / 'apps/desktop/client.c').read_text())
        self.assertEqual(set(states), {'hidden', 'minimized', 'shown', 'closed'})
        self.assertFalse(acceptance.WINDOW_RECORD.match(window.replace('title=Welcome', 'title=Two words')))
        welcome = acceptance.Session._page_spec(wizard, {'title': 'Welcome', 'modal': True})
        self.assertEqual(welcome['name'], 'welcome')
        self.assertEqual(acceptance.Session._page_spec(wizard, {'title': 'IDS_TITLE', 'modal': False})['name'], 'copy')
        hung = '[user 7] ' + self.render('apps/desktop/client.c', 'not-responding pid=', order=['pid', 'w', 'state'], pid=12, state=1)
        self.assertRegex(hung, r'^\[user \d+\] \[desktop\] not-responding pid=12 window=\d+ state=1 ')
        notice = '[user 7] ' + self.render('apps/desktop/shell.c', '[desktop] %s icon=%s title=', order=['kind', 'icon'],
                                            kind='message', icon='error')
        self.assertRegex(notice, r'^\[user \d+\] \[desktop\] (?:notify|message) icon=(?:error|warning) ')
        probe = '[user 7] ' + self.render('apps/desktop/autoplay.c', 'autoplay probe drive=', order=['drive', 'entries', 'kind', 'music', 'pictures', 'result'],
                                           drive='D', entries=3, result=0)
        self.assertRegex(probe, r'^\[user \d+\] \[desktop\] autoplay probe drive=D entries=3 run_kind=\d+ music=\d+ pictures=\d+ result=0$')
        prompt = '[user 7] ' + self.render('apps/desktop/autoplay.c', 'autoplay prompt drive=', order=['drive', 'default'], drive='D')
        self.assertRegex(prompt, r'^\[user \d+\] \[desktop\] autoplay prompt drive=D default=\d+$')
        launch = '[user 7] ' + self.render('apps/desktop/autoplay.c', 'autoplay launch action=', order=['action', 'pid'], action=0, pid=0)
        self.assertRegex(launch, r'^\[user \d+\] \[desktop\] autoplay launch action=0 pid=-?\d+$')
        files = '[user 7] ' + self.render('apps/desktop/desktop.c', 'launch name=%.40s pid=', order=['name', 'pid', 'error'],
                                           name='files', pid=31, error=0)
        self.assertRegex(files, r'^\[user \d+\] \[desktop\] launch name=files pid=(?P<cd_files>[1-9]\d*) error=0 ')
        dialog = '[console] ' + self.render('apps/desktop/client.c', 'end task dialog kind=', order=['kind', 'pid'], kind='dos', pid=40)
        self.assertRegex(dialog, r'^\[console\] \[desktop\] end task dialog kind=dos pid=40 ')

    def test_desktop_console_records_are_single_lines(self):
        """fx-acc1 (acceptance run f63df5ab, serial lines 1402-1408): every
        write() of a program is its own [console] line, and newlib's
        unbuffered stderr wrote each fprintf conversion separately, so
        "end task dialog kind=dos pid=6 session=0" arrived as seven lines and
        the DOS games' close step never matched it. fx-autorun: the SDK C
        runtime makes stderr line buffered for every native program before
        main (the desktop's own setvbuf is gone); every record the desktop
        writes ends with a newline, so none is held back."""
        crt = (ROOT / 'sdk/crt/start.c').read_text()
        start = crt[crt.index('void __ciuki_start(uint32_t *stack) {'):]
        self.assertLess(start.index('__sinit(&main_reent);'), start.index('stdio_line_buffering();'))
        self.assertLess(start.index('stdio_line_buffering();'), start.index('exit(main(argc,argv,environ));'))
        self.assertIn('stderr->_flags=(short)((stderr->_flags&~__SNBF)|__SLBF);', crt)
        self.assertNotIn('setvbuf(stderr', (ROOT / 'apps/desktop/desktop.c').read_text())
        unterminated, other = [], []
        for path in sorted((ROOT / 'apps/desktop').glob('*.c')):
            text = path.read_text()
            for match in re.finditer(r'fprintf\(\s*stderr\s*,\s*((?:"(?:[^"\\]|\\.)*"\s*)+)', text):
                literal = ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', match[1]))
                if not literal.endswith('\\n'):
                    unterminated.append((path.name, literal[:60]))
            other += [(path.name, m[0]) for m in re.finditer(r'\b(?:fputs|fwrite|vfprintf|perror|fputc|putc)\s*\([^;]*\bstderr\b', text)]
        self.assertEqual(unterminated, [])
        # settings_io.c writes whole records it formatted with a final newline.
        self.assertEqual(sorted({name for name, _ in other}), ['settings_io.c'])
        settings = (ROOT / 'apps/desktop/settings_io.c').read_text()
        self.assertIn('"[desktop] stage=%s us=%llu%s\\n"', settings)

    def test_dosbox_full_screen_records(self):
        """fx-dosfull: the DOS Box's automatic full screen, its refusal and the
        remembered usage are one console line each (stderr line buffered as
        the desktop's, fx-acc1), and the acc-dos-games patterns match them."""
        source = (ROOT / 'apps/dosbox/dosbox.c').read_text()
        body = source[source.index('int main(int argc, char **argv)\n{'):]
        head = body[:body.index('int channel = 3')]
        self.assertIn('#ifdef __i386__', head)
        self.assertIn('setvbuf(stderr, NULL, _IOLBF, BUFSIZ);', head)
        for match in re.finditer(r'fprintf\(\s*stderr\s*,\s*((?:"(?:[^"\\]|\\.)*"\s*)+)', source):
            self.assertTrue(''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', match[1])).endswith('\\n'), match[0])
        refused = '[console] ' + self.render('apps/dosbox/dosbox.c', 'result=refused error=%d continuation=windowed',
                                             order=['vm', 'gen', 'usage', 'graphics', 'error'], usage='graphics', graphics=1, error=0)
        self.assertEqual(refused, '[console] dosbox: fullscreen auto vm=1 generation=1 usage=graphics graphics=1 result=refused error=0 continuation=windowed')
        usage = '[console] ' + self.render('apps/dosbox/dosbox.c', 'dosbox: screen usage vm=',
                                           order=['vm', 'gen', 'usage', 'origin', 'saved'], usage='window', origin='alt-enter', saved=1)
        self.assertEqual(usage, '[console] dosbox: screen usage vm=1 generation=1 usage=window source=alt-enter saved=1')
        entered = '[console] ' + self.render('apps/dosbox/dosbox.c', 'result=entered',
                                             order=['vm', 'gen', 'usage', 'graphics'], usage='graphics', graphics=1)
        games = next(c for c in json.loads((ROOT / 'tests/suites/acc-t23.json').read_text())['cases'] if c['id'] == 'acc-dos-games')
        steps = {s['name']: s for s in games['steps'] if 'name' in s}
        for g in ('doom', 'wolf3d', 'war2', 'duke3d', 'quake'):
            with self.subTest(game=g):
                self.assertMatches(steps[g + '-fs-auto-fallback']['wait'], refused)
                self.assertMatches(steps[g + '-fs-auto-return']['wait'][1], usage)
                self.assertMatches(steps[g + '-fs-auto-fallback']['fail'], usage)
                self.assertNotRegex(entered, steps[g + '-fs-auto-fallback']['wait'].replace('<<%s_vm>>' % g, '1').replace('<<%s_gen>>' % g, '1'))
                assertion = next(a for a in games['assertions'] if a['name'] == g + '-fs-auto-refused-once')
                self.assertMatches(assertion['pattern'], refused)
                # The automatic request precedes the window record (QEMU) or replaces it until the return (hardware).
                self.assertEqual(steps[g + '-graphics']['since'], 'boot')
                self.assertEqual(steps[g + '-fs-auto-fallback']['unless'], [g + '-fs-auto-entered'])

    def test_kernel_records(self):
        cases = (
            ('src/kernel/vm/dosvm_vga.c', '[dosvm-window] video vm=', {'order': ['vm', 'gen', 'vbe', 'w', 'h', 'text'], 'vm': 1, 'gen': 2,
             'vbe': 0, 'w': 320, 'h': 200, 'text': 0},
             r'^\[dosvm-window\] video vm=(?P<g_vm>\d+) gen=(?P<g_gen>\d+) vbe=[0-9a-f]+ width=(?P<w>[1-9]\d{2,3}) height=(?P<h>[1-9]\d{2,3}) text=0$'),
            ('src/kernel/vm/dosvm_perf.inc', '[dosvm] perf vm=', {'order': ['vm', 'gen', 'exits'], 'vm': 1, 'gen': 2, 'exits': 900},
             r'^\[dosvm\] perf vm=1 gen=2 exits=[1-9]\d* '),
            ('src/kernel/vm/dosvm_fullscreen.c', '[dosvm] fullscreen_request vm=', {'order': ['vm', 'gen', 'mode'], 'vm': 1, 'gen': 2, 'mode': 19},
             r'^\[dosvm\] fullscreen_request vm=1 generation=2 mode=(?P<m>\d+)$'),
            ('src/kernel/vm/dosvm_fullscreen.c', '[dosvm] fullscreen=1 vm=', {'order': ['vm', 'gen', 'mode'], 'vm': 1, 'gen': 2},
             r'^\[dosvm\] fullscreen=1 vm=1 generation=2 mode=\d+$'),
            ('src/kernel/vm/dosvm_fullscreen.c', '[dosvm] fullscreen=0 vm=', {'order': ['vm', 'gen', 'result'], 'vm': 1, 'gen': 2, 'result': 0},
             r'^\[dosvm\] fullscreen=0 vm=1 generation=2 result=0 '),
            ('src/kernel/vm/dosvm_fullscreen.c', 'fullscreen_refused vm=%u generation=%u error=%d continuation=windowed',
             {'order': ['vm'], 'vm': 1}, r'^\[dosvm\] fullscreen_(?:refused|transport_fault) vm=1 '),
            ('src/kernel/vm/dosvm.c', '[dosvm] lifecycle stage=cleanup vm=', {'order': ['vm', 'gen', 'result'], 'vm': 1, 'gen': 2, 'result': 0},
             r'^\[dosvm\] lifecycle stage=cleanup vm=1 generation=2 result=0 '),
            ('src/kernel/vm/dosvm_client.c', '[launch-stage] name=dosbox pid=', {'order': ['pid', 'window'], 'pid': 40, 'window': 'Duke Nukem 3D'},
             r'^\[launch-stage\] name=dosbox pid=(?P<p>\d+) stage=vm-create window=Duke\ Nukem\ 3D$'),
            ('src/kernel/core/power.c', '[power] %s requested by pid %u attempt=', {'order': ['what'], 'what': 'reboot'},
             r'^\[power\] reboot requested by pid \d+ attempt=\d+ progress=\d+ storage=\w+$'),
            ('src/kernel/core/power.c', '[power] %s requested by pid %u attempt=', {'order': ['what'], 'what': 'power off'},
             r'^\[power\] power off requested by pid \d+ attempt=\d+ progress=\d+ storage=\w+$'),
            ('src/kernel/core/power.c', 'stage=reset fallback=8042-triple-fault', {}, r'^\[power\] stage=reset fallback=8042-triple-fault$'),
            ('src/kernel/core/power.c', '[power] stage=%s-off state=quiescent', {'order': ['p'], 'p': 'acpi'},
             r'^\[power\] stage=(?:(?:acpi|apm)-off state=quiescent$|acpi-s5-off )'),
            ('src/kernel/fs/mount.c', '[storage] completion drive=%c kind=durable result=%d incomplete=%u generation=%u group=',
             {'order': ['d', 'r', 'i'], 'd': 'C', 'r': 0, 'i': 0}, r'^\[storage\] completion drive=C kind=durable result=0 incomplete=0 '),
            ('src/kernel/drivers/optical.c', '[storage] optical slot=', {'order': ['slot', 'drive', 'gen', 'result', 'reason'],
             'drive': 'D', 'result': 3, 'reason': 'mounted'},
             r'^\[storage\] optical slot=\d+ drive=(?P<cd>[A-Z]) mode=ro generation=\d+ result=\d+ reason=mounted$'),
            # fx-emptydrive: an empty drive's record names the letter it keeps.
            ('src/kernel/drivers/optical.c', '[storage] optical slot=', {'order': ['slot', 'drive', 'gen', 'result', 'reason'],
             'drive': 'D', 'result': -123, 'reason': 'no_medium'},
             r'^\[storage\] optical slot=\d+ drive=D mode=ro generation=\d+ result=-?\d+ reason=no_medium$'),
            ('src/kernel/proc/win32/diagnostics.c', '[win32] start pid=%u exe=%s pe=%08x:%08x shims=%s',
             {'order': ['pid', 'exe'], 'pid': 12, 'exe': 'C:\\Documents\\Desktop\\Quake_III_Arena_Demo\\Q3ADemo.exe'},
             r'^\[win32\] start pid=(?P<setup>\d+) exe=\S*[Qq]3[Aa][Dd][Ee][Mm][Oo]\.[Ee][Xx][Ee] '),
            ('src/kernel/proc/win32/diagnostics.c', '[win32] exit pid=', {'order': ['pid', 'code', 'reason'], 'pid': 12, 'code': '00000000',
             'reason': 'normal'}, r'^\[win32\] exit pid=12 code=\S+ reason=normal '),
            ('src/kernel/proc/supervisor.c', '[supervisor] desktop pid=', {'order': ['pid', 'pgid']},
             r'^\[supervisor\] desktop pid=(?P<d>\d+) pgid=\d+'),
            ('src/kernel/core/init.c', '[init] flag safe_mode=', {'order': ['safe'], 'safe': 1}, r'^\[init\] flag safe_mode=1 '),
            ('src/kernel/fs/mount.c', '[scandisk] start drive=', {'order': ['d'], 'd': 'C'}, r'^\[scandisk\] start drive=C '),
        )
        for source, needle, values, pattern in cases:
            with self.subTest(needle=needle):
                path = source
                if not (ROOT / path).read_text().count(needle):
                    path = next(str(p.relative_to(ROOT)) for p in (ROOT / 'src/kernel').rglob('*.c') if needle in p.read_text())
                line = self.render(path, needle, **values)
                self.assertRegex(line, pattern)

    def test_loader_menu_strings(self):
        menu = (ROOT / 'src/boot/ciukldr/menu.inc').read_text()
        startup = (ROOT / 'src/boot/ciukldr/startup.inc').read_text()
        self.assertIn("menu_text db 'SELECT_READY',13,10", menu)
        self.assertIn("cmp al, 'n'", menu)                   # N: Normal at once
        self.assertIn("startup_selected db 'L:STARTUP=',0", startup)
        self.assertRegex(startup, r"mov al, '3'\n\s+cmp ah, 0x3f")      # F5 (3F00h): item 3, Safe mode
        self.assertIn('CBI_F_SAFE_MODE | CBI_F_SAFE_STORAGE, CBI_F_STEP', startup)

    def test_first_frame_record_of_the_display_lease(self):
        source = (ROOT / 'src/kernel/core/display_present.c').read_text()
        if 'present_lease=first-frame' not in source:
            self.skipTest('the fx-q3vise first-frame record (main 93d4ce76d) is not in this tree yet')
        line = self.render('src/kernel/core/display_present.c', 'present_lease=first-frame pid=', order=['pid', 'token', 'ms', 'path'],
                           pid=44, token=2, ms=1234, path='copy')
        self.assertRegex(line, r'^\[display\] present_lease=first-frame pid=44 token=\d+ ms=(?P<ms>\d+) path=\S+$')


@unittest.skipUnless(HAVE_XORRISO, 'xorriso required')
class IsoFixtureTests(unittest.TestCase):
    def test_volume_sequence_label_and_root_entries(self):
        SCRATCH.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=SCRATCH) as temp:
            temp = Path(temp)
            plain = iso_fixtures.generate({'level': 3, 'label': 'CIUKI_STD'}, temp, media_name='std')
            zero = iso_fixtures.generate({'level': 3, 'label': 'CIUKI_VS0', 'volume_sequence': 0}, temp, media_name='vs0')
            joliet = iso_fixtures.generate({'level': 3, 'joliet': True, 'volume_sequence': 0}, temp, media_name='joliet')
            self.assertEqual(Path(plain['path']).name, 'media-std.iso')
            self.assertTrue((temp / 'media-vs0-manifest.json').is_file())
            for manifest, number in ((plain, 1), (zero, 0), (joliet, 0)):
                data = Path(manifest['path']).read_bytes()
                offsets = iso_fixtures.descriptor_offsets(manifest['path'])
                self.assertEqual(len(offsets), 2 if manifest['joliet'] else 1)
                for offset in offsets:
                    self.assertEqual(data[offset + 124:offset + 128], number.to_bytes(2, 'little') + number.to_bytes(2, 'big'))
                self.assertEqual(manifest['sha256'], iso_fixtures.sha(manifest['path']))
            self.assertEqual(data[16 * 2048 + 40:16 * 2048 + 48], b'CIUKI_F4')    # the default label
            self.assertEqual(Path(plain['path']).read_bytes()[16 * 2048 + 40:16 * 2048 + 49], b'CIUKI_STD')
            self.assertEqual(plain['root_entries'], ['/DATA', '/LONG_DIRECTORY', '/README.TXT'])
            self.assertEqual(zero['volume_sequence_sectors'], [16])
            self.assertEqual(len(joliet['root_entries']), 4)
            for bad in ({'label': 'lower'}, {'label': 'X' * 33}, {'volume_sequence': 70000}):
                with self.subTest(spec=bad), self.assertRaises(ValueError):
                    iso_fixtures.generate({'level': 3, **bad}, temp, media_name='bad' + str(len(str(bad))))
            out = temp / 'cli'
            done = subprocess.run([sys.executable, str(ROOT / 'scripts/test/iso_fixtures.py'), '--out', str(out), '--volume-sequence', '0'],
                                  capture_output=True, text=True, check=True)
            self.assertEqual(json.loads(done.stdout)['volume_sequence'], 0)


class InstallShield5MediumTests(unittest.TestCase):
    """fx-acc1 item 9: the generated InstallShield 5 test medium of
    acc-installshield5 (iso_fixtures.is5_files): authored bytes only, admitted
    and decoded by the production bootstrap code (sdk/win32/is5boot)."""
    CHECK = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "private.h"
static unsigned char *load(const char *path, unsigned *n) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long size = ftell(f); rewind(f);
    unsigned char *p = malloc(size ? (size_t)size : 1);
    if (p && fread(p, 1, (size_t)size, f) != (size_t)size) { free(p); p = 0; }
    fclose(f); *n = (unsigned)size; return p;
}
int main(int argc, char **argv) {
    unsigned n = 0, size = 0;
    unsigned char *stub = load(argv[1], &n);
    if (!stub || !is5_ne(stub, n)) { puts("FAIL stub"); return 1; }
    unsigned char *raw = load(argv[2], &size); struct is5_archive a;
    if (!raw || !is5_directory(raw, size, &a)) { puts("FAIL directory"); return 1; }
    if ((int)a.count * 3 != argc - 3) { puts("FAIL count"); return 1; }
    for (unsigned i = 0; i < a.count; i++) {
        struct is5_member *m = a.member + i; unsigned want = 0;
        unsigned char *expected = load(argv[5 + 3 * i], &want), *out = malloc(want ? want : 1);
        if (!expected || strcmp(m->original, argv[3 + 3 * i]) || strcmp(m->runtime, argv[4 + 3 * i]) || m->expanded != want ||
            !is5_dcl(raw + m->offset, m->packed, out, want) || memcmp(out, expected, want)) { printf("FAIL member %u\n", i); return 1; }
        free(out); free(expected);
    }
    printf("PASS %u members\n", a.count);
    free(raw); free(stub); return 0;
}
'''

    def test_admitted_and_decoded_by_the_production_bootstrap(self):
        SCRATCH.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=SCRATCH) as temp:
            temp = Path(temp)
            files = iso_fixtures.is5_files()
            for name, data in files.items():
                (temp / name).write_bytes(data)
            members = [('INSTALL.EXE', '_INS0000._MP', iso_fixtures.is5_engine_pe()),
                       ('zdata.dll', 'ZDataI51.dll', b'CiukiOS IS5 test support file 1\r\n'),
                       ('WUTL95i.DLL', '_WUTL951.DLL', b'CiukiOS IS5 test support file 2\r\n')]
            arguments = []
            for k, (name, runtime, data) in enumerate(members):
                (temp / ('member%d' % k)).write_bytes(data)
                arguments += [name, runtime, str(temp / ('member%d' % k))]
            (temp / 'check.c').write_text(self.CHECK)
            binary = temp / 'check'
            subprocess.run(['clang', '-std=c17', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-I', str(ROOT / 'sdk/win32/is5boot'), str(ROOT / 'sdk/win32/is5boot/archive.c'),
                            str(temp / 'check.c'), '-o', str(binary)], check=True)
            done = subprocess.run([str(binary), str(temp / 'SETUP.EXE'), str(temp / '_INST32I.EX_'), *arguments],
                                  capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            self.assertIn('PASS 3 members', done.stdout)
            # A corrupted stream or a library NE header is refused by the same code.
            broken = bytearray(files['_INST32I.EX_']); broken[-1] ^= 0xff
            (temp / 'broken').write_bytes(broken)
            self.assertNotEqual(subprocess.run([str(binary), str(temp / 'SETUP.EXE'), str(temp / 'broken'), *arguments],
                                               capture_output=True).returncode, 0)
            library = bytearray(files['SETUP.EXE']); library[0x80 + 13] |= 0x80
            (temp / 'library').write_bytes(library)
            self.assertNotEqual(subprocess.run([str(binary), str(temp / 'library'), str(temp / '_INST32I.EX_'), *arguments],
                                               capture_output=True).returncode, 0)
        # The kernel admission (is5boot.h is5_redirect) reads the same header
        # fields and needs SETUP.INS and _INST32I.EX_ beside the stub.
        admission = (ROOT / 'src/kernel/proc/win32/is5boot.h').read_text()
        for needle in ('"SETUP.INS","_INST32I.EX_"', 'ne[54]!=2', '(is5_word(ne+12)&0x8000)'):
            self.assertIn(needle, admission)
        self.assertEqual(sorted(files), ['SETUP.EXE', 'SETUP.INI', 'SETUP.INS', 'SETUP.LID', '_INST32I.EX_'])
        self.assertIn(b'[Startup]', files['SETUP.INI'])
        self.assertIn(b'[Languages]', files['SETUP.LID'])
        self.assertEqual(iso_fixtures.is5_files(), files)   # deterministic

    def test_engine_pe_layout(self):
        pe = iso_fixtures.is5_engine_pe()
        self.assertEqual(len(pe), 1536)
        self.assertEqual(pe[:2], b'MZ')
        self.assertEqual(pe[0x40:0x44], b'PE\0\0')
        machine, sections = struct_unpack('<HH', pe, 0x44)
        self.assertEqual((machine, sections), (0x14c, 1))
        magic = struct_unpack('<H', pe, 0x58)[0]
        entry, = struct_unpack('<I', pe, 0x58 + 16)
        subsystem, = struct_unpack('<H', pe, 0x58 + 68)
        self.assertEqual((magic, subsystem), (0x10b, 2))
        self.assertTrue(0x1000 <= entry < 0x1400)
        for name in (b'kernel32.dll', b'CreateFileA', b'GetCurrentDirectoryA', b'WriteFile', b'CloseHandle', b'ExitProcess',
                     iso_fixtures.IS5_ENGINE_LOG.encode()):
            self.assertIn(name, pe)
        self.assertIn(b'\x6a' + bytes([iso_fixtures.IS5_ENGINE_EXIT]) + b'\xff\x15', pe)   # push 42; call [ExitProcess]

    @unittest.skipUnless(os.environ.get('CIUKI_IS5_WINE') and shutil.which('wine'), 'set CIUKI_IS5_WINE=1 to run the engine under Wine')
    def test_engine_runs_under_wine(self):
        SCRATCH.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=SCRATCH) as temp:
            temp = Path(temp)
            (temp / 'engine.exe').write_bytes(iso_fixtures.is5_engine_pe('C:\\windows\\temp\\is5engine.txt'))
            env = dict(os.environ, WINEPREFIX=str(temp / 'prefix'), WINEDEBUG='-all')
            done = subprocess.run(['wine', 'engine.exe'], cwd=temp, env=env, capture_output=True, text=True, timeout=600)
            self.assertEqual(done.returncode, iso_fixtures.IS5_ENGINE_EXIT, done.stderr[-2000:])
            written = (temp / 'prefix/drive_c/windows/temp/is5engine.txt').read_text()
            self.assertTrue(written.endswith(temp.name), written)

    @unittest.skipUnless(HAVE_XORRISO, 'xorriso absent')
    def test_medium_iso(self):
        SCRATCH.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=SCRATCH) as temp:
            manifest = iso_fixtures.generate({'level': 3, 'label': 'CIUKI_IS5', 'content': 'is5'}, temp, media_name='is5')
            self.assertEqual(manifest['root_entries'], ['/README.TXT', '/SETUP.EXE', '/SETUP.INI', '/SETUP.INS', '/SETUP.LID', '/_INST32I.EX_'])
            files = iso_fixtures.is5_files()
            for entry in manifest['entries']:
                name = entry['path'][1:]
                if name in files:
                    self.assertEqual(entry['sha256'], hashlib.sha256(files[name]).hexdigest(), name)
            self.assertEqual(manifest['content'], 'is5')
            with self.assertRaises(ValueError):
                iso_fixtures.generate({'level': 3, 'content': 'other'}, temp, media_name='other')

    def test_case_records(self):
        suite = json.loads((ROOT / 'tests/suites/acc-t23.json').read_text())
        c = next(c for c in suite['cases'] if c['id'] == 'acc-installshield5')
        self.assertFalse(c.get('pending'))
        self.assertEqual(c['media']['is5']['content'], 'is5')
        acceptance.validate_case(c)
        steps = {s['name']: s for s in acceptance.expand_steps(c['steps'])}
        start = RecordFormatTests.render('src/kernel/proc/win32/diagnostics.c', '[win32] start pid=%u exe=%s pe=%08x:%08x shims=%s',
                                         order=['pid', 'exe'], pid=21, exe='C:\\System\\Win32\\is5boot.exe')
        self.assertRegex(start, steps['is5-helper']['wait'])
        self.assertRegex(start.replace('C:\\System\\Win32\\is5boot.exe', '/System/Win32/is5boot.exe'), steps['is5-helper']['wait'])
        self.assertNotRegex(start.replace('is5boot.exe', 'SETUP.EXE'), steps['is5-helper']['wait'])
        engine = start.replace('pid=21', 'pid=22').replace('C:\\System\\Win32\\is5boot.exe', 'C:\\WINDOWS\\TEMP\\_INS1234._MP')
        self.assertRegex(engine, steps['is5-engine']['wait'])
        exit_line = RecordFormatTests.render('src/kernel/proc/win32/diagnostics.c', '[win32] exit pid=', order=['pid', 'code', 'reason'],
                                             pid=22, code='0000002a', reason='normal')
        variables = {'is5_engine': '22', 'is5_helper': '21'}
        fill = lambda p: acceptance.PLACEHOLDER.sub(lambda m: variables[m[1]], p)
        self.assertRegex(exit_line, fill(steps['is5-engine-exit']['wait']))
        self.assertTrue(any(re.match(fill(p), exit_line.replace('0000002a', '00000000')) for p in steps['is5-engine-exit']['fail']))
        self.assertFalse(any(re.match(fill(p), exit_line) for p in steps['is5-engine-exit']['fail']))
        self.assertRegex(exit_line.replace('pid=22', 'pid=21'), fill(steps['is5-helper-exit']['wait']))
        box = '[user 7] ' + RecordFormatTests.render('apps/desktop/input.c', 'window pid=', order=['pid', 'w', 'owner', 'modal', 'state', 'title'],
                                                     pid=21, w=9, owner=0, modal=0, state='shown', title='CiukiOS_IS5_Test')
        self.assertTrue(any(re.match(fill(p), box) for p in steps['is5-helper-exit']['fail']))
        self.assertEqual(c['disk']['files'][0]['sha256'], hashlib.sha256(b'D:\\').hexdigest())
        self.assertEqual(c['disk']['files'][0]['path'], iso_fixtures.IS5_ENGINE_LOG)
        self.assertIs(c['disk']['menu_links'], False)
        self.assertEqual(steps['run-setup']['actions'][0]['text'], 'D:\\SETUP.EXE')
        # A disk check without Start-menu links passes on its files alone.
        delta = {'added': [{'path': 'C:/Documents/is5engine.txt', 'kind': 'file', 'size': 3, 'sha256': c['disk']['files'][0]['sha256']}]}
        self.assertEqual(acceptance.disk_check(c['disk'], delta)['status'], 'pass')
        self.assertEqual(acceptance.disk_check({k: v for k, v in c['disk'].items() if k != 'menu_links'}, delta)['status'], 'fail')


class SiteFixtureTests(unittest.TestCase):
    INDEX = (b'<html><head><link href="https://www.example-owner.test/build/assets/app-9f3a.css" rel="stylesheet">'
             b'<script src="/build/assets/app-77aa.js"></script></head><body>'
             b'<a href="https://www.example-owner.test/it/progetti">p</a><img src="/images/logo.svg">'
             b'<a href="https://elsewhere.test/x">e</a><a href="//www.example-owner.test">h</a></body></html>')

    def setUp(self):
        SCRATCH.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=SCRATCH); self.dir = Path(self.temp.name)
        site = self.dir / 'snap' / 'www.example-owner.test'
        (site / 'images').mkdir(parents=True)
        (site / 'index.html').write_bytes(self.INDEX)
        (site / 'app.css').write_bytes(b'body{background:url(/images/logo.svg)}')
        (site / 'app.js').write_bytes(b'var a=1;')
        (site / 'images/logo.svg').write_bytes(b'<svg/>')
        (site / 'headers.txt').write_bytes(b'private capture metadata')
        other = self.dir / 'snap' / 'blog.example-owner.test'
        other.mkdir()
        (other / 'index.html').write_bytes(b'<p>second</p>')
        (self.dir / 'snap' / 'not-a-site').mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def test_load_sites_by_number(self):
        sites = https_fixture.load_sites({'source': 'snap', 'count': 2}, self.dir)
        self.assertEqual(sorted(sites), ['1', '2'])
        self.assertEqual(sites['1']['host'], 'blog.example-owner.test')
        self.assertEqual(sites['2']['files'], ['app.css', 'app.js', 'images/logo.svg', 'index.html'])
        with patch.dict(os.environ, {'CIUKI_NS_SITES': str(self.dir / 'snap')}):
            self.assertEqual(sorted(https_fixture.load_sites({'source': 'nowhere', 'env': 'CIUKI_NS_SITES', 'count': 1}, ROOT)), ['1'])
        with self.assertRaises(EvidenceNotRun):
            https_fixture.load_sites({'source': 'snap', 'count': 3}, self.dir)
        with self.assertRaises(https_fixture.SitesAbsent):
            https_fixture.load_sites({'source': 'missing', 'count': 1}, self.dir)
        for bad in ({'source': 'snap'}, {'source': '/abs', 'count': 1}, {'source': '../x', 'count': 1},
                    {'source': 'snap', 'count': 1, 'sites': {}}, {'source': 'snap', 'count': 1, 'env': 'lower'}):
            with self.subTest(declaration=bad), self.assertRaises(ValueError):
                https_fixture.load_sites(bad, self.dir)

    def test_served_rewritten_aliased_and_recorded(self):
        sites = https_fixture.load_sites({'source': 'snap', 'count': 2}, self.dir)
        with https_fixture.FixtureServer(ROOT / 'tests/suites/fixtures/f6-browser', sites=sites) as server:
            def get(path):
                connection = http.client.HTTPConnection('127.0.0.1', server.http_port, timeout=5)
                connection.request('GET', path, headers={'Host': 'fixture.test'})
                response = connection.getresponse(); body = response.read(); connection.close()
                return response.status, response.getheader('Content-Type'), body
            status, kind, body = get('/site/2/')
            self.assertEqual((status, kind), (200, 'text/html; charset=utf-8'))
            self.assertIn(b'href="https://fixture.test/site/2/build/assets/app-9f3a.css"', body)
            self.assertIn(b'src="https://fixture.test/site/2/build/assets/app-77aa.js"', body)
            self.assertIn(b'href="https://fixture.test/site/2/it/progetti"', body)
            self.assertIn(b'src="https://fixture.test/site/2/images/logo.svg"', body)
            self.assertIn(b'href="https://fixture.test/site/2"', body)
            self.assertIn(b'href="https://elsewhere.test/x"', body)
            self.assertNotIn(b'example-owner', body)
            self.assertEqual(get('/site/2/build/assets/app-9f3a.css')[2], b'body{background:url(https://fixture.test/site/2/images/logo.svg)}')
            self.assertEqual(get('/site/2/build/assets/app-77aa.js')[:2], (200, 'application/javascript; charset=utf-8'))
            self.assertEqual(get('/site/2/images/logo.svg')[2], b'<svg/>')
            self.assertEqual(get('/site/2/headers.txt')[0], 404)
            self.assertEqual(get('/site/2/../../etc/passwd')[0], 404)
            self.assertEqual(get('/site/2/it/progetti')[0], 404)
            self.assertEqual(get('/site/9/')[0], 404)
            self.assertEqual(get('/site/2')[0], 301)
            self.assertEqual(get('/site/1/')[2], b'<p>second</p>')
            self.assertEqual(get('/page')[0], 200)
            snapshot = server.snapshot()
        self.assertEqual(snapshot['sites'], {'1': {'files': 1}, '2': {'files': 4}})
        self.assertNotIn('example-owner', json.dumps(snapshot))
        css = next(r for r in snapshot['requests'] if r['path'] == '/site/2/build/assets/app-9f3a.css')
        self.assertEqual((css['site'], css['alias'], css['file']), ('2', 'app.css', 'app.css'))
        report = acceptance.browser_sites_report(
            {'outcome': 'pass', 'browser_fixture': snapshot},
            '[console] [netsurf] fetch url=https://fixture.test/site/2/ stage=done code=200 errno=0\n'
            '[console] [netsurf] fetch url=https://elsewhere.test/x stage=error code=0 errno=110\n'
            '[console] [netsurf] layout slow elapsed_us=9 class=local accepted=0\n')
        self.assertEqual((report['fixture_fetch_errors'], report['other_host_fetch_errors'], report['layout_rejections']), ([], 1, 1))
        self.assertIn('/site/2/it/progetti', report['missing_from_snapshot'])
        result = {'outcome': 'pass', 'browser_fixture': snapshot}
        acceptance.browser_sites_report(result, '[console] [netsurf] fetch url=https://fixture.test/site/2/x.css stage=error code=0\n')
        self.assertEqual((result['outcome'], result['reason']), ('fail', 'NetSurf fetch errors on fixture-served site resources'))


class RunnerIntegrationTests(unittest.TestCase):
    """acceptance cases through run.run_case and the scripted QEMU boundary."""
    def setUp(self):
        SCRATCH.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=SCRATCH); self.root = Path(self.temp.name)
        self.image = self.root / 'image.raw'; self.image.write_bytes(b'canonical immutable image')
        self.firmware = self.root / 'bios.bin'; self.firmware.write_bytes(b'firmware fixture')
        self.cgroup = self.root / 'cgroup'; self.cgroup.mkdir()
        (self.cgroup / 'memory.events').write_text('oom 0\noom_kill 0\n')
        self.img = self.root / 'fake-img'
        self.img.write_text('#!/usr/bin/env python3\nimport pathlib,sys\nassert sys.argv[1:4]==["create","-f","qcow2"]\n'
                            'pathlib.Path(sys.argv[-1]).write_bytes(b"fixture overlay")\n')
        self.img.chmod(0o755)
        self.script = self.root / 'scenario.json'
        self.screen = self.root / 'desktop.ppm'; self.screen.write_bytes(desktop_frame())
        self.patches = [patch.object(runner, 'git_identity', return_value={'revision': 'fixture', 'dirty': False}),
                        patch.dict(os.environ, {'CIUKI_FAKE_SCRIPT': str(self.script)})]
        for p in self.patches:
            p.start()
        self.profile = runner.load(ROOT / 'tests/profiles/qemu-t23.json')

    def tearDown(self):
        for p in reversed(self.patches):
            p.stop()
        self.temp.cleanup()

    def run_case(self, c, **scenario):
        scenario = {'normal_boot': True, 'screen': str(self.screen), **scenario}
        self.script.write_text(json.dumps(scenario))
        host = FakeHost(self.cgroup)
        with res.ExclusiveLock(self.root / 'shared.lock'):
            result, directory = runner.run_case(self.root, 'acc-fixture', c, self.profile, self.image,
                                                str(ROOT / 'tests/host/fake_qemu.py'), self.firmware, host=host, qemu_img=str(self.img))
        self.assertEqual(host.scope_pids(self.cgroup), [])
        if getattr(host, 'tx', None) is not None:
            os.close(host.tx); os.close(host.rx)
        self.assertEqual(json.loads((directory / 'result.json').read_text())['outcome'], result['outcome'])
        return result, directory

    def restart_case(self):
        quick = [{'keys': ['ctrl+esc', 'u'], 'pause': 0.05}]
        return case(
            id='restart', resets=1, timeout=60, items={'1': ['boot'], '7': ['restart'], '8': ['off']},
            steps=[{'template': 'boot', 'prefix': 'b1', 'check': 'boot', 'settle': 0.1, 'timeout': 60},
                   {'name': 'b1-screen', 'check': 'boot', 'screen': {'geometry': [1024, 768], 'desktop': True}},
                   {'name': 'restart', 'check': 'restart', 'arm': 'reset', 'actions': [*quick, {'keys': 'r', 'pause': 0.05}, {'keys': 'ret'}]},
                   {'name': 'power', 'check': 'restart', 'timeout': 20,
                    'wait': [r'^\[power\] reboot requested by pid \d+ ', r'^\[storage\] completion drive=C kind=durable result=0 ',
                             r'^\[power\] stage=reset fallback=8042-triple-fault$'],
                    'fail': r'^\[storage\] shutdown (?:busy|refused) '},
                   {'name': 'reset', 'check': 'restart', 'wait_event': 'RESET', 'timeout': 20},
                   {'template': 'boot', 'prefix': 'b2', 'check': 'restart', 'settle': 0.1, 'timeout': 60},
                   {'name': 'shutdown', 'check': 'off', 'arm': 'shutdown', 'actions': [*quick, {'keys': 's', 'pause': 0.05}, {'keys': 'ret'}]},
                   {'name': 'off', 'check': 'off', 'wait_event': 'SHUTDOWN', 'timeout': 20}],
            assertions=[{'name': 'clean', 'check': 'restart', 'pattern': r'^\[scandisk\] start drive=C ', 'from': 'b2-loader', 'max': 0}])

    def restart_scenario(self):
        reset = {'qmp_event': {'event': 'RESET', 'data': {'guest': True, 'reason': 'guest-reset'}}}
        off = {'qmp_event': {'event': 'SHUTDOWN', 'data': {'guest': True, 'reason': 'guest-shutdown'}}}
        return {'records': ['L:CPU signature=00000673', 'SELECT_READY'],
                'on_key': {'n#1': boot_lines(), 'n#2': boot_lines(build='second'),
                           'r#1': ['[power] reboot requested by pid 3 attempt=1 progress=0 storage=pending',
                                   '[storage] completion drive=C kind=durable result=0 incomplete=0 generation=1 group=1',
                                   '[power] stage=reset fallback=8042-triple-fault', reset, {'delay': 0.2},
                                   'L:CPU signature=00000673', 'SELECT_READY'],
                           's#1': ['[power] power off requested by pid 3 attempt=1 progress=0 storage=pending', off]}}

    def test_restart_and_shutdown_pass(self):
        result, directory = self.run_case(self.restart_case(), **self.restart_scenario())
        self.assertEqual(result['outcome'], 'pass', (result['reason'], result.get('acceptance', {}).get('steps')))
        self.assertEqual(result['evidence_kind'], 'acceptance')
        self.assertIsNone(result['selector'])
        args = result['qemu']['arguments']
        self.assertNotIn('-no-reboot', args); self.assertIn('-no-shutdown', args); self.assertNotIn('-fw_cfg', args)
        acc = result['acceptance']
        self.assertEqual({k: v['status'] for k, v in acc['checks'].items()}, {'boot': 'pass', 'restart': 'pass', 'off': 'pass'})
        self.assertEqual([o['event'] for o in acc['observations']], ['RESET', 'SHUTDOWN'])
        self.assertEqual(acc['boots'], 2)
        self.assertEqual(acc['variables']['b2_build'], 'second')
        self.assertEqual(acc['items']['8'], {'checks': ['off'], 'status': 'pass'})
        self.assertEqual(sorted(p.name for p in directory.iterdir()), ['result.json', 'screen-04-b1-screen.ppm', 'serial.log'])
        self.assertIn(b'[power] stage=reset fallback=8042-triple-fault\n', (directory / 'serial.log').read_bytes())

    def test_unarmed_reset_forbidden_line_and_timeout_fail(self):
        scenario = self.restart_scenario()
        scenario['on_key']['n#1'] = boot_lines() + [{'qmp_event': {'event': 'RESET', 'data': {'guest': True, 'reason': 'guest-reset'}}}]
        result, directory = self.run_case(self.restart_case(), **scenario)
        self.assertEqual(result['outcome'], 'fail')
        self.assertIn('unexpected guest reset', result['reason'])
        self.assertTrue((directory / 'run.qcow2').is_file())          # a failing run keeps its overlay
        self.assertTrue((directory / 'screen-fail.ppm').is_file())
        scenario = self.restart_scenario()
        scenario['on_key']['n#1'] = boot_lines()[:2] + ['Ciuki VMM panic: halting.', 'more diagnostics']
        result, directory = self.run_case(self.restart_case(), **scenario)
        self.assertEqual(result['outcome'], 'fail')
        self.assertRegex(result['reason'], r'^forbidden serial line \d+: Ciuki VMM panic')
        self.assertIn(b'more diagnostics', (directory / 'serial.log').read_bytes())
        # No stimulus after a forbidden line: the step in progress is the failure.
        statuses = [step['status'] for step in result['acceptance']['steps']]
        self.assertEqual(statuses[statuses.index('fail') + 1:], ['not_run'] * (len(statuses) - statuses.index('fail') - 1))
        self.assertEqual(statuses[:statuses.index('fail')], ['pass'] * statuses.index('fail'))
        result, _ = self.run_case(case(id='late', timeout=30, steps=[{'name': 'w', 'wait': '^never$', 'timeout': 1}]),
                                  records=['L:CPU signature=1'])
        self.assertEqual(result['outcome'], 'fail')
        self.assertIn('step w: timeout after 1 s waiting for ^never$', result['reason'])
        self.assertEqual(result['acceptance']['steps'][0]['failure_kind'], 'timeout')

    def test_pending_case_never_launches_qemu(self):
        c = case(id='later', pending={'directive': 'fx-is5boot', 'requires': ['docs/design/no-such-design.md'], 'reason': 'not merged'})
        result, directory = self.run_case(c)
        self.assertEqual(result['outcome'], 'not_run')
        self.assertIn('pending fx-is5boot', result['reason'])
        self.assertFalse((directory / 'fake-arguments.json').exists())

    def test_required_pending_install_case_is_incomplete_without_launch(self):
        c = case(id='install-calibration', pending={'directive': 'fx-games20c', 'requires': [],
                 'blocking': True, 'reason': 'needs-lead-calibration: installer destination and sound menu'})
        result, directory = self.run_case(c)
        self.assertEqual(result['outcome'], 'not_run')
        self.assertTrue(result['reason'].startswith('required pending fx-games20c'))
        self.assertFalse((directory / 'fake-arguments.json').exists())
        state = acceptance.verdict([{'case': c['id'], 'outcome': result['outcome'], 'evidence': result}])
        self.assertEqual(state, ('INCOMPLETE', [], [c['id']]))

    @unittest.skipUnless(HAVE_XORRISO, 'xorriso required')
    def test_generated_media_insert_and_eject(self):
        c = case(id='cd', timeout=60, devices={'optical': 'empty'},
                 media={'std': {'generator': 'xorriso', 'level': 3, 'label': 'CIUKI_STD'}},
                 steps=[{'name': 'insert', 'check': 'cd', 'actions': [{'media': {'insert': 'std'}}]},
                        {'name': 'mount', 'check': 'cd', 'timeout': 20,
                         'wait': [r'^\[storage\] optical slot=\d+ drive=(?P<cd>[A-Z]) mode=ro generation=\d+ result=\d+ reason=mounted$',
                                  r'^\[user \d+\] \[desktop\] autoplay probe drive=<<cd>> entries=<<media_std_root>> ']},
                        {'name': 'eject', 'check': 'cd', 'actions': [{'media': {'eject': True}}]},
                        {'name': 'gone', 'check': 'cd', 'timeout': 20,
                         'wait': r'^\[storage\] optical slot=\d+ drive=<<cd>> mode=ro generation=\d+ result=-?\d+ reason=no_medium$'}])
        scenario = {'records': ['L:CPU signature=1'],
                    'on_qmp': {'blockdev-change-medium#1': ['[storage] optical slot=2 drive=D mode=ro generation=2 result=3 reason=mounted',
                                                            D + ' autoplay probe drive=D entries=3 run_kind=0 music=0 pictures=0 result=0'],
                               'eject#1': ['[storage] optical slot=2 drive=D mode=ro generation=3 result=-123 reason=no_medium']}}
        result, directory = self.run_case(c, **scenario)
        self.assertEqual(result['outcome'], 'pass', (result['reason'], result.get('acceptance', {}).get('steps')))
        args = result['qemu']['arguments']
        self.assertIn('ide-cd,drive=ciuki-optical,bus=ide.1,unit=0', args)
        media = result['acceptance_media'][0]
        self.assertEqual((media['name'], media['label'], media['root_entries']), ('std', 'CIUKI_STD', ['/DATA', '/LONG_DIRECTORY', '/README.TXT']))
        self.assertEqual(result['acceptance']['media']['std']['path'], 'media-std.iso')
        self.assertFalse(any(p.suffix == '.iso' for p in directory.iterdir()))   # passing runs keep no images

    def test_disk_inventory_of_an_installer_case(self):
        c = case(id='inst', timeout=60, disk=DiskAndSummaryTests.SPEC,
                 steps=[{'name': 'go', 'check': 'install', 'wait': '^L:CPU signature='}])
        good = {'added': [{'path': r'C:\Q3Ademo\quake3.exe', 'kind': 'file', 'size': 3, 'sha256': 'ab'},
                          {'path': r'C:\Q3Ademo\demoq3\pak0.pk3', 'kind': 'file', 'size': 9, 'sha256': 'x'}],
                'links': [{'path': 'C:\\WINDOWS\\Start Menu\\Programs\\Q\\quake3.lnk', 'target': '<foreign>'}],
                'menu_links': ['C:\\WINDOWS\\Start Menu\\Programs\\Q\\quake3.lnk']}
        with patch.object(runner.win32_inventory, 'overlay_delta', return_value=good) as delta:
            result, _ = self.run_case(c, records=['L:CPU signature=1'])
        self.assertEqual(result['outcome'], 'pass', result['reason'])
        self.assertEqual(delta.call_count, 1)
        self.assertEqual(result['acceptance']['disk']['status'], 'pass')
        with patch.object(runner.win32_inventory, 'overlay_delta', return_value={**good, 'menu_links': []}):
            result, _ = self.run_case(c, records=['L:CPU signature=1'])
        self.assertEqual((result['outcome'], result['reason']), ('fail', 'acceptance checks failed: disk'))

    def test_site_snapshots_absent_or_served(self):
        c = {'id': 'site', 'probe': 'browser', 'profile': 'qemu-t23', 'item': 5, 'title': 'site',
             'selector': 'f6:browser run={run_id} url=https://fixture.test/site/1/', 'device_exceptions': {'network': 'i82559er'},
             'browser_sites': {'source': 'build/downloads/ns-sites-none', 'env': 'CIUKI_TEST_SITES', 'count': 1}, 'timeout': 30,
             'expected': {'terminal': 'END', 'predicates': [{'where': {'event': 'DATA', 'case': 'page'}, 'fields': {'status': {'eq': 200}},
                                                             'exact_count': 1}]}}
        result, directory = self.run_case(c, normal_boot=False)
        self.assertEqual(result['outcome'], 'not_run')
        self.assertIn('site snapshots absent', result['reason'])
        self.assertFalse((directory / 'fake-arguments.json').exists())
        site = self.root / 'snap' / 'www.example-owner.test'; site.mkdir(parents=True)
        (site / 'index.html').write_text('<p>x</p>')
        records = [{'event': 'BEGIN'}, {'event': 'DATA', 'case': 'page', 'status': '200'}, {'event': 'END', 'status': 'PASS'},
                   '[console] [netsurf] fetch url=https://fixture.test/site/1/ stage=done code=200 errno=0']
        with patch.dict(os.environ, {'CIUKI_TEST_SITES': str(self.root / 'snap')}):
            result, _ = self.run_case(c, normal_boot=False, records=records)
            self.assertEqual(result['outcome'], 'pass', result['reason'])
            self.assertEqual(result['browser_fixture']['sites'], {'1': {'files': 1}})
            self.assertEqual(result['browser_sites']['fetches_done'], 1)
            result, _ = self.run_case(c, normal_boot=False, records=records + [
                '[console] [netsurf] fetch url=https://fixture.test/site/1/app.css stage=error code=0 errno=104'])
            self.assertEqual((result['outcome'], result['reason']), ('fail', 'NetSurf fetch errors on fixture-served site resources'))
        self.assertNotIn('example-owner', json.dumps(result))


class MainTests(unittest.TestCase):
    def test_case_selection_gate_and_summary(self):
        suite = runner.load_suite('acc-t23')
        picked = runner.select_cases(suite, ['acc-quake3', 'acc-netsurf-site-2'])
        self.assertEqual([c['id'] for c in picked['cases']], ['acc-quake3', 'acc-netsurf-site-2'])
        with self.assertRaises(res.Refusal):
            runner.select_cases(suite, ['nope'])
        with self.assertRaises(res.Refusal):
            runner.select_cases({'cases': [{'id': 'boot', 'probe': 'boot'}]}, ['boot'])

    def test_main_runs_cases_independently_until_a_gate_and_writes_the_page(self):
        calls = []
        def fake_run(root, suite, c, profile, image, executable, firmware, keep=False):
            calls.append(c['id'])
            failed = c['id'] in ('acc-quake3', 'acc-desktop-cd') and self.gate_fails or c['id'] == 'acc-quake3'
            result = {'run_id': 'r' + str(len(calls)), 'outcome': 'fail' if failed else 'pass', 'reason': 'x',
                      'acceptance': {'item': c.get('item'), 'title': c.get('title'), 'checks': {'boot': {'status': 'fail' if failed else 'pass'}}},
                      'image': {'sha256': 'cd' * 32}, 'build_manifest': {'revision': 'r'}}
            directory = SCRATCH / 'main' / c['id']; directory.mkdir(parents=True, exist_ok=True)
            return result, directory
        out = SCRATCH / 'main-root'
        (out / 'build/test-runs').mkdir(parents=True, exist_ok=True)
        image = out / 'image.raw'; image.write_bytes(b'image')
        for gate_fails, expected in ((False, len(SUITE['cases'])), (True, 1)):
            self.gate_fails = gate_fails; calls.clear()
            (out / 'build/test-runs/acc-t23').mkdir(parents=True, exist_ok=True)
            with patch.object(runner, 'ROOT', out), patch.object(runner, 'run_case', fake_run), \
                    patch.object(runner, 'load_suite', lambda name: copy.deepcopy(SUITE)), \
                    patch.object(runner.res, 'common_lock', lambda root: root / 'lock'), \
                    patch.object(runner.res, 'SlotLock', NoLock), patch.object(runner.Host, 'preflight', lambda self, root: 3), \
                    patch.object(runner.shutil, 'which', lambda name: '/usr/bin/true'), \
                    patch.object(runner, 'find_firmware', lambda executable: image), \
                    patch.object(runner, 'load', lambda path: PROFILE if 'profiles' in str(path) else json.loads(Path(path).read_text())), \
                    patch('sys.stdout'):
                (out / 'config').mkdir(exist_ok=True)
                (out / 'config/toolchain.json').write_text(json.dumps({'qemu_machine': PROFILE['machine']}))
                code = runner.main(['acc-t23', '--image', str(image)])
            self.assertEqual(code, 1)
            self.assertEqual(len(calls), expected)
            page = (out / 'build/test-runs/acc-t23/summary.txt').read_text()
            self.assertIn('verdict BLOCKED', page)
            data = json.loads((out / 'build/test-runs/acc-t23/summary.json').read_text())
            self.assertEqual(len(data['cases']), len(SUITE['cases']))
            if gate_fails:
                self.assertEqual(data['cases'][1]['reason'], 'gate acc-desktop-cd failed')
                self.assertEqual(data['cases'][1]['item'], 2)


class NoLock:
    def __init__(self, path): pass
    def __enter__(self): return self
    def __exit__(self, *a): return False


SUITE = runner.load_suite('acc-t23')
PROFILE = runner.load(ROOT / 'tests/profiles/qemu-t23.json')


if __name__ == '__main__':
    unittest.main()
