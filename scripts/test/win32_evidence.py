"""F5-17A proposed win32-* serial records; deliberately separate from run.py.

No guest currently emits this draft protocol. Canned host records validate
framing/verdicts, not PE execution or original installer/game compatibility.
"""
import re


class EvidenceError(ValueError):
    pass


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
}
NUMBERS = {'pid', 'guest_ms', 'icount', 'width', 'height', 'bpp', 'duration_ms', 'game_frames',
           'presents', 'avg_milli', 'median_milli', 'p05_milli', 'p95_us',
           'samples', 'underruns', 'consumed_frames', 'non_silent_frames', 'input_id', 'latency_us',
           'latency_frames', 'counter', 'round_trips', 'count', 'system_writes', 'peak_bytes'}
BOOLS = {'qualified', 'corrupt', 'default_choices', 'menu_entry', 'uninstall_registered',
         'key_removed', 'menu_removed', 'handled'}
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
        elif case == 'focus':
            require(int(r['round_trips']) <= 10, 'too many focus round trips')
        elif case == 'ledger':
            require(r['when'] in ('pre','post') and r['kind'] in LEDGERS, 'invalid resource ledger')
        elif case in ('install','uninstall'):
            require(r['segment'] == case, 'installer record in wrong segment')

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

    def verdict(self, host, *, focus=False, uninstall=False, timeout=False, hardware=False,
                policy=None, exit_codes=(0,)):
        """Combine guest observations with independent host/operator evidence.

        Missing evidence never passes. Hardware policy must be a lead-disposed
        explicit duration/fps/input threshold; this module cannot resolve Q7.
        """
        if timeout:
            return {'outcome': 'fail', 'reason': 'host_timeout'}
        if self.outcome == 'not_run':
            return {'outcome': 'not_run', 'reason': self.terminal['reason']}
        if self.outcome == 'fail':
            return {'outcome': 'fail', 'reason': 'guest_failure'}
        if any(r.get('case') == 'crash' or r.get('case') == 'stub' and r['handled'] == '0'
               or r.get('case') == 'loader' and r['status'] == 'FAIL' for r in self.records):
            return {'outcome': 'fail', 'reason': 'crash_stub_or_loader_failure'}
        missing, failures = [], []
        if self.outcome != 'pass':
            missing.append('terminal_PASS')
        if not self.identity:
            missing.append('identity')
        needed = ['install','start','play','exit']+(['focus'] if focus else [])+(['uninstall'] if uninstall else [])
        for segment in needed:
            if 'end' not in self.segments.get(segment, {}):
                missing.append('segment_'+segment)
        host_needed = ['installed_files','menu_screen','play_screen','game_progress','audio_reference','desktop_screen','input_acknowledged']
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

        install = one('install','install')
        if install:
            if install['default_choices'] != '1' or install['system_writes'] != '0' or install['menu_entry'] != '1':
                failures.append('installation')
            if (install['uninstall_registered'] == '1') != uninstall:
                missing.append('uninstall_registration_policy')
        start = self.segments.get('start', {})
        if 'end' in start and start['end']-start['begin'] > 120000:
            failures.append('start_deadline')
        play = self.segments.get('play', {})
        duration = play.get('end', 0)-play.get('begin', 0)
        if 'end' in play and duration < 180000:
            failures.append('short_play')
        if self.last_ms > 600000:
            failures.append('case_guest_time_ceiling')
        frames, fps, audio = one('frames','play'), one('fps','play'), one('audio','play')
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
        if uninstall:
            observation = one('uninstall','uninstall')
            if observation and (observation['key_removed'] != '1' or observation['menu_removed'] != '1'):
                failures.append('uninstall')
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
        return {'outcome': 'fail' if failures else 'not_run' if missing else 'pass',
                'failures': sorted(set(failures)), 'missing': sorted(set(missing))}
