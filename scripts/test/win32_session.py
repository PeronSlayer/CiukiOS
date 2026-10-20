"""Host-only F5 session model: fw_cfg selector, ARM-paced QMP and screen oracle.

Existing run.py suites are untouched. Caller supplies monotonic milliseconds,
QMP, guest records and independent screenshot references; no VM is launched.
"""
import hashlib
import re

FW_CFG_ITEM = 'opt/it.alcybercloud.ciukios/test'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def selector(request, *, source='fw_cfg', validated_fw_cfg=False):
    require(source == 'fw_cfg' and validated_fw_cfg, 'Win32 selection requires validated fw_cfg')
    match = re.fullmatch(r'f5:(win32-[a-z0-9]+(?:-[a-z0-9]+)*) run=([0-9a-f]{8})', request)
    require(match is not None and len(request) <= 160, 'invalid Win32 selector')
    return {'phase': 5, 'probe': match[1], 'run': match[2], 'item': FW_CFG_ITEM}


def validate_events(events):
    require(isinstance(events, list) and 1 <= len(events) <= 64, 'input batch must contain 1..64 events')
    for event in events:
        require(isinstance(event, dict) and set(event) == {'type','data'}, 'input event fields')
        kind, data = event['type'], event['data']
        require(isinstance(data, dict), 'input data')
        if kind == 'key':
            require(set(data) == {'down','key'} and type(data['down']) is bool, 'key fields')
            key = data['key']
            require(isinstance(key, dict) and set(key) == {'type','data'} and key['type'] == 'qcode'
                    and isinstance(key['data'], str) and re.fullmatch('[a-z0-9_]+', key['data']), 'qcode key')
        elif kind == 'btn':
            require(set(data) == {'down','button'} and type(data['down']) is bool and data['button'] in ('left','middle','right'), 'button fields')
        elif kind == 'rel':
            require(set(data) == {'axis','value'} and data['axis'] in ('x','y') and type(data['value']) is int and abs(data['value']) <= 32767, 'relative motion')
        else:
            raise ValueError('unsupported QMP input kind')


def screen_name(value):
    return isinstance(value, str) and re.fullmatch('[a-z0-9]+(?:-[a-z0-9]+)*', value) is not None


def screen_names(script):
    """Every screen check a validated script waits for (anchors and steps)."""
    names = set()
    for segment in script['segments']:
        anchor = segment.get('anchor', {'type': 'arm'})
        if anchor['type'] == 'screen':
            names.add(anchor['check'])
        for step in segment['steps']:
            if 'after_screen' in step:
                names.add(step['after_screen'])
    return names


def latency_input_ids(script):
    """The input IDs whose guest latency record the verdict must match."""
    return [step['input_id'] for segment in script['segments'] for step in segment['steps'] if step.get('measure_latency')]


def validate_script(script):
    try:
        require(script['schema_version'] == 1 and script['clock'] == 'host-monotonic-ms', 'script version/clock')
        require(type(script['timeout_ms']) is int and 0 < script['timeout_ms'] <= 600000, 'script timeout')
        require(re.fullmatch('[a-z0-9-]+', script['workload']) and re.fullmatch('[a-z0-9-]+', script['scene']), 'script identity')
        require(isinstance(script['segments'], list) and 0 < len(script['segments']) <= 6, 'segments list')
        order = ('install','start','play','focus','exit','uninstall')
        previous = -1
        input_ids = set()
        for segment in script['segments']:
            require(segment['name'] in order and order.index(segment['name']) > previous, 'script segment order')
            previous = order.index(segment['name'])
            # f5-17B: a segment's steps are timed from its ARM (default) or from
            # the host-observed match of a named screen region, never a fixed sleep.
            anchor = segment.get('anchor', {'type': 'arm'})
            require(isinstance(anchor, dict) and anchor.get('type') in ('arm', 'screen'), 'segment anchor type')
            if anchor['type'] == 'screen':
                require(set(anchor) == {'type', 'check'} and screen_name(anchor['check']), 'screen anchor needs a check name')
            else:
                require(set(anchor) == {'type'}, 'arm anchor carries no fields')
            steps = segment['steps']
            require(isinstance(steps, list) and len(steps) <= 4096, 'script step cap')
            last = -1
            held = set()
            for step in steps:
                require(type(step['input_id']) is int and step['input_id'] > 0 and step['input_id'] not in input_ids, 'input IDs must be unique positive integers')
                input_ids.add(step['input_id'])
                at = step['at_ms']
                if 'after_screen' in step:
                    last = -1  # offsets restart at a new screen anchor
                require(type(at) is int and last < at < script['timeout_ms'], 'input times must increase within timeout')
                last = at
                require(set(step) <= {'input_id', 'at_ms', 'events', 'after_screen', 'measure_latency', 'note'}, 'unknown step field')
                if 'after_screen' in step:
                    require(screen_name(step['after_screen']), 'after_screen needs a check name')
                if 'measure_latency' in step:
                    require(type(step['measure_latency']) is bool, 'measure_latency is boolean')
                if 'note' in step:
                    require(isinstance(step['note'], str) and len(step['note']) <= 200, 'step note')
                validate_events(step['events'])
                for event in step['events']:
                    if event['type'] == 'rel':
                        continue
                    data = event['data']
                    key = (event['type'], data['key']['data'] if event['type'] == 'key' else data['button'])
                    require((key not in held) if data['down'] else (key in held), 'unbalanced key/button transition')
                    if data['down']:
                        held.add(key)
                    else:
                        held.remove(key)
            require(not held, 'script leaves a key/button held')
    except (KeyError, TypeError) as error:
        raise ValueError('malformed input script') from error
    return script


class InputPlayer:
    """One batch per tick; lateness preserves minimum gaps, never catch-up bursts.

    All times are HOST monotonic ms, not guest icount. ARM is trusted only after
    the Parser validates its run/sequence; selector must match script identity.
    Acknowledgements record host timing, not input latency or game progress.
    """
    def __init__(self, script, request, *, started_at_ms, validated_fw_cfg=False, screen_time=None):
        validate_script(script)
        selected = selector(request, validated_fw_cfg=validated_fw_cfg)
        require(selected['probe'] == 'win32-'+script['workload'], 'selector/script mismatch')
        self.script, self.selected = script, selected
        require(type(started_at_ms) is int and started_at_ms >= 0, 'host launch time required')
        # f5-17B: host ms at which a named screen check matched, or None while
        # it is still unmatched; the runner session supplies it (never a sleep).
        require(screen_time is not None or not screen_names(script), 'screen-anchored script needs a screen oracle')
        self.screen_time = screen_time
        self.waiting_for = None
        self.index = 0
        self.step = 0
        self.armed_at = None
        self.started_at = started_at_ms
        self.seen_begin = False
        self.last_now = None
        self.last_sent = None
        self.anchor_name = None
        self.observed = []

    def _time(self, now_ms):
        require(type(now_ms) is int and now_ms >= 0, 'host monotonic time must be unsigned integer ms')
        require(now_ms >= self.started_at, 'time precedes host launch')
        require(self.last_now is None or now_ms >= self.last_now, 'host clock moved backwards')
        self.last_now = now_ms
        if now_ms-self.started_at >= self.script['timeout_ms']:
            raise TimeoutError('Win32 scripted session deadline')

    def observe(self, record, now_ms):
        self._time(now_ms)
        if not self.seen_begin:
            require(record.get('event') == 'BEGIN', 'session requires BEGIN')
            self.seen_begin = True
        require(record.get('run') == self.selected['run'] and record.get('probe') == self.selected['probe'], 'record/session mismatch')
        if record.get('case') == 'identity':
            require(record.get('scene') == self.script['scene'], 'script scene mismatch')
        if record.get('event') != 'ARM':
            return
        require(self.index < len(self.script['segments']) and self.armed_at is None, 'duplicate/unexpected input ARM')
        segment = self.script['segments'][self.index]
        require(record.get('segment') == segment['name'] and record.get('action') == 'script', 'input ARM segment mismatch')
        self.armed_at = now_ms
        self.step = 0
        self.last_sent = None
        self.anchor_name = segment.get('anchor', {'type': 'arm'}).get('check')

    def tick(self, now_ms, qmp):
        self._time(now_ms)
        if self.armed_at is None:
            return None
        steps = self.script['segments'][self.index]['steps']
        if self.step == len(steps):
            self.index += 1
            self.armed_at = None
            return None
        step = steps[self.step]
        segment = self.script['segments'][self.index]
        if 'after_screen' in step:
            self.anchor_name = step['after_screen']
        anchor_name = self.anchor_name
        anchor = self.armed_at
        if anchor_name is not None:
            matched = self.screen_time(anchor_name)
            if matched is None:
                self.waiting_for = anchor_name
                return None
            require(type(matched) is int and matched >= self.armed_at, 'screen match precedes the segment ARM')
            anchor = matched
        self.waiting_for = None
        gap = step['at_ms']-steps[self.step-1]['at_ms'] if self.step and 'after_screen' not in step else 0
        due = max(anchor+step['at_ms'], (self.last_sent+gap) if self.last_sent is not None else 0)
        if now_ms < due:
            return None
        qmp.input_events(step['events'])  # failure does not advance the script
        receipt = {'segment': segment['name'], 'step': self.step,
                   'input_id': step['input_id'], 'anchor': 'screen:'+anchor_name if anchor_name else 'arm',
                   'scheduled_host_ms': anchor+step['at_ms'], 'sent_host_ms': now_ms,
                   'late_ms': now_ms-(anchor+step['at_ms']), 'qmp_acknowledged': True,
                   'measure_latency': bool(step.get('measure_latency'))}
        self.observed.append(receipt)
        self.last_sent = now_ms
        self.step += 1
        if self.step == len(steps):
            self.index += 1
            self.armed_at = None
        return receipt

    @property
    def complete(self):
        return self.index == len(self.script['segments'])

    def pending(self):
        """Diagnostic for a timed-out case: where the script stopped."""
        if self.complete:
            return None
        segment = self.script['segments'][self.index]
        return {'segment': segment['name'], 'step': self.step, 'steps': len(segment['steps']),
                'armed': self.armed_at is not None, 'waiting_for_screen': self.waiting_for}


def region(frame, rectangle):
    """Frame is (width, height, RGB bytes) from QMP P6 read_ppm, row-major."""
    width, height, pixels = frame
    require(type(width) is int and type(height) is int and width > 0 and height > 0
            and isinstance(pixels, bytes) and len(pixels) == width*height*3, 'invalid RGB frame')
    require(isinstance(rectangle, (list, tuple)) and len(rectangle) == 4 and all(type(v) is int for v in rectangle), 'invalid region')
    x, y, w, h = rectangle
    require(x >= 0 and y >= 0 and w > 0 and h > 0 and x+w <= width and y+h <= height, 'region outside screen')
    return b''.join(pixels[((y+row)*width+x)*3:((y+row)*width+x+w)*3] for row in range(h))


def screen_check(frame, check, reference=None):
    """Exact region digest or explicitly bounded reference-pixel tolerance.

    Both modes pin the same reference region SHA-256. Changing enough pixels
    fails even if the average colour is similar; frames never prove progress
    by themselves. Tolerance is per RGB channel and bad-pixel count.
    """
    pixels = region(frame, check['region'])
    expected = check['sha256']
    require(isinstance(expected, str) and re.fullmatch('[0-9a-f]{64}', expected), 'screen digest pin required')
    actual = hashlib.sha256(pixels).hexdigest()
    mode = check['mode']
    if mode == 'sha256':
        return {'match': actual == expected, 'sha256': actual}
    require(mode == 'tolerance' and reference is not None, 'tolerance needs a pinned reference frame')
    golden = region(reference, check['region'])
    require(hashlib.sha256(golden).hexdigest() == expected, 'reference digest mismatch')
    require(frame[:2] == reference[:2], 'reference geometry mismatch')
    delta, maximum = check['channel_delta'], check['max_bad_pixels']
    require(type(delta) is int and 0 <= delta <= 255 and type(maximum) is int and 0 <= maximum < len(pixels)//3, 'invalid screen tolerance')
    bad = sum(any(abs(pixels[i+k]-golden[i+k]) > delta for k in range(3)) for i in range(0, len(pixels), 3))
    return {'match': bad <= maximum, 'sha256': actual, 'bad_pixels': bad}
