"""H14 (sec-user-06): the security runtime cases' host parser and fixture
wiring, never a run. The suites tests/suites/sec-users.json (SU-01..14 and
the round-1/round-2 additions of user-security-contract.md 10.1) and the
acc-t23 account case parse exactly as run.py parses them before a boot; every
serial pattern they wait for is matched against the record the production
source writes (rendered from its printf format); the LOGON-aware templates,
the power cut and the LOGON normal-boot milestones replay on synthetic serial
captures; the sectest model and the self-test KDF run on the host. QEMU, the
canonical image and the hardware are the lead's (directive sec-user-06).

Run: python3 -B -m unittest discover -s tests/host/security -p test_runner_contract.py
"""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
for path in (ROOT / 'scripts/test', ROOT / 'tests/host'):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))
import acceptance  # noqa: E402
import run as runner  # noqa: E402
import test_acceptance as ta  # noqa: E402  (FakeQMP, drive, case, ppm: helpers only)

OUT = ROOT / 'build/host/security/runner-contract'
CLANG = ['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=all']


def load(name):
    return json.loads((ROOT / 'tests/suites' / name).read_text())


def render(source, needle, order=(), exact=False, **values):
    """The first printf format of source containing needle (or, exact, equal
    to it), with sample values."""
    text = (ROOT / source).read_text()
    match = re.search(r'"(%s)"' % re.escape(needle) if exact else
                      r'"((?:[^"\\]|\\.)*%s(?:[^"\\]|\\.)*)"' % re.escape(needle), text)
    assert match, (source, needle)
    fmt = match[1].replace('\\"', '"').replace('\\n', '')
    names = iter(order)

    def value(m):
        if m[0] == '%%':
            return '%'
        name = next(names, None)
        if name is not None and name in values:
            return str(values[name])
        return {'s': 'x', 'c': 'D', 'x': '1a', 'd': '0'}.get(m[0][-1], '1')
    return re.sub(r'%(?:%|[-0-9.]*(?:ll|z|l)?[udxsc])', value, fmt)


def pattern(text, **captures):
    """A suite pattern with its <<placeholders>> replaced (by a value or any token)."""
    return re.compile(acceptance.PLACEHOLDER.sub(lambda m: re.escape(captures[m[1]]) if m[1] in captures else r'[A-Za-z0-9_]+', text))


def steps_of(case):
    return {s['name']: s for s in acceptance.expand_steps(case['steps'])}


def waits(step):
    w = step.get('wait', [])
    return [w] if isinstance(w, str) else list(w)


class SuiteWiring(unittest.TestCase):
    """Both suites parse as run.py parses them; acceptance.sh lists them."""
    @classmethod
    def setUpClass(cls):
        cls.acc = runner.load_suite('acc-t23')
        cls.sec = runner.load_suite('sec-users')
        cls.raw = load('sec-users.json')

    def test_suites_parse_and_list_without_qemu(self):
        self.assertTrue(self.acc['acceptance'] and self.sec['acceptance'])
        for suite, data in (('acc-t23', self.acc), ('sec-users', self.sec)):
            with self.subTest(suite=suite):
                for case in data['cases']:
                    self.assertIn(runner.case_kind(case), ('acceptance', 'probe'))
                    if runner.case_kind(case) == 'acceptance':
                        self.assertIsNone(acceptance.selector(case))   # a normal boot: no fw_cfg request
                        self.assertFalse({'selector', 'patches', 'loader_options', 'fault'} & set(case))
                        self.assertEqual(case['profile'], 'qemu-t23')
                listing = subprocess.run(['bash', str(ROOT / 'scripts/test/acceptance.sh'), '--suite', suite, '--list'],
                                         cwd=ROOT, capture_output=True, text=True, timeout=120)
                self.assertEqual(listing.returncode, 0, listing.stderr)
                for case in data['cases']:
                    self.assertIn(case['id'], listing.stdout)
                self.assertIn('%s: %d cases valid' % (suite, len(data['cases'])), listing.stdout)
        refused = subprocess.run(['bash', str(ROOT / 'scripts/test/acceptance.sh'), '--suite=f0-smoke', '--list'],
                                 cwd=ROOT, capture_output=True, text=True, timeout=60)
        self.assertEqual(refused.returncode, 2)

    def test_owner_security_items_in_acc_t23(self):
        case = next(c for c in self.acc['cases'] if c['id'] == 'acc-accounts')
        self.assertEqual(sorted(int(n) for n in case['items']), list(range(10, 19)))
        steps = steps_of(case)
        self.assertEqual(steps['b1-loader']['actions'], [{'keys': 'n'}])
        # Every item has a step or an assertion on a serial record, never a screenshot alone.
        checks = {s.get('check', 'main') for s in steps.values() if s.get('wait') or s.get('wait_event')}
        checks |= {a['check'] for a in case['assertions']}
        for number, names in case['items'].items():
            self.assertTrue(set(names) & checks, number)
        self.assertFalse(any('screen' in s for s in steps.values()))
        names = {a['name'] for a in case['assertions']}
        self.assertTrue({'autologon-requested', 'bob-programs-refused', 'bob-game-folder', 'bob-hklm-refused',
                         'bob-other-profile-refused', 'bob-machine-setting-refused', 'guest-root-refused',
                         'saver-gate-entered', 'kernel-secure-attention', 'no-autologon-after-password',
                         'no-secret-on-serial'} <= names)
        self.assertEqual(case['resets'], 1)
        self.assertEqual([s['wait_event'] for s in steps.values() if 'wait_event' in s], ['RESET', 'SHUTDOWN'])
        total = sum(c['timeout'] for c in self.acc['cases'])
        # Games, LAN guests (f6-28), and installer/sound setup (fx-games20c)
        # retain their separate 2,400 s bounds.
        extra = [c for c in self.acc['cases'] if 'dos_game' in c or 'guests' in c or c['id'].startswith('acc-install-')]
        self.assertTrue(all(c['timeout'] <= 2400 for c in extra))
        self.assertLessEqual(total, 7200 * 4 + 2400 * len(extra))

    def test_contract_cases_are_covered(self):
        """SU-01..14 and each runtime case the directive names have a case or a
        pending declaration naming the host evidence (items 1..20)."""
        covered = set()
        for c in self.raw['cases']:
            covered |= {int(n) for n in c.get('items', {str(c['item']): []})}
        self.assertEqual(sorted(covered), list(range(1, 21)))
        for su in range(1, 15):
            # SU-05..SU-08 run inside su-04-roles (items 4..8, one setup of the three roles).
            case = next((c for c in self.raw['cases'] if 'SU-%02d' % su in c['title'] or str(su) in c.get('items', {})), None)
            self.assertIsNotNone(case, 'SU-%02d' % su)
            self.assertIn('SU-%02d' % min(su, 4) if 5 <= su <= 8 else 'SU-%02d' % su, case['title'])
        named = {
            'challenge/lock/epoch': 'su-r2-gate', 'STATUS/checkpoint': 'su-r2-status', 'profile': 'su-r2-profile',
            'transition/reboot': 'su-01-ready', 'launch/roles/batch/seed': 'su-04-roles',
            'protected installer and DLL': 'su-04-roles', 'DOS short path': 'su-r1-dos', 'restart': 'su-r1-restart',
            'console and libreg': 'su-r1-console',
        }
        ids = {c['id'] for c in self.raw['cases']}
        for what, cid in named.items():
            self.assertIn(cid, ids, what)
        roles = next(c for c in self.raw['cases'] if c['id'] == 'su-04-roles')
        self.assertEqual(set(roles['items']), {'4', '5', '6', '7', '8'})
        tested = {re.search(r'check=([a-z-]+) ', a['pattern'])[1] for a in roles['assertions'] if 'sectest check=' in a['pattern']}
        self.assertTrue({'installer-store', 'installer-shortcuts', 'machine-registry', 'machine-setting', 'foreign-folder',
                         'environment', 'other-profile', 'raw-disk', 'account-store'} <= tested)

    def test_pending_cases_are_not_run_and_name_their_evidence(self):
        pending = [c for c in self.raw['cases'] if c.get('pending')]
        self.assertEqual(sorted(c['id'] for c in pending),
                         ['su-02-setup', 'su-10-resume', 'su-r1-console', 'su-r1-dos', 'su-r1-restart', 'su-r2-profile',
                          'su-r2-status'])
        for case in pending:
            with self.subTest(case=case['id']):
                self.assertRegex(case['pending']['directive'], r'H\d\d')
                self.assertTrue(case['pending']['reason'])
                session = acceptance.Session(case, '00000000', OUT, ROOT)
                with self.assertRaises(acceptance.EvidenceNotRun):
                    session.precheck()
        rows = [{'case': c['id'], 'outcome': 'not_run', 'reason': 'pending x: y', 'evidence': {'reason': 'pending x: y'}}
                for c in pending]
        self.assertEqual(acceptance.verdict(rows)[0], 'ACCEPTED')

    def test_typed_secrets_are_asserted_off_the_serial_port(self):
        """Every case that types a password asserts it never appears on the
        serial port; no record format prints a password field."""
        for suite in (load('acc-t23.json'), load('sec-users.json')):
            for case in suite['cases']:
                typed = set()
                for step in acceptance.expand_steps(case.get('steps', [])):
                    for action in step.get('actions', []):
                        if 'text' in action and re.fullmatch(r'[A-Za-z]+-pass-\d+', action['text']):
                            typed.add(action['text'])
                if not typed:
                    continue
                with self.subTest(case=case['id']):
                    guard = next(a for a in case['assertions'] if a['name'] == 'no-secret-on-serial')
                    self.assertEqual((guard['from'], guard['to'], guard['max']), ('start', 'end', 0))
                    for secret in typed:
                        self.assertRegex(secret, guard['pattern'][len('^.*'):])
        # The records themselves (LOGON, LOGON_UI, the account audit, sectest)
        # take no password, confirmation or secret argument.
        for source in ('apps/logon/coordinator.c', 'apps/logon/ui.c', 'apps/accounts/kernel_adapter.c', 'apps/systools/sectest_model.c'):
            text = (ROOT / source).read_text()
            calls = [c[0] for c in re.finditer(r'(?:lgn_note|snprintf|emit)\([^;]*;', text)
                     if re.search(r'"\[logon(?:-ui)?\]|"\[accounts\] manage|"sectest ', c[0])]
            self.assertTrue(calls, source)
            for call in calls:
                args = call.split('",', 1)[-1]
                self.assertNotRegex(args, r'password|secret|confirm|old_', (source, call[:80]))



class RecordFormats(unittest.TestCase):
    """The suites' patterns match the records the production sources write."""
    @classmethod
    def setUpClass(cls):
        cls.acc = next(c for c in load('acc-t23.json')['cases'] if c['id'] == 'acc-accounts')
        cls.steps = steps_of(cls.acc)
        cls.sec = {c['id']: c for c in load('sec-users.json')['cases']}

    def match(self, regex, line, **captures):
        self.assertRegex(line, pattern(regex, **captures))

    def test_kernel_launch_and_milestones(self):
        logon = render('src/kernel/proc/supervisor.c', '[supervisor] bootstrap: trusted logon pid=', ['pid'], pid=4)
        self.assertEqual(logon, '[supervisor] bootstrap: trusted logon pid=4')
        desktop = render('src/kernel/proc/security.c', '[security] session %u %s pid=',
                         ['session', 'what', 'pid', 'pgid', 'uid', 'gid', 'groups'],
                         session=1, what='desktop', pid=8, pgid=8, uid=1000, gid=545, groups='544,545')
        self.assertEqual(desktop, '[security] session 1 desktop pid=8 pgid=8 uid=1000 gid=545 groups=544,545')
        names = dict(runner.NORMAL_BOOT_MILESTONES)
        self.assertTrue(names['desktop_spawn'].match(logon.encode()))
        m = names['desktop_pid'].match(desktop.encode())
        self.assertEqual((m[1], m[2]), (b'8', b'8'))
        self.assertFalse(names['desktop_pid'].match(desktop.replace(' desktop ', ' w32srv ').encode()))
        boot = acceptance.boot_template('b1', 'boot')
        self.match(boot[2]['wait'], desktop)
        self.match(boot[2]['wait'], '[supervisor] desktop pid=3 pgid=3')
        logon_steps = acceptance.logon_steps('bob', 'roles', 'Bob', 'Bob-pass-2026')
        self.match(logon_steps[3]['wait'][1], desktop.replace('uid=1000', 'uid=1001').replace('groups=544,545', 'groups=545'),
                   bob_session='1', bob_uid='1001')
        for a in self.acc['assertions']:
            if a['name'] == 'ciukiuser-desktop':
                self.match(a['pattern'], desktop)
            if a['name'] == 'ciukiuser-w32srv':
                self.match(a['pattern'], desktop.replace(' desktop ', ' w32srv '))
        # The trusted gate (security.c console_enter/leave, grants.c timing).
        enter = render('src/kernel/proc/security.c', '[security] console enter session=',
                       ['session', 'reason', 'auth', 'physical'], session=2, reason='saver', auth=1, physical='')
        self.assertEqual(enter, '[security] console enter session=2 reason=saver authentication=1')
        self.match(next(a for a in self.acc['assertions'] if a['name'] == 'saver-gate-entered')['pattern'], enter, bob_session='2')
        leave = render('src/kernel/proc/security.c', '[security] console leave session=', ['session'], session=2)
        self.match(self.steps['saver-unlocked']['wait'][0], leave, bob_session='2')
        reasons = re.search(r'return r==1 \? "(\w+)" : r==2 \? "(\w+)" : r==3 \? "(\w+)" : r==4 \? "(\w+)"',
                            (ROOT / 'src/kernel/proc/security.c').read_text()).groups()
        self.assertEqual(reasons, ('lock', 'saver', 'resume', 'attention'))
        concealed = render('src/kernel/proc/grants.c', '[security] console concealed', ['n', 't', 'c'], n=35, t=410, c=1)
        restored = render('src/kernel/proc/grants.c', '[security] console restored', ['ms', 'c'], ms=220, c=1)
        latency = next(f for f in self.acc['figures'] if f['name'] == 'gate-latency')['pattern']
        self.match(latency, concealed); self.match(latency, restored)
        attention = render('src/kernel/proc/secure_console.c', '[security] secure attention source=keyboard result=',
                           ['r'], r=-16)
        self.match(next(a for a in self.acc['assertions'] if a['name'] == 'kernel-secure-attention')['pattern'], attention)
        drained = render('src/kernel/proc/secure_console.c', '[security] session drained session=', ['s', 'p'], s=3, p=2)
        self.match(next(a for a in self.sec['su-11-logoff']['assertions'] if a['name'] == 'old-session-drained')['pattern'],
                   drained, bob_session='3')

    def test_logon_records(self):
        user = '[user 9] '
        source = 'apps/logon/coordinator.c'
        line = lambda needle, order=(), **v: user + render(source, needle, order, **v)
        modes = re.search(r'names\[\]=\{"none","starting","welcome","loading","hidden","locked","security",\s*'
                          r'"ending","unavailable","power","enroll"\}', (ROOT / source).read_text())
        self.assertIsNotNone(modes)
        welcome = line('[logon] screen %s', ['m'], m='welcome')
        self.match(self.steps['lo1-done']['wait'][2], welcome)
        self.match(self.steps['lo1-done']['wait'][0], line('[logon] session %u ending reason=', ['s', 'r'], s=1, r=1))
        self.match(self.steps['lo1-done']['wait'][1], line('[logon] session %u ended reason=', ['s', 'r'], s=1, r=1), lo1_session='1')
        self.assertIn('LGN_END_CRASH = 0, LGN_END_LOGOFF, LGN_END_SHUTDOWN', (ROOT / 'apps/logon/logon.h').read_text())
        self.match(acceptance.logon_steps('bob', 'c', 'Bob', 'x' * 8)[2]['wait'],
                   line('[logon] authenticated uid=', ['u'], u=1001))
        self.match(acceptance.logon_steps('bob', 'c', 'Bob', 'x' * 8)[3]['wait'][0],
                   line('[logon] session %u created uid=', ['s', 'u'], s=2, u=1001), bob_uid='1001')
        self.match(acceptance.logon_steps('bob', 'c', 'Bob', 'x' * 8)[3]['wait'][2],
                   line('[logon] session %u running uid=', ['s', 'u', 'ms'], s=2, u=1001, ms=812), bob_session='2', bob_uid='1001')
        self.match(next(a for a in self.acc['assertions'] if a['name'] == 'autologon-requested')['pattern'],
                   line('[logon] automatic logon requested'))
        self.match(self.sec['su-12-recovery']['assertions'][1]['pattern'],
                   line('[logon] automatic logon not applicable safe=', ['s'], s=1))
        saver = line('[logon] session %u locked by the %s', ['s', 'w'], s=2, w='screen saver')
        self.match(self.steps['saver-locked']['wait'][0], saver, bob_session='2')
        su09 = steps_of(self.sec['su-09-lock'])
        self.match(su09['menu-locked']['wait'][0], line('[logon] session %u locked by the desktop', ['s'], s=1), b1_session='1')
        self.match(su09['cad-locked']['wait'][0], line('[logon] session %u locked by the security menu', ['s'], s=1),
                   b1_session='1')
        self.match(self.steps['saver-unlocked']['wait'][1], line('[logon] session %u unlocked', ['s'], s=2), bob_session='2')
        refused = line('[logon] authentication refused action=', ['a', 's'], a=3, s=-13)
        self.match(self.steps['saver-refused']['wait'][0], refused)
        su03 = steps_of(self.sec['su-03-refusal'])
        self.match(su03['unknown-refused']['wait'][0], refused.replace('action=3', 'action=2'))
        restart = line('[logon] %s requested at the logon screen', ['what'], what='restart')
        self.match(self.steps['wprestart-power']['wait'][0], restart)
        self.match(self.steps['wpoff-power']['wait'][0], restart.replace('restart', 'shutdown'))
        self.match(self.steps['cad-menu']['wait'][0], line('[logon] screen %s', ['m'], m='security'))
        self.match(self.steps['cad-return']['wait'][1], line('[logon] screen %s', ['m'], m='hidden'))
        selftest = line('[logon] self-test: security menu returned result=', ['e'], e=0)
        self.assertEqual(selftest, user + '[logon] self-test: security menu returned result=0')

    def test_logon_ui_views(self):
        source = (ROOT / 'apps/logon/ui.c').read_text()
        views = re.search(r'views\[\]=\{([^}]*)\}', source)[1]
        views = re.findall(r'"([a-z-]+)"', views)
        enum = re.search(r'enum lui_view \{([^}]*)\}', (ROOT / 'apps/logon/ui.h').read_text())[1]
        self.assertEqual(len(views), len(re.findall(r'LUI_VIEW_\w+', enum)))
        self.assertEqual(views[:7], ['list', 'password', 'typed', 'confirm-logoff', 'confirm-power', 'blank-lock', 'menu'])
        line = '[user 10] ' + render('apps/logon/ui.c', '[logon-ui] view mode=%s', ['m', 'v', 'i', 'a', 's'],
                                     m='welcome', v='list', i=4, a=2, s='none')
        self.assertEqual(line, '[user 10] [logon-ui] view mode=welcome view=list items=4 accounts=2 status=none')
        logon = acceptance.logon_steps('bob', 'c', 'Bob', 'x' * 8)
        self.match(logon[0]['wait'], line)
        self.assertNotRegex(line.replace('accounts=2', 'accounts=0'), pattern(logon[0]['wait']))
        self.match(logon[1]['wait'], line.replace('view=list', 'view=typed'))
        self.match(self.steps['saver-refused']['wait'][1],
                   line.replace('mode=welcome view=list', 'mode=locked view=password').replace('none', 'incorrect'))
        self.match(self.steps['wprestart-confirm']['wait'], line.replace('view=list items=4', 'view=confirm-power items=3'))
        self.match(self.steps['cad-menu']['wait'][1], line.replace('mode=welcome view=list items=4', 'mode=security view=menu items=5'))
        for cls in ('incorrect', 'wait', 'unavailable', 'other', 'none'):
            self.assertIn('"%s"' % cls, source)
        # Every view and mode a suite waits for is one the UI can record.
        for suite in (load('acc-t23.json'), load('sec-users.json')):
            for case in suite['cases']:
                for step in acceptance.expand_steps(case.get('steps', [])):
                    for regex in waits(step):
                        m = re.search(r'view mode=(\w+) view=([a-z-]+)', regex)
                        if m:
                            self.assertIn(m[2], views, (case['id'], step['name']))
                            self.assertIn('"%s"' % m[1], source, (case['id'], step['name']))

    def test_account_management_audit(self):
        source = 'apps/accounts/kernel_adapter.c'
        line = '[user 6] ' + render(source, '[accounts] manage op=', ['op', 'c', 't', 'type', 'r'],
                                    op='create', c=1000, t=1001, type='standard', r=0)
        self.assertEqual(line, '[user 6] [accounts] manage op=create caller=1000 target=1001 type=standard result=0')
        self.match(self.steps['newbob-done']['wait'], line)
        self.assertEqual(re.match(pattern(self.steps['newbob-done']['wait']), line)['newbob_uid'], '1001')
        self.assertRegex(line.replace('result=0', 'result=-1'), pattern(self.steps['newbob-done']['fail']))
        names = (ROOT / source).read_text()
        for op in ('create', 'password', 'enable', 'delete', 'rename', 'type', 'picture', 'update'):
            self.assertIn('="%s"' % op, names)
        types = (ROOT / 'apps/accounts/service.c').read_text()
        self.assertIn('"admin" : groups == ACCT_GUEST ? "guest" : groups ? "standard"', types)
        self.match(self.steps['guest-pw-done']['wait'], line.replace('create', 'password').replace('1001', '501').replace('standard', 'guest'))
        self.match(self.steps['cp1-pw-done']['wait'], line.replace('create', 'password').replace('1001', '1000').replace('standard', 'admin'))
        opened = '[user 6] ' + render('apps/accounts/server.c', 'open; account database available')
        self.match(self.sec['su-13-crash']['assertions'][0]['pattern'], '[user 6] [accounts] open; account database available')
        self.assertIn('open; account database available', opened)

    def test_desktop_saver_launch_and_end_session(self):
        start = '[user 7] ' + render('apps/desktop/client.c', '[desktop] saver start kind=%u source=%s', ['k', 's'], k=1, s='command')
        wake = '[user 7] ' + render('apps/desktop/client.c', '[desktop] saver wake protect=1 lock=', ['l'], l=0)
        self.match(self.steps['saver-start']['wait'][1], start)
        self.match(next(a for a in self.acc['assertions'] if a['name'] == 'saver-locked-before-desktop')['pattern'], wake)
        launch = '[user 7] ' + render('apps/desktop/desktop.c', 'launch name=%.40s pid=', ['n', 'p', 'e'], n='sectest', p=42, e=0)
        self.match(self.steps['stbob-done']['wait'][0], launch)
        self.match(self.steps['cp1-window']['wait'][0], launch.replace('sectest', 'control'))
        focus = '[user 7] ' + render('apps/desktop/input.c', 'focus pid=', ['pid', 'w', 'titled', 'internal', 'app'],
                                     pid=42, w=3, titled=1, internal=0, app='control')
        self.match(self.steps['cp1-window']['wait'][1], focus, cp1='42')
        su11 = steps_of(self.sec['su-11-logoff'])
        begin = '[console] ' + render('apps/desktop/client.c', '[desktop] end-session begin action=', ['a', 'n'], a=3, n=2)
        self.match(su11['veto-asked']['wait'], begin)
        veto = '[console] ' + render('apps/desktop/client.c', '[desktop] end-session veto pid=', ['p'], p=40)
        self.match(su11['veto-kept']['wait'], veto, note='40')
        agreed = '[console] ' + render('apps/desktop/client.c', '[desktop] end-session agreed action=', ['a', 'n', 'w'], a=3, n=2, w='no')
        self.match(su11['consent-done']['wait'][0], agreed)

    def test_sectest_records(self):
        source = 'apps/systools/sectest_model.c'
        begin = '[console] ' + render(source, 'sectest begin version=1', ['uid', 'gid', 'session', 'groups', 'role', 'expect', 'profile', 'r', 'v'],
                                      uid=1001, gid=545, session=3, groups='545', role='standard', expect='standard',
                                      profile='u000003e9', r=0, v='pass')
        self.assertEqual(begin, '[console] sectest begin version=1 uid=1001 gid=545 session=3 groups=545 role=standard '
                                'expect=standard profile=u000003e9 result=0 verdict=pass')
        done = self.steps['stbob-done']
        self.match(done['wait'][1], begin, newbob_uid='1001')
        end = '[console] ' + render(source, 'sectest end role=', ['role', 'c', 'p', 'f', 's'], role='standard', c=13, p=13, f=0, s=0)
        self.match(done['wait'][2], end)
        failed = '[console] ' + render(source, 'sectest check=%s expect=%s result=%d verdict=%s reason=',
                                       ['c', 'e', 'r', 'v', 'why'], c='programs', e='deny', r=0, v='fail', why='allowed')
        self.assertRegex(failed, pattern(done['fail'][2]))
        check = '[console] ' + render(source, 'sectest check=%s expect=%s result=%d verdict=%s', ['c', 'e', 'r', 'v'],
                                      exact=True, c='programs', e='deny', r=-13, v='pass')
        self.assertEqual(check, '[console] sectest check=programs expect=deny result=-13 verdict=pass')
        self.match(next(a for a in self.acc['assertions'] if a['name'] == 'bob-programs-refused')['pattern'], check)
        names = re.findall(r'"([a-z-]+)"', re.search(r'check_names\[ST_CHECKS\] = \{([^}]*)\}', (ROOT / source).read_text())[1])
        for suite in (load('acc-t23.json'), load('sec-users.json')):
            for case in suite['cases']:
                for a in case.get('assertions', []):
                    m = re.search(r'sectest check=([a-z-]+) ', a['pattern'])
                    if m:
                        self.assertIn(m[1], names, (case['id'], a['name']))
                for step in acceptance.expand_steps(case.get('steps', [])):
                    for action in step.get('actions', []):
                        if action.get('text', '').startswith('sectest'):
                            args = action['text'].split()[1:]
                            for arg in args:
                                self.assertRegex(arg, r'^(?:saver|all|--keep|--expect=(?:admin|standard|guest)|'
                                                      r'--(?:folder|foreign)=[A-Za-z0-9_-]{1,31}|--check=[a-z-]+)$')
        saver = '[console] ' + render(source, 'sectest saver result=', ['r'], r=0)
        self.match(self.steps['saver-start']['wait'][0], saver)

    def test_selftest_figure_line(self):
        desktop = (ROOT / 'apps/desktop/desktop.c').read_text()
        self.assertIn('"[selftest] registry_backup backup_ms=%llu result=%d bytes=%u files=%u"', desktop)
        self.assertIn('!strcmp(v->reason,"registry-backup")', desktop)
        syscall = (ROOT / 'src/kernel/core/syscall.c').read_text()
        hook = syscall.split('static int32_t sys_debug_write', 1)[1].split('\n}\n', 1)[0]
        self.assertIn('!memcmp(kbuf, "[selftest] ", 11) && selftest_active()', hook)
        self.assertIn('p->pid == supervisor_desktop_pid()', hook)
        self.assertLess(hook.index('klog("[user %u] %s"'), hook.index('selftest_submit(kbuf + 11)'))
        checks = (ROOT / 'src/kernel/core/selftest_checks.c').read_text()
        self.assertIn('token_value(backup_tokens, "backup_ms", -1)', checks)
        logon = (ROOT / 'apps/logon/coordinator.c').read_text()
        self.assertIn('g->selftest=!e && (flags&LGN_BOOT_SELF_TEST);', logon)
        self.assertIn('#define LGN_BOOT_SELF_TEST 0x00800000u', (ROOT / 'apps/logon/logon.h').read_text())
        self.assertIn('#define CBI_F_SELF_TEST        (1u << 23)', (ROOT / 'src/kernel/include/ciuki/boot_info.h').read_text())
        # The self-test's attention is the physical filter's transition, entered
        # by the input driver itself, refused outside a T boot and before LOGON.
        i8042 = (ROOT / 'src/kernel/drivers/i8042.c').read_text()
        self.assertIn('input_secure_domain(true, CIUKI_SECURITY_SECURE_ATTENTION);\n    if (security_attention_physical) '
                      'security_attention_physical();', i8042)
        entry = i8042.split('int input_selftest_attention(void)\n{', 1)[1].split('\n}\n', 1)[0]
        self.assertLess(entry.index('if (!selftest_active || !selftest_active()) return -EPERM;'),
                        entry.index('input_secure_domain(true, CIUKI_SECURITY_SECURE_ATTENTION);'))
        self.assertIn('!security_attention_armed()', entry)
        self.assertLess(entry.index('input_secure_domain(true, CIUKI_SECURITY_SECURE_ATTENTION);'),
                        entry.index('security_attention_physical();'))
        self.assertIn('int e = input_selftest_attention();', checks)
        self.assertNotIn('security_attention_physical', checks)
        self.assertNotIn('input_secure_domain', checks)


class Replay(unittest.TestCase):
    """The LOGON-aware templates and the power cut on synthetic captures."""
    def setUp(self):
        OUT.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=OUT); self.dir = Path(self.temp.name)
        self.sleep = patch.object(acceptance.time, 'sleep', lambda s: None); self.sleep.start()

    def tearDown(self):
        self.sleep.stop(); self.temp.cleanup()

    def session(self, c, on_key=None):
        acceptance.validate_case(c)
        s = acceptance.Session(c, '00000000', self.dir, ROOT, read_ppm=runner.read_ppm,
                               observe_screen=runner.observe_normal_boot_screen)
        return s, ta.FakeQMP(None, on_key, s.parser)

    LOGON_BOOT = ['L:SELECT_SOURCE=menu', 'Ciuki VMM F0 build f5242070d0b4 - CiukiOS',
                  '[supervisor] bootstrap: trusted logon pid=4', '[user 5] [logon] trusted logon starting safe=0',
                  '[user 5] [logon] automatic logon requested', '[user 5] [logon] authenticated uid=1000; starting session',
                  '[user 5] [logon] session 1 created uid=1000',
                  '[security] session 1 w32srv pid=7 pgid=7 uid=1000 gid=545 groups=544,545',
                  '[security] session 1 desktop pid=8 pgid=8 uid=1000 gid=545 groups=544,545',
                  '[user 5] [logon] session 1 running uid=1000 start_ms=900',
                  '[user 9] [desktop] display start mode=280 1024x768x32 saved=0 safe=0']

    def test_autologon_boot_template(self):
        c = ta.case(steps=[{'template': 'boot', 'prefix': 'b1', 'check': 'boot', 'settle': 0.1}],
                    assertions=[{'name': 'admin', 'check': 'boot', 'from': 'b1-loader', 'to': 'b1-display', 'min': 1,
                                 'pattern': r'^\[security\] session <<b1_session>> desktop pid=<<b1_desktop>> pgid=\d+ uid=1000 gid=545 groups=544,545$'}])
        s, qmp = self.session(c, on_key={'n': self.LOGON_BOOT})
        ta.drive(s, qmp, lines=[(0.0, 'L:CPU signature=00000673'), (0.1, 'SELECT_READY')])
        self.assertTrue(s.done, s.state)
        self.assertEqual((s.vars['b1_desktop'], s.vars['b1_session']), ('8', '1'))
        summary = s.finish({'outcome': 'pass'})
        self.assertEqual(summary['assertions'][0]['status'], 'pass')

    def test_logon_template_drives_welcome(self):
        welcome = '[user 10] [logon-ui] view mode=welcome view=list items=4 accounts=2 status=none'
        typed = '[user 10] [logon-ui] view mode=welcome view=typed items=5 accounts=2 status=none'
        auth = ['[user 5] [logon] authenticated uid=1001; starting session', '[user 5] [logon] session 2 created uid=1001',
                '[security] session 2 w32srv pid=21 pgid=21 uid=1001 gid=545 groups=545',
                '[security] session 2 desktop pid=22 pgid=22 uid=1001 gid=545 groups=545',
                '[user 5] [logon] session 2 running uid=1001 start_ms=1300',
                '[user 23] [desktop] display start mode=280 1024x768x32 saved=0 safe=0']
        c = ta.case(steps=[{'template': 'logon', 'prefix': 'bob', 'check': 'logon', 'user': 'Bob', 'password': 'Bob-pass-2026',
                            'settle': 0.1}])
        s, qmp = self.session(c, on_key={'ret#1': [typed], 'ret#3': auth})
        ta.drive(s, qmp, until=60, lines=[(0.0, welcome)])
        self.assertTrue(s.done, s.state)
        pressed = qmp.pressed()
        self.assertEqual(pressed[:3], ['up', 'up', 'ret'])
        self.assertEqual(pressed.count('ret'), 3)                    # list, name -> password, submit
        self.assertEqual(''.join(k for k in pressed[3:6]), 'shiftbo')  # B (shift+b), o ...
        self.assertEqual((s.vars['bob_uid'], s.vars['bob_session'], s.vars['bob_desktop'], s.vars['bob_groups']),
                         ('1001', '2', '22', '545'))
        log = json.dumps(s.state)
        self.assertNotIn('Bob-pass-2026', log)                       # typed text is counted, never logged
        # A refusal ends the step at once.
        s, qmp = self.session(c, on_key={'ret#1': [typed], 'ret#3': ['[user 5] [logon] authentication refused action=2 status=-13']})
        ta.drive(s, qmp, until=60, lines=[(0.0, welcome)])
        self.assertIn('authentication refused', s.failed or '')

    def test_safe_template_logs_on_as_the_blank_ciukiuser(self):
        steps = acceptance.expand_steps([{'template': 'boot-safe', 'prefix': 's1', 'check': 'safe'}])
        self.assertEqual([s['name'] for s in steps], ['s1-loader', 's1-kernel', 's1-welcome', 's1-typed', 's1-auth',
                                                      's1-desktop', 's1-display'])
        self.assertEqual(steps[0]['actions'], [{'keys': 'f5'}])
        self.assertEqual(steps[3]['actions'], [{'text': 'CiukiUser'}, {'keys': 'ret', 'pause': 0.5}, {'keys': 'ret'}])
        self.assertTrue(steps[-1]['wait'].endswith('safe=1$'))
        recovery = acceptance.expand_steps([{'template': 'boot-safe-logon', 'prefix': 's1', 'check': 'safe',
                                             'user': 'CiukiAdministrator', 'password': 'Admin-pass-2026'}])
        self.assertEqual(recovery[3]['actions'][2], {'text': 'Admin-pass-2026'})

    def test_template_and_power_cut_validation(self):
        bad = ({'template': 'logon', 'prefix': 'x', 'user': 'two words', 'password': 'Bob-pass-2026'},
               {'template': 'logon', 'prefix': 'x', 'user': 'Bob', 'password': 'short'},
               {'template': 'logon', 'prefix': 'x', 'user': 'Bob', 'password': 'Bob-pass-2026èx'},
               {'template': 'logon', 'prefix': 'x'},
               {'template': 'boot', 'prefix': 'x', 'user': 'Bob'})
        for step in bad:
            with self.subTest(step=step):
                with self.assertRaises(runner.res.Refusal):
                    acceptance.validate_case(ta.case(steps=[step]))
        with self.assertRaises(runner.res.Refusal):
            acceptance.validate_case(ta.case(steps=[{'name': 'cut', 'actions': [{'power_cut': True}]}]))
        with self.assertRaises(runner.res.Refusal):
            acceptance.validate_case(ta.case(resets=1, steps=[{'name': 'cut', 'actions': [{'power_cut': False}]}]))

    def test_power_cut_counts_one_reset_with_the_firmware_follow_up(self):
        steps = [{'name': 'cut', 'actions': [{'keys': 'ret'}, {'power_cut': True}]},
                 {'name': 'reset', 'wait_event': 'RESET', 'timeout': 5},
                 {'name': 'again', 'wait': '^SELECT_READY$', 'timeout': 5}]
        host = {'event': 'RESET', 'data': {'guest': False, 'reason': 'host-qmp-system-reset'},
                'timestamp': {'seconds': 100, 'microseconds': 0}}
        firmware = {'event': 'RESET', 'data': {'guest': True, 'reason': 'guest-reset'},
                    'timestamp': {'seconds': 100, 'microseconds': 400000}}
        s, qmp = self.session(ta.case(resets=1, steps=steps))
        original = qmp.command

        def command(name, arguments=None):
            if name == 'system_reset':
                qmp.events.extend([host, firmware])
                s.parser.feed(b'L:CPU signature=1\n'); s.parser.feed(b'SELECT_READY\n')
            return original(name, arguments)
        qmp.command = command
        ta.drive(s, qmp)
        self.assertTrue(s.done, (s.failed, s.state))
        self.assertEqual(s.resets, 1)
        self.assertEqual([o.get('after_power_cut', False) for o in s.observations], [False, True])
        self.assertIn(('system_reset', {}), qmp.commands)
        # Without a power cut the same host reset is unexpected.
        s, qmp = self.session(ta.case(resets=1, steps=[{'name': 'a', 'wait': '^never$', 'timeout': 5}]))
        qmp.events.append(host)
        ta.drive(s, qmp, until=1)
        self.assertIn('unexpected reset', s.failed)


class NormalBoot(unittest.TestCase):
    def test_logon_milestones_in_order(self):
        parser = runner.NormalBootParser('00000000')
        for line in ['L:CPU signature=00000673', 'L:SELECT_SOURCE=menu', 'Ciuki VMM F0 build f5242070d0b4 - CiukiOS',
                     '[supervisor] bootstrap: trusted logon pid=4',
                     '[security] session 1 w32srv pid=7 pgid=7 uid=1000 gid=545 groups=544,545',
                     '[security] session 1 desktop pid=8 pgid=8 uid=1000 gid=545 groups=544,545']:
            parser.feed(line.encode() + b'\n')
        # LOGON shows the composed desktop at "session N running" (run.py NORMAL_BOOT_SESSION).
        self.assertFalse(parser.desktop_seen)
        parser.feed(b'[user 35] [logon] session 1 running uid=1000 start_ms=1644\n')
        self.assertTrue(parser.desktop_seen)
        self.assertTrue(parser.check())
        self.assertEqual((parser.summary()['desktop_pid'], parser.summary()['desktop_pgid']), (8, 8))
        # A second session's desktop in the same normal boot is a repeated milestone.
        parser.feed(b'[security] session 2 desktop pid=30 pgid=30 uid=1001 gid=545 groups=545\n')
        self.assertEqual(parser.failure['name'], 'repeated_milestone')


class SectestModel(unittest.TestCase):
    """apps/systools/sectest_model.c on fake operations; its table against the policy sources."""
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)
        cls.binary = OUT / 'sectest_test'
        subprocess.run([*CLANG, '-I', str(ROOT / 'apps/systools'), '-I', str(ROOT / 'apps/accounts'), '-I', str(ROOT / 'sdk/private'),
                        '-I', str(ROOT / 'src/kernel/include'), str(ROOT / 'tests/host/security/sectest_test.c'),
                        str(ROOT / 'apps/systools/sectest_model.c'), str(ROOT / 'src/kernel/lib/pbkdf2.c'),
                        str(ROOT / 'apps/accounts/crypto.c'), str(ROOT / 'src/kernel/lib/sha256.c'), '-o', str(cls.binary)],
                       check=True)

    def test_model(self):
        out = subprocess.run([str(self.binary), 'model'], check=True, capture_output=True, text=True, timeout=60).stdout
        self.assertIn('sectest model: ', out)
        blocks = dict(re.findall(r'== ([a-z ]+)\n((?:(?!== ).*\n)*)', out))
        self.assertIn('sectest end role=standard checks=13 passed=13 failed=0 skipped=0', blocks['standard'])
        self.assertIn('sectest check=root-folder expect=allow result=-13 verdict=fail reason=refused', blocks['standard wrong'])
        self.assertIn('sectest check=programs expect=deny result=0 verdict=fail reason=allowed', blocks['standard wrong'])
        self.assertIn('sectest check=raw-disk expect=deny result=-5 verdict=fail reason=error', blocks['standard wrong'])
        self.assertIn('sectest check=backups expect=deny result=-2 verdict=skip reason=absent', blocks['standard wrong'])
        self.assertIn('verdict=fail', blocks['system'])
        self.assertIn('profile=- result=0 verdict=pass', blocks['no profile'])
        self.assertIn('sectest check=environment expect=allow result=-2 verdict=fail reason=error', blocks['no profile'])
        self.assertIn('sectest saver result=0', blocks['saver'])
        self.assertIn('check=machine-setting expect=none result=0 verdict=skip reason=not_attempted_for_admin', blocks['admin'])

    def test_table_follows_the_policy_sources(self):
        out = subprocess.run([str(self.binary), 'model'], check=True, capture_output=True, text=True, timeout=60).stdout
        table = {m[1]: dict(re.findall(r'(\w+)=(\w+)', m[2])) for m in re.finditer(r'^table ([a-z-]+) (.*)$', out, re.M)}
        policy = (ROOT / 'src/kernel/fs/security_policy.c').read_text()
        for path, kind in (('/Programs', 'POLICY_PROGRAM'), ('/System/Security', 'POLICY_SECRET'),
                           ('/System/SysBckup', 'POLICY_SECRET'), ('/Windows/Profiles', 'POLICY_PROFILES'),
                           ('/Documents/Desktop/Quake III Arena Demo', 'POLICY_PROGRAM')):
            self.assertIn('{"%s",%s}' % (path, kind), policy)
        # PROGRAM: administrators write; SECRET: nobody but SYSTEM; PROFILE: owner and administrators.
        self.assertIn('if (l->kind==POLICY_SECRET) return -EACCES;', policy)
        self.assertIn('if (l->kind==POLICY_PROFILE && !admin && c->uid!=l->uid) return -EACCES;', policy)
        self.assertIn('if (admin && l->kind!=POLICY_PROFILES) return 0;', policy)
        self.assertIn('if (l->kind==POLICY_CREATOR) return c->uid==l->uid?0:-EACCES;', policy)
        self.assertLess(policy.index('if (guest) return -EACCES;'), policy.index('if (l->kind==POLICY_ROOT) {'))
        self.assertEqual(table['programs'], {'admin': 'allow', 'standard': 'deny', 'guest': 'deny'})
        self.assertEqual(table['account-store'], {'admin': 'deny', 'standard': 'deny', 'guest': 'deny'})
        self.assertEqual(table['backups'], table['account-store'])
        self.assertEqual(table['other-profile'], {'admin': 'allow', 'standard': 'deny', 'guest': 'deny'})
        self.assertEqual(table['root-folder'], {'admin': 'allow', 'standard': 'allow', 'guest': 'deny'})
        self.assertEqual(table['foreign-folder'], {'admin': 'allow', 'standard': 'deny', 'guest': 'deny'})
        self.assertEqual(table['installer-shortcuts'], table['programs'])
        self.assertIn('STORE_ROOT = "/Programs/Installers"', (ROOT / 'scripts/q3a_demo.py').read_text())
        broker = (ROOT / 'apps/registry/broker.c').read_text()
        self.assertIn('if(mutation&&(root==RB_USERS||!admin(c)))return -EACCES;', broker)
        self.assertEqual(table['machine-registry'], table['programs'])
        sysvc = (ROOT / 'src/kernel/proc/syscalls_sysvc.c').read_text()
        self.assertLess(sysvc.index('if (!sysvc_admin(s)) return -EPERM;'), sysvc.index('char *text = buffer_alloc(r.length);'))
        self.assertEqual(table['machine-setting'], {'admin': 'none', 'standard': 'deny', 'guest': 'deny'})
        lease = (ROOT / 'src/kernel/fs/disklease.c').read_text()
        self.assertIn('if (!c->system && system_disk(s,disk)) return -EPERM;', lease)
        self.assertEqual(table['raw-disk'], table['account-store'])

    def test_kdf_equals_the_account_service_and_pbkdf2(self):
        out = subprocess.run([str(self.binary), 'kdf'], check=True, capture_output=True, text=True, timeout=120).stdout
        rows = re.findall(r'^kdf (\d) ([0-9a-f]{64}) ([0-9a-f]{64})$', out, re.M)
        self.assertEqual(len(rows), 4)
        for _, kernel, service in rows:
            self.assertEqual(kernel, service)
        salt = bytes([0x43, 0x69, 0x75, 0x6b, 0x69, 0x4f, 0x53, 0x2d, 0x73, 0x65, 0x6c, 0x66, 0x74, 0x65, 0x73, 0x74])
        self.assertEqual(rows[0][1], hashlib.pbkdf2_hmac('sha256', b'selftest-password', salt, 10000).hex())
        self.assertEqual(rows[1][1], hashlib.pbkdf2_hmac('sha256', b'', bytes(16), 10000).hex())
        accounts = (ROOT / 'apps/accounts/accounts.h').read_text()
        self.assertIn('#define ACCT_ITERATIONS 10000u', accounts)
        checks = (ROOT / 'src/kernel/core/selftest_checks.c').read_text()
        library = (ROOT / 'src/kernel/include/ciuki/pbkdf2.h').read_text()
        self.assertIn('#define CIUKI_ACCOUNT_KDF_ITERATIONS 10000u', library)
        self.assertIn('#include <ciuki/pbkdf2.h>', checks)
        self.assertIn('CIUKI_ACCOUNT_KDF_ITERATIONS, out)', checks)
        self.assertEqual(checks.count('{ "security.kdf"'), 1)
        self.assertIn('target_ms=1000', checks)


if __name__ == '__main__':
    unittest.main()
