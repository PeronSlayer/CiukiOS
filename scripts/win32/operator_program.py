#!/usr/bin/env python3
"""f5-15C: the guest-executable part of a per-title operator record.

tests/win32/operators/<id>.json (the f5-17A hardware-operator format) gains a
`guest` block that the QEMU `installers` tier executes in the guest:

    "guest": {
      "schema_version": 1,
      "status": "proposed" | "calibrated" | "qualified",
      "choices": "default",
      "entry": "SETUP.EXE",               # relative to the payload medium root
      "arguments": "",
      "timeout_seconds": 330,             # install segment (install + uninstall <= 480)
      "defaults": {"enabled": true, "idle_ms": 4000, "max": 40},
      "restart": "accept" | "decline",
      "uninstall": {"enabled": true, "timeout_seconds": 150},
      "shot_ms": 1500,
      "steps": [ {"segment": "install", "action": "button", "button": "Next*"}, ... ]
    }

compile_program() renders the line program parsed by the guest
(src/probes/win32_installers/title_program.h). Every value is percent-encoded
outside the safe set, so a value carries no space or '='. `mode` selects the
calibration stage: "install" disables the uninstall segment (the host delta
then is the install manifest), "full" (default) runs it when the installer
registered an Add/Remove Programs entry.
"""
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ACTIONS = ('default', 'button', 'keys', 'text', 'click', 'wait', 'gone', 'expect')
SEGMENTS = ('install', 'uninstall')
STATUSES = ('proposed', 'calibrated', 'qualified')
GUEST_KEYS = {'schema_version', 'status', 'note', 'choices', 'entry', 'arguments', 'timeout_seconds', 'defaults',
              'restart', 'uninstall', 'shot_ms', 'steps'}
STEP_KEYS = {'segment', 'action', 'title', 'class', 'button', 'text', 'value', 'keys', 'x', 'y',
             'timeout_seconds', 'settle_ms', 'repeat', 'optional', 'replace', 'note'}
SAFE = re.compile(rb'[A-Za-z0-9._,:/\\*?+()<>!#-]')
NAMED_KEYS = {'enter', 'tab', 'space', 'escape', 'backspace', 'up', 'down', 'left', 'right', 'home', 'end',
              'delete', 'pageup', 'pagedown', *('f%d' % i for i in range(1, 13))}
TYPEABLE = set(" `~\\|1234567890-=!@#$%^&*()_+qwertyuiop[]QWERTYUIOP{}asdfghjkl;'ASDFGHJKL:\"zxcvbnm,./ZXCVBNM<>?")
LINE = 640
SEGMENT_BUDGET = 480
MAX_STEPS = 64


def require(condition, message):
    if not condition:
        raise ValueError(message)


def encode(value):
    data = value.encode('ascii') if isinstance(value, str) else value
    require(all(0 < b < 128 for b in data), 'operator values are non-NUL ASCII')
    return ''.join(chr(b) if SAFE.fullmatch(bytes([b])) else '%%%02X' % b for b in data)


def chord_ok(text):
    parts = text.split('+')
    *mods, key = parts
    require(all(m in ('alt', 'shift', 'ctrl') for m in mods) and len(set(mods)) == len(mods), 'key chord modifiers: ' + text)
    require(key in NAMED_KEYS or (len(key) == 1 and (key.islower() or key.isdigit() or key in "`-=[];',./\\")),
            'key chord key (shifted characters are shift+<base>): ' + text)
    return text


def validate(guest):
    require(isinstance(guest, dict), 'guest block must be an object')
    require(set(guest) <= GUEST_KEYS, 'guest keys: ' + ', '.join(sorted(set(guest) - GUEST_KEYS)))
    require(guest.get('schema_version') == 1, 'guest schema_version 1')
    require(guest.get('status') in STATUSES, 'guest status')
    require(guest.get('choices') == 'default', 'the installers tier installs with default choices only')
    entry = guest.get('entry')
    require(isinstance(entry, str) and entry and not entry.startswith(('\\', '/')) and ':' not in entry and '..' not in entry,
            'guest entry is a relative path on the payload medium')
    require(isinstance(guest.get('arguments', ''), str), 'guest arguments')
    require(type(guest.get('timeout_seconds', 480)) is int and 1 <= guest.get('timeout_seconds', 480) <= 600, 'install timeout')
    defaults = guest.get('defaults', {'enabled': True})
    require(isinstance(defaults, dict) and set(defaults) <= {'enabled', 'idle_ms', 'max'} and type(defaults.get('enabled')) is bool,
            'defaults block')
    require(type(defaults.get('idle_ms', 4000)) is int and 500 <= defaults.get('idle_ms', 4000) <= 60000, 'defaults idle_ms')
    require(type(defaults.get('max', 40)) is int and 0 <= defaults.get('max', 40) <= 200, 'defaults max')
    require(guest.get('restart', 'accept') in ('accept', 'decline'), 'restart answer')
    uninstall = guest.get('uninstall', {'enabled': True})
    require(isinstance(uninstall, dict) and set(uninstall) <= {'enabled', 'timeout_seconds'} and type(uninstall.get('enabled')) is bool,
            'uninstall block')
    require(type(uninstall.get('timeout_seconds', 240)) is int and 1 <= uninstall.get('timeout_seconds', 240) <= 600, 'uninstall timeout')
    require(type(guest.get('shot_ms', 1500)) is int and 0 <= guest.get('shot_ms', 1500) <= 10000, 'shot_ms')
    steps = guest.get('steps', [])
    require(isinstance(steps, list) and len(steps) <= MAX_STEPS, 'at most %d steps' % MAX_STEPS)
    seen_uninstall = False
    for step in steps:
        require(isinstance(step, dict) and set(step) <= STEP_KEYS, 'step keys: ' + json.dumps(step)[:80])
        require(step.get('segment') in SEGMENTS and step.get('action') in ACTIONS, 'step segment/action')
        if step['segment'] == 'uninstall':
            seen_uninstall = True
        require(step['segment'] == 'uninstall' or not seen_uninstall, 'install steps precede uninstall steps')
        action = step['action']
        target = any(step.get(k) for k in ('title', 'class', 'button', 'text'))
        for key in ('title', 'class', 'button', 'text', 'value', 'note'):
            require(key not in step or isinstance(step[key], str) and step[key], key + ' must be a nonempty string')
        require(action != 'button' or step.get('button'), 'button action needs button=')
        require(action != 'keys' or isinstance(step.get('keys'), list) and step['keys'], 'keys action needs keys')
        require(action == 'keys' or 'keys' not in step, 'keys only for the keys action')
        for chord in step.get('keys', []):
            chord_ok(chord)
        require(action != 'text' or step.get('value'), 'text action needs value')
        require(action == 'text' or 'value' not in step, 'value only for the text action')
        require(all(c in TYPEABLE for c in step.get('value', '')), 'text value must be typeable on the US layout')
        require(action != 'click' or (type(step.get('x')) is int and type(step.get('y')) is int and
                                      0 <= step['x'] <= 4095 and 0 <= step['y'] <= 4095), 'click needs x, y')
        require(action == 'click' or ('x' not in step and 'y' not in step), 'x, y only for click')
        require(action != 'expect' or step.get('text'), 'expect needs text=')
        require(target or action in ('default', 'keys', 'text'), 'the action needs a window match')
        require(type(step.get('timeout_seconds', 60)) is int and 1 <= step.get('timeout_seconds', 60) <= 600, 'step timeout')
        require(type(step.get('settle_ms', 500)) is int and 0 <= step.get('settle_ms', 500) <= 30000, 'settle_ms')
        require(type(step.get('repeat', 1)) is int and 1 <= step.get('repeat', 1) <= 64, 'repeat')
        for key in ('optional', 'replace'):
            require(type(step.get(key, False)) is bool, key + ' is boolean')
    require(not seen_uninstall or uninstall.get('enabled'), 'uninstall steps need uninstall.enabled')
    # The whole case is bounded at 600 s on the host, boot and the kernel's
    # inventory included; the observer itself gives up at 560 s.
    total = guest.get('timeout_seconds', 480) + (uninstall.get('timeout_seconds', 240) if uninstall.get('enabled') else 0)
    require(total <= SEGMENT_BUDGET, 'install + uninstall timeouts exceed %d s' % SEGMENT_BUDGET)
    return guest


def compile_program(workload, guest, mode='full'):
    validate(guest)
    require(mode in ('install', 'full'), 'mode is install or full')
    defaults = guest.get('defaults', {'enabled': True})
    uninstall = guest.get('uninstall', {'enabled': True})
    enabled = uninstall.get('enabled') and mode == 'full'
    lines = ['CIUKIOP 1', 'workload ' + encode(workload), 'choices default', 'entry ' + encode(guest['entry'])]
    if guest.get('arguments'):
        lines.append('arguments ' + encode(guest['arguments']))
    lines += ['timeout %d' % guest.get('timeout_seconds', 480),
              'defaults %d idle=%d max=%d' % (defaults['enabled'], defaults.get('idle_ms', 4000), defaults.get('max', 40)),
              'restart ' + guest.get('restart', 'accept'),
              'uninstall %d timeout=%d' % (enabled, uninstall.get('timeout_seconds', 240)),
              'shot %d' % guest.get('shot_ms', 1500)]
    for step in guest.get('steps', []):
        if step['segment'] == 'uninstall' and not enabled:
            continue
        words = ['step', step['segment'], step['action']]
        for key in ('title', 'class', 'button', 'text', 'value'):
            if key in step:
                words.append(key + '=' + encode(step[key]))
        if 'keys' in step:
            words.append('keys=' + encode(','.join(step['keys'])))
        for key in ('x', 'y'):
            if key in step:
                words.append('%s=%d' % (key, step[key]))
        for key, name, default in (('timeout_seconds', 'timeout', 60), ('settle_ms', 'settle', 500), ('repeat', 'repeat', 1)):
            if step.get(key, default) != default:
                words.append('%s=%d' % (name, step[key]))
        for key in ('optional', 'replace'):
            if step.get(key):
                words.append(key + '=1')
        line = ' '.join(words)
        require(len(line) < LINE, 'operator line too long')
        lines.append(line)
    lines.append('end')
    return '\n'.join(lines) + '\n'


def load(workload, root=ROOT):
    path = Path(root) / 'tests/win32/operators' / (workload + '.json')
    record = json.loads(path.read_text())
    require(record.get('workload') == workload, 'operator record workload')
    require('guest' in record, 'operator record has no guest block: ' + workload)
    return record, validate(record['guest'])


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('workload')
    parser.add_argument('--mode', choices=('install', 'full'), default='full')
    args = parser.parse_args()
    _, guest = load(args.workload)
    print(compile_program(args.workload, guest, args.mode), end='')
