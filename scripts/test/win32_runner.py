"""f5-17B runner tiers for Win32 titles: `installers` and `titles` cases.

run.py owns QEMU, the overlay, the scope and the serial collector; this module
validates a tier case, then drives one boot's Win32 session:

* the fw_cfg selector `f5:win32-<workload> run=<id>` (win32_session.selector);
* the win32_evidence.Parser bound to the image SHA-256, the fixture payload
  identity and the scene, plus the kernel `[win32]` lines;
* the screen oracle: named region checks (pinned SHA-256 or bounded tolerance)
  polled through QMP screendumps after a trigger record or when the input
  script waits for them, never after a fixed sleep;
* the input player (win32_session.InputPlayer) paced from segment ARMs and
  screen matches;
* the verdict and the evidence summary (`result['win32']`) tying every record
  to the image hash, the title edition and the scene.

Nothing here claims guest support: the probes `win32-<id>` are not registered
by the kernel yet (docs/directives/f5-17b-acceptance-suite-runner-titles.md).
"""
import hashlib
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import probe_tables  # noqa: E402
import resources as res  # noqa: E402
from evidence import EvidenceError, EvidenceNotRun  # noqa: E402
import win32_evidence  # noqa: E402
import win32_fixtures  # noqa: E402
import win32_session  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
KINDS = {'win32-installer': 'installers', 'win32-title': 'titles'}
PROBE_LIMIT = probe_tables.PROBE_LIMIT
SCRIPTS = 'tests/win32/scripts'
FRAMES = 'tests/win32/frames'
CASE_KEYS = {'id', 'kind', 'probe', 'profile', 'selector', 'timeout', 'expected', 'attempt', 'boot_kind',
             'fixtures', 'win32', 'integration_pending', 'usb', 'i8042', 'device_exceptions',
             'audio_expectations', '_smoke', '_run_id'}
WIN32_KEYS = {'workload', 'scene', 'script', 'screens', 'checkpoints', 'focus', 'uninstall', 'exit_codes',
              'installer_exit_codes', 'launch', 'progress', 'edition'}
SCREEN_KEYS = {'region', 'sha256', 'mode', 'channel_delta', 'max_bad_pixels', 'reference', 'role',
               'description', 'timeout_ms', 'poll_ms'}
ROLES = {'install': 'install_screens', 'menu': 'menu_screen', 'play': 'play_screen',
         'desktop': 'desktop_screen', 'focus': 'focus_screens'}
CHECKPOINT_KEYS = {'name', 'after', 'screen', 'timeout_ms', 'poll_ms', 'required'}
LAUNCH_KEYS = {'kind', 'path', 'entry'}
DEFAULT_TIMEOUT_MS = 30000
DEFAULT_POLL_MS = 500
MIN_POLL_MS = 250


def selector(request, source='fw_cfg', validated_fw_cfg=False):
    """run.py's selector dictionary for a Win32 title request."""
    selected = win32_session.selector(request, source=source, validated_fw_cfg=validated_fw_cfg)
    return {**selected, 'platform': None, 'safe': False}


def refuse(condition, message):
    if not condition:
        raise res.Refusal(message)


def name_ok(value):
    return win32_session.screen_name(value)


def load_script(case, root=ROOT):
    path = (Path(root) / case['win32']['script']).resolve()
    refuse(path.is_relative_to((Path(root) / SCRIPTS).resolve()) and path.is_file(),
           'win32 script must be a file under ' + SCRIPTS)
    try:
        script = win32_session.validate_script(json.loads(path.read_text()))
    except (ValueError, OSError) as error:
        raise res.Refusal('invalid win32 input script ' + case['win32']['script'] + ': ' + str(error)) from error
    return path, script


def validate_case(case, root=ROOT):
    """Structural checks before any boot; returns the tier name."""
    kind = case.get('kind')
    refuse(kind in KINDS, 'unknown case kind: ' + str(kind))
    tier = KINDS[kind]
    extra = sorted(set(case) - CASE_KEYS)
    refuse(not extra, 'win32 case refuses keys: ' + ', '.join(extra))
    win32 = case.get('win32')
    refuse(isinstance(win32, dict), 'win32 case needs a win32 block')
    extra = sorted(set(win32) - WIN32_KEYS)
    refuse(not extra, 'win32 block refuses keys: ' + ', '.join(extra))
    workload, scene = win32.get('workload'), win32.get('scene')
    refuse(isinstance(workload, str) and re.fullmatch('[a-z0-9]+(?:-[a-z0-9]+)*', workload), 'win32 workload id')
    refuse(isinstance(scene, str) and name_ok(scene), 'win32 scene id')
    refuse(case.get('probe') == 'win32-' + workload, 'win32 probe must be win32-<workload>')
    refuse(case.get('selector') == 'f5:' + case['probe'] + ' run={run_id}', 'win32 selector must be f5:<probe> run={run_id}')
    refuse(type(case.get('timeout')) is int and 1 <= case['timeout'] <= 600, 'win32 case timeout must be 1..600 s')
    refuse(case.get('boot_kind', 'cold') == 'cold', 'win32 cases are cold boots')
    expected = case.get('expected')
    refuse(isinstance(expected, dict) and expected.get('terminal') == 'END', 'win32 expected.terminal must be END')
    fixtures = case.get('fixtures')
    refuse(isinstance(fixtures, list) and 1 <= len(fixtures) <= 2, 'win32 case declares one or two workload fixtures')
    for fixture in fixtures:
        try:
            row = win32_fixtures.validate(fixture, root)
        except ValueError as error:
            raise res.Refusal(str(error)) from error
        refuse(fixture['workload'] == workload, 'fixture workload differs from the case workload')
    # The first fixture's payload identity binds the guest identity record; a
    # second one is the install/play disc (at most one disk and one CD image).
    refuse(sum(win32_fixtures.is_disk(f) for f in fixtures) <= 1 and sum(win32_fixtures.is_optical(f) for f in fixtures) <= 1,
           'win32 case fixtures: at most one disk and one CD image')
    refuse(row['scene']['id'] == scene, 'scene differs from the workload manifest')
    edition = win32.get('edition')
    refuse(edition is None or isinstance(edition, str) and 0 < len(edition) <= 80, 'edition is a short label')
    refuse(edition is None or row['version'] is None or edition == row['version'], 'edition differs from the workload manifest version')
    for key in ('focus', 'uninstall'):
        refuse(type(win32.get(key, False)) is bool, key + ' must be boolean')
    for key in ('exit_codes', 'installer_exit_codes'):
        codes = win32.get(key, [0])
        refuse(isinstance(codes, list) and codes and all(type(c) is int for c in codes), key + ' must list integers')
    screens = win32.get('screens', {})
    refuse(isinstance(screens, dict), 'screens must map names to region checks')
    for name, check in screens.items():
        refuse(name_ok(name) and isinstance(check, dict), 'screen check name/shape: ' + str(name))
        extra = sorted(set(check) - SCREEN_KEYS)
        refuse(not extra, 'screen check refuses keys: ' + ', '.join(extra))
        region = check.get('region')
        refuse(isinstance(region, list) and len(region) == 4 and all(type(v) is int and v >= 0 for v in region)
               and region[2] > 0 and region[3] > 0, 'screen region [x,y,w,h]: ' + name)
        pin = check.get('sha256')
        refuse(pin is None or isinstance(pin, str) and re.fullmatch('[0-9a-f]{64}', pin), 'screen pin: ' + name)
        mode = check.get('mode', 'sha256')
        refuse(mode in ('sha256', 'tolerance'), 'screen mode: ' + name)
        if mode == 'tolerance':
            delta, bad = check.get('channel_delta'), check.get('max_bad_pixels')
            refuse(type(delta) is int and 0 <= delta <= 255 and type(bad) is int and 0 <= bad < region[2] * region[3],
                   'screen tolerance bounds: ' + name)
            reference = check.get('reference')
            refuse(isinstance(reference, str) and reference.startswith(FRAMES + '/'), 'tolerance needs a reference frame under ' + FRAMES)
        refuse(check.get('role', 'play') in ROLES, 'screen role: ' + name)
        for key, low in (('timeout_ms', 1000), ('poll_ms', MIN_POLL_MS)):
            value = check.get(key)
            refuse(value is None or type(value) is int and low <= value <= 600000, 'screen %s: %s' % (key, name))
    checkpoints = win32.get('checkpoints', [])
    refuse(isinstance(checkpoints, list), 'checkpoints must be a list')
    names = set()
    for checkpoint in checkpoints:
        refuse(isinstance(checkpoint, dict) and not set(checkpoint) - CHECKPOINT_KEYS, 'checkpoint keys')
        refuse(name_ok(checkpoint.get('name')) and checkpoint['name'] not in names, 'checkpoint name')
        names.add(checkpoint['name'])
        refuse(checkpoint.get('screen') in screens, 'checkpoint screen undeclared: ' + str(checkpoint.get('screen')))
        after = checkpoint.get('after')
        refuse(isinstance(after, dict) and after and all(isinstance(k, str) and isinstance(v, (str, int)) for k, v in after.items()),
               'checkpoint after must match record fields')
        refuse(type(checkpoint.get('required', True)) is bool, 'checkpoint required flag')
        for key, low in (('timeout_ms', 1000), ('poll_ms', MIN_POLL_MS)):
            value = checkpoint.get(key)
            refuse(value is None or type(value) is int and low <= value <= 600000, 'checkpoint ' + key)
    refuse(len({c['screen'] for c in checkpoints}) == len(checkpoints), 'checkpoints need distinct screen names')
    progress = win32.get('progress')
    if progress is not None:
        refuse(isinstance(progress, dict) and set(progress) <= {'checkpoints', 'must_differ'}, 'progress keys')
        refs = progress.get('checkpoints')
        refuse(isinstance(refs, list) and len(refs) >= 2 and all(r in names for r in refs) and len(set(refs)) == len(refs),
               'progress needs at least two distinct checkpoints')
        refuse(progress.get('must_differ', True) is True, 'progress frames must differ (repeated presents are not progress)')
    launch = win32.get('launch')
    if tier == 'titles':
        refuse(isinstance(launch, dict) and set(launch) <= LAUNCH_KEYS and launch.get('kind') in win32_evidence.LAUNCH_KINDS,
               'titles tier declares how the title is launched')
    path, script = load_script(case, root)
    refuse(script['workload'] == workload and script['scene'] == scene, 'script identity differs from the case')
    refuse(script['timeout_ms'] <= case['timeout'] * 1000, 'script timeout exceeds the case timeout')
    undeclared = sorted(win32_session.screen_names(script) - set(screens))
    refuse(not undeclared, 'script waits for undeclared screens: ' + ', '.join(undeclared))
    segments = {s['name'] for s in script['segments']}
    if tier == 'installers':
        refuse('install' in segments, 'installers tier script needs an install segment')
    else:
        refuse({'start', 'play', 'exit'} <= segments, 'titles tier script needs start, play and exit segments')
        refuse(('focus' in segments) == win32.get('focus', False), 'focus segment must match the focus flag')
    return tier


class Blocked(EvidenceNotRun):
    """The case cannot run yet: the runner reports not_run before any QEMU launch.

    `blockers` lists, in a stable order, every reason found (never just the
    first): pins not yet qualified by a reference run, a probe name the kernel
    selector cannot hold or does not register.
    """
    def __init__(self, blockers):
        self.blockers = blockers
        super().__init__('; '.join('%s: %s' % (b['kind'], b['detail']) for b in blockers))

    @property
    def kind(self):
        return self.blockers[0]['kind']


def kernel_probes(root=ROOT):
    """Names the kernel selector registers for phase 5 (the only fw_cfg table)."""
    return probe_tables.kernel_names(root).get(5, set())


def blockers_of(case, screens, needed, root=ROOT):
    """Pre-launch blockers of a validated case; empty when it can be run."""
    found = []
    pending = sorted(name for name in needed if screens[name].get('sha256') is None)
    if pending:
        found.append({'kind': 'screen_pins_pending', 'detail': ', '.join(pending)})
    probe = case['probe']
    if len(probe) > PROBE_LIMIT:
        found.append({'kind': 'probe_name_exceeds_kernel_buffer',
                      'detail': '%s is %d bytes, the kernel selector keeps %d' % (probe, len(probe), PROBE_LIMIT)})
    elif probe not in kernel_probes(root):
        found.append({'kind': 'probe_not_registered',
                      'detail': 'f5:%s is not in selector.c f5_names (owner: f5-15B probe registration)' % probe})
    return found


def compare_inventory(row, inventory):
    """Guest-listed files versus the manifest's verified installed inventory.

    None when the manifest inventory is still unknown (the gate stays not_run);
    otherwise a dictionary with `match` and the differences.
    """
    expected = row['installed_files']
    if expected['status'] != 'verified':
        return None
    files = [r for r in inventory if r['kind'] == 'file']
    def normalized(path):
        return path.replace('\\', '/').casefold()
    by_name = {}
    for r in files:
        by_name.setdefault(normalized(r['path']).rsplit('/', 1)[-1], []).append(r)
    missing, mismatched, used = [], [], set()
    for entry in expected['entries']:
        name = normalized(entry['name'])
        candidates = [r for r in files if normalized(r['path']) == name] if '/' in name else by_name.get(name, [])
        if not candidates:
            missing.append(entry['name'])
            continue
        if len(candidates) != 1 or not any(c['size'] == entry['size'] and (entry.get('sha256') in (None, c['sha256'])) for c in candidates):
            mismatched.append(entry['name'])
        used.update(c['path'] for c in candidates)
    unexpected = sorted(r['path'] for r in files if r['path'] not in used)
    return {'match': not missing and not mismatched and not unexpected, 'expected': len(expected['entries']), 'listed': len(files),
            'missing': missing, 'mismatched': mismatched, 'unexpected': unexpected}


def needed_screens(case, script):
    """Screen checks whose reference pins the verdict depends on."""
    win32 = case['win32']
    required = {c['screen'] for c in win32.get('checkpoints', []) if c.get('required', True)}
    return set(win32_session.screen_names(script)) | required


def precheck(case, root=ROOT):
    """Cheap pre-launch gate, run before any fixture image is mastered.

    Raises Blocked (reported as not_run, no QEMU, no multi-hundred-MiB image)
    when a required pin is still null or the probe cannot be selected.
    """
    path, script = load_script(case, root)
    blockers = blockers_of(case, case['win32'].get('screens', {}), needed_screens(case, script), root)
    if script.get('status') != 'reference-qualified':
        blockers.append({'kind': 'input_reference_pending', 'detail': str(path.relative_to(root))})
    if KINDS[case['kind']] == 'titles' and not case['win32'].get('launch', {}).get('path'):
        blockers.append({'kind': 'launch_path_pending', 'detail': 'reference-installed executable path is unknown'})
    if not case['win32'].get('edition') and not win32_fixtures.workload_row(case['win32']['workload'], root)['version']:
        blockers.append({'kind': 'edition_pending', 'detail': 'original edition has not been qualified'})
    if blockers:
        raise Blocked(blockers)


class Session:
    def __init__(self, case, run_id, image_sha256, fixtures, directory, root=ROOT, read_frame=None, started_at_ms=0):
        self.case, self.win32 = case, case['win32']
        self.tier = KINDS[case['kind']]
        self.directory, self.root = Path(directory), Path(root)
        self.read_frame = read_frame
        self.row = win32_fixtures.workload_row(self.win32['workload'], root)
        manifests = [f for f in fixtures.get('manifest', []) if f.get('generator') == win32_fixtures.GENERATOR]
        if not manifests or not all(m.get('payload_sha256') for m in manifests):
            raise EvidenceError('win32 case lacks its workload fixture manifest')
        manifest = manifests[0]
        self.fixture, self.fixtures = manifest, manifests
        self.path, self.script = load_script(case, root)
        self.script_sha256 = hashlib.sha256(self.path.read_bytes()).hexdigest()
        self.screens = self.win32.get('screens', {})
        self.checkpoints = {c['name']: dict(c) for c in self.win32.get('checkpoints', [])}
        precheck(case, root)
        self.request = 'f5:%s run=%s' % (case['probe'], run_id)
        self.parser = win32_evidence.Parser(run_id, case['probe'], image_sha256, manifest['payload_sha256'], self.win32['scene'])
        self.player = win32_session.InputPlayer(self.script, self.request, started_at_ms=started_at_ms,
                                                validated_fw_cfg=True, screen_time=self.screen_time)
        self.waits = {}           # screen name -> wait state
        self.triggered = set()    # checkpoints whose trigger record was seen
        self.frames = []          # every screendump taken: name, attempt, sha256, match
        self.now = started_at_ms
        self.references = {}
        self.host_records = []

    # --- screen oracle -------------------------------------------------------
    def wait_for(self, name, now_ms, source):
        if name in self.waits:
            return self.waits[name]
        check = self.screens[name]
        timeout = check.get('timeout_ms', DEFAULT_TIMEOUT_MS)
        poll = check.get('poll_ms', DEFAULT_POLL_MS)
        for checkpoint in self.checkpoints.values():
            if checkpoint['screen'] == name:
                timeout = checkpoint.get('timeout_ms', timeout)
                poll = checkpoint.get('poll_ms', poll)
        wait = {'name': name, 'source': source, 'started_ms': now_ms, 'timeout_ms': timeout, 'poll_ms': max(poll, MIN_POLL_MS),
                'last_poll_ms': None, 'matched_ms': None, 'attempts': 0, 'failed': False, 'frame_sha256': None, 'region_sha256': None}
        self.waits[name] = wait
        return wait

    def screen_time(self, name):
        wait = self.wait_for(name, self.now, 'script')
        return wait['matched_ms']

    def reference(self, check):
        path = check.get('reference')
        if path is None:
            return None
        if path not in self.references:
            full = (self.root / path).resolve()
            if not full.is_relative_to((self.root / FRAMES).resolve()) or not full.is_file():
                raise EvidenceError('reference frame missing: ' + path)
            self.references[path] = self.read_frame(full)
        return self.references[path]

    def poll(self, wait, now_ms, qmp):
        name = wait['name']
        check = {'mode': 'sha256', **self.screens[name]}
        shot = self.directory / ('screen-%s-%d.ppm' % (name, wait['attempts']))
        qmp.command('screendump', {'filename': str(shot)})
        wait['attempts'] += 1
        wait['last_poll_ms'] = now_ms
        try:
            frame = self.read_frame(shot)
            outcome = win32_session.screen_check(frame, check, self.reference(check))
            digest = hashlib.sha256(frame[2]).hexdigest()
        except ValueError as error:
            shot.unlink(missing_ok=True)
            raise EvidenceError('screen check %s: %s' % (name, error)) from error
        record = {'name': name, 'attempt': wait['attempts'] - 1, 'host_ms': now_ms, 'frame_sha256': digest,
                  'region_sha256': outcome['sha256'], 'match': outcome['match'], 'bad_pixels': outcome.get('bad_pixels')}
        self.frames.append(record)
        if outcome['match']:
            wait['matched_ms'] = now_ms
            wait['frame_sha256'] = digest
            wait['region_sha256'] = outcome['sha256']
            shot.replace(self.directory / ('screen-%s.ppm' % name))
            return True
        shot.unlink(missing_ok=True)
        return False

    def observe(self, line, record, now_ms):
        self.now = now_ms
        if record is None:
            self.parser.observe_line(line)
            return
        self.host_records.append({'seq': record['seq'], 'segment': record.get('segment'), 'host_ms': now_ms})
        if self.parser.outcome in ('not_run', 'fail'):
            return
        self.player.observe(record, now_ms)
        for name, checkpoint in self.checkpoints.items():
            if name in self.triggered:
                continue
            if all(record.get(k) == str(v) for k, v in checkpoint['after'].items()):
                self.triggered.add(name)
                self.wait_for(checkpoint['screen'], now_ms, 'checkpoint:' + name)

    def tick(self, now_ms, qmp):
        self.now = now_ms
        if self.parser.outcome in ('not_run', 'fail'):
            return None
        for wait in list(self.waits.values()):
            if wait['matched_ms'] is not None or wait['failed']:
                continue
            if wait['last_poll_ms'] is None or now_ms - wait['last_poll_ms'] >= wait['poll_ms']:
                if self.poll(wait, now_ms, qmp):
                    continue
                if now_ms - wait['started_ms'] >= wait['timeout_ms']:
                    required = any(c['screen'] == wait['name'] and c.get('required', True) for c in self.checkpoints.values()) \
                        or wait['source'] == 'script'
                    wait['failed'] = True
                    if required:
                        raise EvidenceError('wrong scene: screen %s did not match within %d ms (%d screendumps)'
                                            % (wait['name'], wait['timeout_ms'], wait['attempts']))
        receipt = self.player.tick(now_ms, qmp)
        return receipt

    @property
    def waits_open(self):
        """A triggered screen check is still being polled (not matched, not failed)."""
        return any(w['matched_ms'] is None and not w['failed'] for w in self.waits.values())

    @property
    def complete(self):
        return self.player.complete and not self.waits_open

    def pending(self):
        return {'script': self.player.pending(),
                'screens': [w['name'] for w in self.waits.values() if w['matched_ms'] is None and not w['failed']],
                'checkpoints_untriggered': sorted(set(self.checkpoints) - self.triggered)}

    # --- verdict -------------------------------------------------------------
    def host_checks(self, result):
        host = {}
        roles = {}
        required = needed_screens(self.case, self.script)
        for name, check in self.screens.items():
            wait = self.waits.get(name)
            if wait is None:
                if name not in required:
                    continue
                wait = {'matched_ms': None, 'failed': False}
            roles.setdefault(check.get('role', 'play'), []).append(wait)
        for role, key in ROLES.items():
            waits = roles.get(role)
            if not waits:
                continue
            host[key] = False if any(w['failed'] for w in waits) else True if all(w['matched_ms'] is not None for w in waits) else None
        progress = self.win32.get('progress')
        if progress:
            digests = []
            for name in progress['checkpoints']:
                wait = self.waits.get(self.checkpoints[name]['screen'])
                digests.append(wait['region_sha256'] if wait and wait['matched_ms'] is not None else None)
            if all(digests):
                regions = [self.screens[self.checkpoints[n]['screen']]['region'] for n in progress['checkpoints']]
                host['game_progress'] = all(r == regions[0] for r in regions) and all(a != b for a, b in zip(digests, digests[1:]))
            elif any(w['failed'] for w in self.waits.values()):
                host['game_progress'] = False
        receipts = self.player.observed
        if self.player.complete:
            host['input_acknowledged'] = all(r['qmp_acknowledged'] for r in receipts)
        elif receipts:
            host['input_acknowledged'] = False
        host['input_ids'] = win32_session.latency_input_ids(self.script)
        try:
            inventory = self.parser.installed_inventory()
        except EvidenceError as error:
            host['installed_files'] = False
            host['installed_files_error'] = str(error)
            inventory = []
        else:
            comparison = compare_inventory(self.row, inventory)
            if comparison is not None:
                host['installed_files'] = comparison['match']
                host['installed_files_comparison'] = comparison
        audio = result.get('audio')
        if isinstance(audio, dict):
            host['audio_reference'] = not audio.get('failures')
        return host, inventory

    def finish(self, result):
        host, inventory = self.host_checks(result)
        verdict = self.parser.verdict(host, focus=self.win32.get('focus', False), uninstall=self.win32.get('uninstall', False),
                                      timeout=result['timeout']['occurred'], tier=self.tier,
                                      exit_codes=tuple(self.win32.get('exit_codes', [0])),
                                      installer_exit_codes=tuple(self.win32.get('installer_exit_codes', [0])))
        untriggered = sorted(name for name, c in self.checkpoints.items() if c.get('required', True) and name not in self.triggered)
        if untriggered and verdict['outcome'] != 'fail':
            verdict['outcome'] = 'not_run'
            verdict.setdefault('missing', []).extend('checkpoint_' + name for name in untriggered)
        if self.tier == 'titles':
            declared = self.win32['launch']
            launches = [r for r in self.parser.records if r.get('case') == 'launch']
            for launch in launches:
                try:
                    actual = self.parser.name(launch['id'], 'path').replace('\\', '/').casefold()
                except EvidenceError:
                    continue  # parser.verdict already reports missing launch_path
                expected = declared['path'].replace('\\', '/').casefold()
                if launch['kind'] != declared['kind'] or actual != expected:
                    verdict['outcome'] = 'fail'
                    verdict.setdefault('failures', []).append('launch_identity')
        result['win32'] = {
            'tier': self.tier, 'workload': self.row['id'], 'scene': self.win32['scene'],
            'title': {'name': self.row['title'], 'version': self.row['version'], 'year': self.row['year'],
                      'publisher': self.row['publisher'], 'corpus': self.row['corpus'], 'licence_class': self.row['licence']['class']},
            'image_sha256': self.parser.image_sha256, 'payload_sha256': self.parser.payload_sha256,
            'edition': self.win32.get('edition') or self.row['version'],
            'fixtures': [{'path': f.get('path'), 'sha256': f.get('sha256'), 'sha256_after': f.get('sha256_after'),
                          'media': f.get('media'), 'source': f.get('source'), 'payload_sha256': f.get('payload_sha256'),
                          'file_count': f.get('file_count'), 'total_bytes': f.get('total_bytes')} for f in self.fixtures],
            'script': {'path': str(self.path.relative_to(self.root)), 'sha256': self.script_sha256,
                       'timeout_ms': self.script['timeout_ms'], 'segments': [s['name'] for s in self.script['segments']]},
            'selector': self.request, 'launch': self.win32.get('launch'),
            'segments': self.parser.segments, 'record_host_times': self.host_records, 'kernel': self.parser.kernel,
            'screens': [{**{k: v for k, v in w.items() if k != 'last_poll_ms'}, 'role': self.screens[w['name']].get('role', 'play'),
                         'region': self.screens[w['name']]['region'], 'pin': self.screens[w['name']].get('sha256')}
                        for w in self.waits.values()],
            'frames': self.frames, 'checkpoints_triggered': sorted(self.triggered),
            'input': {'receipts': self.player.observed, 'latency_ids': host.get('input_ids'), 'complete': self.player.complete},
            'installed_inventory': inventory, 'host_checks': host, 'verdict': verdict,
            'clock_domains': {'host': 'monotonic ms since QEMU launch', 'guest_ms': 'guest elapsed ms from the records',
                              'icount': 'TCG instruction count from the records; never converted to host time'}}
        if verdict['outcome'] != 'pass' and result['outcome'] == 'pass':
            result['outcome'] = verdict['outcome']
            detail = verdict.get('reason') or ('failures=%s missing=%s' % (verdict.get('failures'), verdict.get('missing')))
            result['reason'] = 'win32 %s verdict %s: %s' % (self.tier, verdict['outcome'], detail)
        return verdict
