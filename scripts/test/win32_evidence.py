"""F5-17A proposed win32-* serial records; deliberately separate from run.py.

No guest currently emits this draft protocol. Canned host records validate
framing/verdicts, not PE execution or original installer/game compatibility.
"""
import re
# One error class with run.py's collector: a malformed controller line keeps
# its bytes in serial.log (run.py catches evidence.EvidenceError around feed()).
from evidence import EvidenceError, EvidenceNotRun, evaluate_predicates


ENVELOPE = {'v', 'run', 'seq', 'probe', 'event'}
SEGMENTS = ('install', 'start', 'play', 'focus', 'exit', 'uninstall')
# All numeric values are integral: fps in thousandths, latency/frame time in us.
SCHEMAS = {
    'identity': ({'payload_sha256', 'scene'}, set()),
    'segment': ({'segment', 'op', 'guest_ms', 'icount'}, set()),
    'process': ({'segment', 'pid', 'op', 'guest_ms'}, {'code'}),
    'loader': ({'segment', 'pid', 'stage', 'status'}, set()),
    'mode': ({'segment', 'pid', 'width', 'height', 'bpp'}, set()),
    'frames': ({'segment', 'pid', 'duration_ms', 'game_frames', 'presents'}, set()),
    'fps': ({'segment', 'pid', 'avg_milli', 'median_milli', 'p05_milli', 'p95_us', 'samples'}, set()),
    'audio': ({'segment', 'pid', 'qualified', 'underruns', 'consumed_frames', 'non_silent_frames'}, set()),
    'audio_open': ({'segment', 'pid', 'stream_id', 'rate', 'channels'}, set()),
    'input': ({'segment', 'pid', 'input_id', 'latency_us', 'latency_frames'}, set()),
    'progress': ({'segment', 'pid', 'checkpoint', 'counter'}, set()),
    'focus': ({'segment', 'pid', 'round_trips', 'corrupt'}, set()),
    'ledger': ({'segment', 'when', 'kind', 'count', 'guest_ms'}, set()),
    'install': ({'segment', 'default_choices', 'system_writes', 'menu_entry', 'uninstall_registered'}, set()),
    'uninstall': ({'segment', 'key_removed', 'menu_removed'}, set()),
    'commit': ({'segment', 'pid', 'peak_bytes'}, set()),
    'shim': ({'segment', 'pid', 'name'}, set()),
    'stub': ({'segment', 'pid', 'name', 'handled'}, set()),
    'crash': ({'segment', 'pid', 'code'}, set()),
    # f5-17B installer/title inventory: one `installed` row per file, registry
    # key or Start-menu link the guest probe lists after the installer exits,
    # its path/key carried by ordered UTF-8 `name` chunks (run.py record_name
    # grammar, at most 40 bytes each) so long paths fit the 240-byte line.
    'installed': ({'segment', 'id', 'kind'}, {'size', 'sha256'}),
    'name': ({'segment', 'id', 'field', 'offset', 'bytes', 'hex'}, set()),
    # How the title was started in the `start` segment (its path in a name chunk).
    'launch': ({'segment', 'pid', 'kind', 'id'}, set()),
    # f5-15C installers tier (src/kernel/probes/installer_title_probe.inc):
    # one operator action of the guest UI driver about to be applied, after
    # which the guest pauses for the host screendump (`shot`); the window's
    # caption/class travel in `name` chunks (fields title/class).
    'ui': ({'segment', 'step', 'action', 'owner', 'hwnd', 'shot', 'id'}, set()),
    # The kernel's C: delta summary at the end of the install segment.
    'inventory': ({'segment', 'files', 'directories', 'links', 'changed', 'removed', 'temp', 'excluded', 'bytes'}, set()),
    # What the install added and the uninstall left behind.
    'residue': ({'segment', 'files', 'directories', 'links'}, set()),
    # Segment-less summaries of the observer.
    'ledger_summary': ({'clean', 'driver_failure'}, set()),
    'snapshot': ({'error'}, set()),
}
SEGMENTLESS = {'identity', 'ledger_summary', 'snapshot'}
UI_ACTIONS = ('step', 'default', 'restart', 'crash', 'gone', 'process')
NUMBERS = {'pid', 'guest_ms', 'icount', 'width', 'height', 'bpp', 'duration_ms', 'game_frames',
           'presents', 'avg_milli', 'median_milli', 'p05_milli', 'p95_us',
           'samples', 'underruns', 'consumed_frames', 'non_silent_frames', 'input_id', 'latency_us',
           'latency_frames', 'counter', 'round_trips', 'count', 'system_writes', 'peak_bytes',
           'id', 'offset', 'bytes', 'size', 'stream_id', 'rate', 'channels',
           'step', 'owner', 'hwnd', 'files', 'directories', 'links', 'changed', 'removed', 'temp', 'excluded',
           'driver_failure'}
INSTALLED_KINDS = ('file', 'directory', 'registry', 'link')
LAUNCH_KINDS = ('start-menu', 'fixture-tree', 'command')
KERNEL_KINDS = ('start', 'exit', 'crash', 'stub', 'refuse', 'load')
KERNEL_LINE_CAP = 256
# Which guest segments and independent host checks each runner tier needs.
# `gate` is the f5-17A whole-title verdict; `installers` and `titles` are the
# f5-17B runner tiers (docs/directives/f5-17b-acceptance-suite-runner-titles.md).
TIERS = {
    'gate': {'segments': ('install', 'start', 'play', 'exit'), 'install': 'required', 'play': True,
             'host': ('installed_files', 'menu_screen', 'play_screen', 'game_progress',
                      'audio_reference', 'desktop_screen', 'input_acknowledged'),
             'kernel': False, 'launch': False},
    'installers': {'segments': ('install',), 'install': 'required', 'play': False,
                   'host': ('installed_files', 'install_screens', 'desktop_screen'),
                   'kernel': True, 'launch': False},
    'titles': {'segments': ('start', 'play', 'exit'), 'install': 'optional', 'play': True,
               'host': ('menu_screen', 'play_screen', 'game_progress', 'audio_reference',
                        'desktop_screen', 'input_acknowledged'),
               'kernel': True, 'launch': True},
}
BOOLS = {'qualified', 'corrupt', 'default_choices', 'menu_entry', 'uninstall_registered',
         'key_removed', 'menu_removed', 'handled', 'shot', 'clean'}
LEDGERS = {'process', 'handle', 'section', 'stream', 'channel', 'commit'}


def require(condition, message):
    if not condition:
        raise EvidenceError(message)


class Parser:
    def __init__(self, run_id, probe, image_sha256, payload_sha256, scene):
        require(isinstance(run_id, str) and re.fullmatch('[0-9a-f]{8}', run_id), 'invalid run id')
        require(isinstance(probe, str) and re.fullmatch('win32-[a-z0-9]+(?:-[a-z0-9]+)*', probe), 'invalid win32 probe')
        for value in (image_sha256, payload_sha256):
            require(isinstance(value, str) and re.fullmatch('[0-9a-f]{64}', value), 'invalid identity digest')
        require(isinstance(scene, str) and re.fullmatch('[a-z0-9-]+', scene), 'invalid scene')
        self.run_id, self.probe = run_id, probe
        self.image_sha256, self.payload_sha256, self.scene = image_sha256, payload_sha256, scene
        self.records = []
        self.seq = 0
        self.started = False
        self.terminal = None
        self.outcome = None
        self.active = None
        self.segments = {}
        self.last_ms = self.last_icount = 0
        self.identity = False
        self.processes = set()
        self.loader_stages = {}
        self.not_run_subcases = []
        # Kernel `[win32]` serial lines (f5-16 diagnostics), kept per kind and bounded.
        self.kernel = {kind: [] for kind in KERNEL_KINDS}
        self.kernel_dropped = 0

    def feed(self, raw):
        line = raw.removesuffix(b'\n').removesuffix(b'\r')
        if b'CIUKI_TEST' not in line:
            return None
        require(line.startswith(b'CIUKI_TEST ') and len(line) <= 240, 'malformed prefix or record exceeds 240 bytes')
        try:
            words = line.decode('ascii').split(' ')[1:]
        except UnicodeDecodeError as error:
            raise EvidenceError('non-ASCII marker') from error
        record = {}
        for token in words:
            require(re.fullmatch(r'[a-z][a-z0-9_]*=[A-Za-z0-9_:.,/+\-]+', token), 'invalid key=value grammar')
            key, value = token.split('=', 1)
            require(key not in record, 'duplicate key')
            record[key] = value
        require(ENVELOPE <= record.keys(), 'missing envelope field')
        require(record['v'] == '1' and record['run'] == self.run_id and record['probe'] == self.probe, 'version/run/probe mismatch')
        require(re.fullmatch('[0-9]{6}', record['seq']), 'invalid sequence')
        seq = int(record['seq'])
        require(seq > self.seq and not self.terminal, 'nonincreasing sequence or output after terminal')
        require(len(self.records) < 16384, 'record count cap')
        event = record['event']
        fields = set(record)-ENVELOPE
        if event == 'BEGIN':
            require(not self.started and fields == {'image_sha256'}, 'invalid BEGIN')
            require(record['image_sha256'] == self.image_sha256, 'image hash mismatch')
            self.started = True
        elif event == 'NOT_RUN':
            require(not self.started and fields == {'reason'}, 'invalid NOT_RUN')
        else:
            require(self.started, 'missing BEGIN')
            if event == 'DATA':
                self._data(record, fields)
            elif event == 'ARM':
                require(fields == {'segment', 'action'} and record['segment'] == self.active, 'invalid segment ARM')
            elif event == 'END':
                require(fields in ({'status'}, {'status', 'reason'}) and record['status'] in ('PASS','FAIL','NOT_RUN'), 'invalid END')
                require(self.active is None or record['status'] != 'PASS', 'PASS with open segment')
                require(record['status'] != 'NOT_RUN' or 'reason' in record, 'NOT_RUN needs reason')
            elif event in ('ERROR', 'PANIC'):
                require(fields == {'reason'}, 'invalid error record')
            else:
                raise EvidenceError('unknown event')
        if event in ('END', 'ERROR', 'PANIC', 'NOT_RUN'):
            self.terminal = record
            self.outcome = ('not_run' if event == 'NOT_RUN' or record.get('status') == 'NOT_RUN'
                            else 'pass' if record.get('status') == 'PASS' else 'fail')
        self.seq = seq
        self.records.append(record)
        return record

    def _data(self, r, fields):
        case = r.get('case')
        require(case in SCHEMAS, 'unknown DATA case')
        required, optional = SCHEMAS[case]
        require(required | {'case'} <= fields and fields <= required | optional | {'case'}, 'DATA fields differ from schema')
        for key in fields & NUMBERS:
            require(re.fullmatch('[0-9]{1,20}', r[key]) and int(r[key]) <= 0xffffffffffffffff, 'invalid unsigned number: '+key)
        for key in fields & BOOLS:
            require(r[key] in ('0', '1'), 'invalid boolean: '+key)
        if case == 'identity':
            require(not self.identity and not self.segments and r['payload_sha256'] == self.payload_sha256 and r['scene'] == self.scene, 'payload/scene identity mismatch')
            self.identity = True
            return
        require(self.identity, 'missing payload/scene identity')
        if case in SEGMENTLESS:
            require(case != 'snapshot' or re.fullmatch('-?[0-9]{1,10}', r['error']), 'snapshot error code')
            return
        require(r['segment'] in SEGMENTS, 'unknown segment')
        if case == 'segment':
            self._segment(r)
            return
        require(r['segment'] == self.active, 'DATA outside its segment')
        if 'pid' in r:
            require(int(r['pid']) > 0, 'PID zero')
            if case != 'process':
                require(r['pid'] in self.processes, 'DATA for process that has not started')
        if case == 'process':
            require(r['op'] in ('start', 'exit'), 'process operation')
            require(r['guest_ms'].isdigit() and int(r['guest_ms']) >= self.segments[self.active]['begin'], 'process time before segment')
            if r['op'] == 'start':
                require(r['pid'] not in self.processes and 'code' not in r, 'duplicate process start')
                self.processes.add(r['pid'])
            else:
                require(r['pid'] in self.processes and re.fullmatch('-?[0-9]{1,10}', r.get('code','')), 'exit without process/code')
                self.processes.remove(r['pid'])
        elif case == 'loader':
            require(r['status'] in ('PASS','FAIL'), 'loader status')
            order = ('map','imports','tls','entry')
            stages = self.loader_stages.setdefault(r['pid'], [])
            require(len(stages) < len(order) and r['stage'] == order[len(stages)], 'loader stage ordering')
            stages.append(r['stage'])
        elif case == 'mode':
            require(0 < int(r['width']) <= 4096 and 0 < int(r['height']) <= 4096 and int(r['bpp']) in (8,16,24,32), 'invalid DirectDraw mode')
        elif case == 'frames':
            require(int(r['duration_ms']) > 0 and int(r['game_frames']) <= int(r['presents']), 'invalid frame counters')
        elif case == 'fps':
            require(int(r['p05_milli']) <= int(r['median_milli']), 'invalid fps percentiles')
        elif case == 'audio':
            require(int(r['non_silent_frames']) <= int(r['consumed_frames']), 'invalid audio consumption')
        elif case == 'audio_open':
            require(int(r['stream_id']) > 0 and 0 < int(r['rate']) <= 192000 and int(r['channels']) in (1, 2), 'invalid audio stream open')
        elif case == 'focus':
            require(int(r['round_trips']) <= 10, 'too many focus round trips')
        elif case == 'ledger':
            require(r['when'] in ('pre','post') and r['kind'] in LEDGERS, 'invalid resource ledger')
        elif case in ('install','uninstall'):
            require(r['segment'] == case, 'installer record in wrong segment')
        elif case == 'ui':
            require(r['action'] in UI_ACTIONS and r['segment'] in ('install', 'uninstall'), 'ui action/segment')
        elif case == 'inventory':
            require(r['segment'] == 'install', 'inventory summary in the install segment')
        elif case == 'residue':
            require(r['segment'] == 'uninstall', 'residue summary in the uninstall segment')
        elif case == 'installed':
            require(r['kind'] in INSTALLED_KINDS and r['segment'] in ('install', 'start'), 'installed inventory kind/segment')
            if r['kind'] == 'file':
                require({'size', 'sha256'} <= fields and re.fullmatch('[0-9a-f]{64}', r['sha256']), 'installed file needs size and SHA-256')
            else:
                require(not {'size', 'sha256'} & fields, 'only files carry size/SHA-256')
        elif case == 'name':
            require(re.fullmatch('(?:[0-9a-fA-F]{2}){1,40}', r['hex']) and int(r['bytes']) == len(r['hex'])//2, 'name chunk bytes')
            require(r['field'] in ('path', 'target', 'key', 'value', 'title', 'class'), 'name chunk field')
        elif case == 'launch':
            require(r['kind'] in LAUNCH_KINDS and r['segment'] == 'start', 'launch kind/segment')

    def _segment(self, r):
        name, op = r['segment'], r['op']
        ms, icount = int(r['guest_ms']), int(r['icount'])
        require(ms >= self.last_ms and icount >= self.last_icount, 'guest clock/icount moved backwards')
        if op == 'begin':
            require(self.active is None and name not in self.segments, 'overlapping/duplicate segment')
            if self.segments:
                require(SEGMENTS.index(name) > SEGMENTS.index(next(reversed(self.segments))), 'segment order')
            self.segments[name] = {'begin': ms, 'icount_begin': icount}
            self.active = name
        elif op == 'end':
            require(self.active == name, 'segment end without begin')
            self.segments[name].update(end=ms, icount_end=icount)
            self.active = None
        else:
            raise EvidenceError('segment operation')
        self.last_ms, self.last_icount = ms, icount

    def observe_line(self, raw):
        """Kernel `[win32] <kind> ...` serial lines (f5-16): bounded key=value
        observations beside the CIUKI_TEST records; never a record themselves."""
        line = raw.removesuffix(b'\n').removesuffix(b'\r')
        if not line.startswith(b'[win32] ') or len(line) > 400:
            return None
        words = line[8:].decode('utf-8', errors='replace').split(' ')
        kind = words[0]
        if kind not in self.kernel:
            return None
        observation = {'kind': kind}
        for token in words[1:]:
            if '=' in token:
                key, value = token.split('=', 1)
                if re.fullmatch('[a-z_]+', key) and key not in observation:
                    observation[key] = value
            elif token and 'name' not in observation:
                observation['name'] = token
        if kind == 'start' and 'exe' not in observation:
            return None  # priority/check notes, not a process start
        if len(self.kernel[kind]) >= KERNEL_LINE_CAP:
            self.kernel_dropped += 1
            return None
        self.kernel[kind].append(observation)
        return observation

    def name(self, identity, field):
        """Decode the ordered UTF-8 chunks of one `name` field (run.py record_name)."""
        chunks = [r for r in self.records if r.get('case') == 'name' and r['id'] == identity and r['field'] == field]
        require(chunks, 'missing name chunks for id '+str(identity))
        data = bytearray()
        for chunk in chunks:
            payload = bytes.fromhex(chunk['hex'])
            require(int(chunk['offset']) == len(data), 'name chunk offsets must be contiguous')
            data.extend(payload)
        try:
            return data.decode('utf-8')
        except UnicodeError as error:
            raise EvidenceError('name chunks are not UTF-8') from error

    def installed_inventory(self):
        """The guest-listed files/directories/registry keys/links, decoded and sorted."""
        rows = []
        seen = set()
        for r in self.records:
            if r.get('case') != 'installed':
                continue
            require(r['id'] not in seen, 'duplicate installed id '+r['id'])
            seen.add(r['id'])
            row = {'id': int(r['id']), 'kind': r['kind'], 'segment': r['segment'],
                   'path': self.name(r['id'], 'key' if r['kind'] == 'registry' else 'path')}
            if r['kind'] == 'file':
                row.update(size=int(r['size']), sha256=r['sha256'])
            if r['kind'] == 'link':
                row['target'] = self.name(r['id'], 'target')
            rows.append(row)
        return sorted(rows, key=lambda row: (row['kind'], row['path'].casefold(), row['id']))

    def kernel_exit_codes(self):
        """Exit codes of `[win32] exit` lines as Windows DWORDs (and signed view)."""
        codes = []
        for line in self.kernel['exit']:
            try:
                value = int(line.get('code', ''), 16)
            except ValueError:
                continue
            codes.append(value-2**32 if value >= 2**31 else value)
        return codes

    def check(self, expected):
        """Suite predicates over the win32 records (shared evidence engine)."""
        if self.outcome == 'not_run':
            # A guest-reported NOT_RUN is a missing prerequisite, never a failure.
            raise EvidenceNotRun('probe not_run: '+self.terminal.get('reason', 'unknown'))
        if not self.terminal or self.terminal['event'] != expected['terminal']:
            raise EvidenceError('missing or unexpected terminal event')
        if expected['terminal'] == 'END' and self.terminal['status'] != 'PASS':
            raise EvidenceError('guest reported FAIL')
        self.not_run_subcases = evaluate_predicates(self.records, expected)
        self.not_run_subcases.extend(expected.get('not_run_subcases', []))
        if self.not_run_subcases:
            raise EvidenceNotRun('required subcases not_run: '+str(self.not_run_subcases))
        return True

    def verdict(self, host, *, focus=False, uninstall=False, timeout=False, hardware=False,
                policy=None, exit_codes=(0,), tier='gate', installer_exit_codes=(0,)):
        """Combine guest observations with independent host/operator evidence.

        Missing evidence never passes. Hardware policy must be a lead-disposed
        explicit duration/fps/input threshold; this module cannot resolve Q7.
        `tier` selects the f5-17B runner tier (installers/titles) or the whole
        title gate (default); see TIERS.
        """
        require(tier in TIERS, 'unknown verdict tier')
        profile = TIERS[tier]
        if timeout:
            return {'outcome': 'fail', 'reason': 'host_timeout', 'tier': tier}
        if self.outcome == 'not_run':
            return {'outcome': 'not_run', 'reason': self.terminal['reason'], 'tier': tier}
        if self.outcome == 'fail':
            return {'outcome': 'fail', 'reason': 'guest_failure', 'tier': tier}
        if any(r.get('case') == 'crash' or r.get('case') == 'stub' and r['handled'] == '0'
               or r.get('case') == 'loader' and r['status'] == 'FAIL' for r in self.records):
            return {'outcome': 'fail', 'reason': 'crash_stub_or_loader_failure', 'tier': tier}
        if self.kernel['crash']:
            return {'outcome': 'fail', 'reason': 'kernel_crash_record', 'tier': tier}
        missing, failures = [], []
        if self.outcome != 'pass':
            missing.append('terminal_PASS')
        if not self.identity:
            missing.append('identity')
        install_required = profile['install'] == 'required' or (profile['install'] == 'optional' and 'install' in self.segments)
        needed = list(profile['segments'])+(['focus'] if focus else [])+(['uninstall'] if uninstall else [])
        if install_required and 'install' not in needed:
            needed.insert(0, 'install')
        for segment in needed:
            if 'end' not in self.segments.get(segment, {}):
                missing.append('segment_'+segment)
        host_needed = list(profile['host'])
        if install_required and 'installed_files' not in host_needed:
            host_needed.append('installed_files')
        if focus:
            host_needed.append('focus_screens')
        if uninstall:
            host_needed.append('uninstalled_files')
        for name in host_needed:
            if host.get(name) is False:
                failures.append(name)
            elif host.get(name) is not True:
                missing.append(name)

        def one(case, segment=None):
            rows = [r for r in self.records if r.get('case') == case and (segment is None or r.get('segment') == segment)]
            if len(rows) != 1:
                (failures if rows else missing).append(case+'_single_summary')
                return None
            return rows[0]

        if install_required:
            install = one('install','install')
            if install:
                if install['default_choices'] != '1' or install['system_writes'] != '0' or install['menu_entry'] != '1':
                    failures.append('installation')
                if (install['uninstall_registered'] == '1') != uninstall:
                    missing.append('uninstall_registration_policy')
            install_starts = [r for r in self.records if r.get('case') == 'process' and r['segment'] == 'install' and r['op'] == 'start']
            install_exits = [r for r in self.records if r.get('case') == 'process' and r['segment'] == 'install' and r['op'] == 'exit']
            if tier == 'installers' and (not install_starts or not install_exits):
                missing.append('installer_process_start_exit')
            if any(int(r['code']) not in installer_exit_codes for r in install_exits):
                failures.append('installer_exit_code')
        if profile['kernel']:
            if not self.kernel['start'] or not self.kernel['exit']:
                missing.append('kernel_start_exit_records')
            elif len(self.kernel_exit_codes()) != len(self.kernel['exit']) or any(code not in (installer_exit_codes if tier == 'installers' else exit_codes) for code in self.kernel_exit_codes()):
                failures.append('kernel_exit_code')
            if self.kernel['refuse']:
                failures.append('kernel_refusal')
            if self.kernel_dropped:
                failures.append('kernel_record_cap')
        if profile['launch']:
            launch = one('launch', 'start')
            if launch:
                try:
                    self.name(launch['id'], 'path')
                except EvidenceError:
                    missing.append('launch_path')
        if self.last_ms > 600000:
            failures.append('case_guest_time_ceiling')
        if uninstall:
            observation = one('uninstall','uninstall')
            if observation and (observation['key_removed'] != '1' or observation['menu_removed'] != '1'):
                failures.append('uninstall')
        if not profile['play']:
            if self.processes:
                failures.append('live_processes')
            return {'outcome': 'fail' if failures else 'not_run' if missing else 'pass', 'tier': tier,
                    'failures': sorted(set(failures)), 'missing': sorted(set(missing))}
        start = self.segments.get('start', {})
        if 'end' in start and start['end']-start['begin'] > 120000:
            failures.append('start_deadline')
        play = self.segments.get('play', {})
        duration = play.get('end', 0)-play.get('begin', 0)
        if 'end' in play and duration < 180000:
            failures.append('short_play')
        frames, fps, audio = one('frames','play'), one('fps','play'), one('audio','play')
        if tier == 'titles' and not any(r.get('case') == 'audio_open' and r['segment'] == 'play' for r in self.records):
            missing.append('audio_stream_open')
        if frames and (int(frames['duration_ms']) != duration or int(frames['game_frames']) == 0):
            failures.append('game_frames')
        if frames and fps:
            average = int(frames['game_frames'])*1000000//int(frames['duration_ms'])
            if abs(average-int(fps['avg_milli'])) > 1:
                failures.append('fps_counter_mismatch')
            if not 0 < int(fps['samples']) <= int(frames['game_frames']):
                failures.append('fps_sample_count')
        if audio:
            if audio['qualified'] != '1':
                missing.append('qualified_audio_backend')
            if int(audio['non_silent_frames']) == 0:
                failures.append('silent_audio')
        inputs = [r for r in self.records if r.get('case') == 'input' and r['segment'] == 'play']
        if not inputs:
            missing.append('input_latency')
        elif len({r['input_id'] for r in inputs}) != len(inputs):
            failures.append('duplicate_input_id')
        input_ids = host.get('input_ids')
        if not isinstance(input_ids, list) or not input_ids or any(type(i) is not int or i <= 0 for i in input_ids):
            missing.append('host_input_ids')
        elif inputs and (len(set(input_ids)) != len(input_ids) or {int(r['input_id']) for r in inputs} != set(input_ids)):
            failures.append('input_ack_identity')
        progress = [int(r['counter']) for r in self.records if r.get('case') == 'progress' and r['segment'] == 'play']
        if len(progress) < 2:
            missing.append('progress_checkpoints')
        elif any(b <= a for a, b in zip(progress, progress[1:])):
            failures.append('game_progress')
        one('mode','play')
        one('commit','play')
        stages = [r for r in self.records if r.get('case') == 'loader' and r['segment'] == 'start']
        if [r['stage'] for r in stages] != ['map','imports','tls','entry'] or len({r['pid'] for r in stages}) != 1:
            missing.append('loader_stages')
        elif any(r.get('segment') == 'play' and 'pid' in r and r['pid'] != stages[0]['pid'] for r in self.records):
            failures.append('game_process_identity')
        if focus:
            observation = one('focus','focus')
            if observation and (observation['round_trips'] != '10' or observation['corrupt'] != '0'):
                failures.append('focus')
        exits = [r for r in self.records if r.get('case') == 'process' and r['segment'] == 'exit' and r['op'] == 'exit']
        if len(exits) != 1:
            missing.append('process_exit')
        elif int(exits[0]['code']) not in exit_codes:
            failures.append('exit_code')
        if self.processes:
            failures.append('live_processes')
        for kind in LEDGERS:
            before = [r for r in self.records if r.get('case') == 'ledger' and r['when'] == 'pre' and r['kind'] == kind and r['segment'] == 'start']
            after = [r for r in self.records if r.get('case') == 'ledger' and r['when'] == 'post' and r['kind'] == kind and r['segment'] == 'exit']
            if len(before) != 1 or len(after) != 1:
                (failures if len(before) > 1 or len(after) > 1 else missing).append('ledger_'+kind)
            elif before[0]['count'] != after[0]['count'] or exits and int(after[0]['guest_ms'])-int(exits[0]['guest_ms']) not in range(2001):
                failures.append('ledger_'+kind)
        if hardware:
            if policy is None:
                missing.append('lead_hardware_policy')
            else:
                require(set(policy) == {'play_ms','min_fps_milli','max_underruns','max_latency_us','max_latency_frames'}, 'hardware policy fields')
                require(all(type(v) is int and v >= 0 for v in policy.values()) and policy['play_ms'] >= 180000, 'hardware policy values')
                if duration < policy['play_ms'] or fps and int(fps['avg_milli']) < policy['min_fps_milli']:
                    failures.append('hardware_playable')
                if audio and int(audio['underruns']) > policy['max_underruns']:
                    failures.append('audio_underruns')
                if any(int(r['latency_us']) > policy['max_latency_us'] or int(r['latency_frames']) > policy['max_latency_frames'] for r in inputs):
                    failures.append('input_latency')
        return {'outcome': 'fail' if failures else 'not_run' if missing else 'pass', 'tier': tier,
                'failures': sorted(set(failures)), 'missing': sorted(set(missing))}
