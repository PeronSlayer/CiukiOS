#!/usr/bin/env python3
"""fx-accept: the owner's T23 trial list as an automated acceptance run on QEMU.

Owner decision 2026-10-27 (docs/directives/fx-accept.md): every hardware image
first passes the trial list on the `qemu-t23` profile; a failing item blocks
the image. The suite is tests/suites/acc-t23.json, run by
`scripts/test/run.py acc-t23` or `scripts/test/acceptance.sh`, which also
writes the one-page `build/test-runs/acc-t23/summary.txt`.

Case kind "acceptance" (run.py `case_kind`): a boot exactly as a user boots
(no fw_cfg test request, no probe), driven by the runner like a person at the
keyboard and judged on serial lines:

* the runner presses keys at the loader menu (`n` Normal, F5 Safe mode, as
  documented in the menu hint) and on the desktop through QMP
  `input-send-event`, types text, inserts and ejects generated ISO images
  (`blockdev-change-medium`, `eject`), takes screendumps and waits for the
  QMP guest events RESET and SHUTDOWN;
* every pass predicate is a line-anchored regular expression over the serial
  capture (kernel `klog` lines, `[user N] [desktop] ...` diagnostic records,
  `[console] ...` program output), counted in a window of the timeline, or an
  independent disk observation of the run's overlay after QEMU stopped
  (win32_inventory.overlay_delta). Screendumps pace the input (wait for a
  window to appear, for a wizard page to stay still) and are failure
  evidence; a `screen` step checks only geometry and the desktop layout
  colours of run.observe_normal_boot_screen, never a frame digest.

Steps run in order: `wait` (patterns that must all appear, in order; `fail`
patterns end the step; `wait_event` a QMP guest event), `settle`, `actions`,
`screen_settle`, `screen`. A wait searches the serial lines after the cursor:
the line after the previous match, or the moment the previous step began
sending its actions, whichever is later; `since` widens that to the current
boot or the whole case. Named groups `(?P<name>...)` are captured and
`<<name>>` in a later pattern is replaced by the escaped value; `<<media_<m>_root>>`
is the root entry count of the generated image <m>. A failed required step
stops the script unless it declares `on_fail: "continue"`; `required: false`
steps record a failure (`known_failure` names its directive) and continue;
`requires` skips a step whose prerequisites did not pass. A `fallback` runs
other actions after a wait timed out and waits again (`counts_as` pass|fail).
A `wizard` drives an installer while its wait runs: whenever the screen has
not changed for `idle` seconds it presses the next key, but only while the
last `[desktop] focus` record matches `press_when`; a focus matching
`close_when` (a Files window opened over the installer) gets `close_keys`.
A step with `template` expands to a standard boot (loader key, kernel,
desktop, display mode record).

QEMU Machine Protocol sources: input-send-event, blockdev-change-medium,
eject, screendump, RESET and SHUTDOWN events
(https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html); -no-reboot is
omitted when a case expects a guest reset, -no-shutdown is kept so ACPI S5
pauses QEMU with a SHUTDOWN event (reason guest-shutdown)
(https://www.qemu.org/docs/master/system/invocation.html). An empty CD-ROM
is an `-drive if=none` backend without a file on the secondary master, the
T23's drive position.
"""
import hashlib
import json
import re
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import resources as res  # noqa: E402
from evidence import EvidenceError, EvidenceNotRun  # noqa: E402
import iso_fixtures  # noqa: E402
import dos_games  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
KIND = 'acceptance'
CASE_KEYS = {'id', 'kind', 'probe', 'profile', 'timeout', 'expected', 'item', 'items', 'title', 'gate', 'devices',
             'media', 'resets', 'forbid', 'steps', 'assertions', 'figures', 'disk', 'pending', 'note',
             'blocking', '_run_id', '_prune_keep', 'dos_game', 'guests', '_lan'}
STEP_KEYS = {'name', 'check', 'wait', 'fail', 'since', 'timeout', 'wait_event', 'settle', 'actions',
             'screen_settle', 'screen', 'required', 'on_fail', 'known_failure', 'requires', 'unless', 'fallback',
             'arm', 'note', 'wizard'}
TEMPLATE_KEYS = {'template', 'prefix', 'check', 'note', 'settle', 'timeout', 'user', 'password'}
ACTION_KEYS = {'keys', 'text', 'pause', 'media', 'screendump', 'sleep', 'hold', 'mouse', 'power_cut'}
ASSERTION_KEYS = {'name', 'check', 'pattern', 'from', 'to', 'min', 'max', 'required', 'known_failure', 'note'}
FIGURE_KEYS = {'name', 'check', 'pattern', 'from', 'to', 'reducer', 'page'}
WIZARD_KEYS = {'keys', 'then', 'idle', 'max', 'focus', 'press_when', 'close_when', 'close_keys', 'max_closes',
               'region', 'start_after', 'pid', 'pages', 'page_timeout', 'retry', 'max_retries'}
PAGE_KEYS = {'name', 'title', 'modal', 'keys', 'timeout'}
REDUCERS = ('dosvm_perf', 'lines', 'count', 'first', 'last')
NICS = (None, 'i82559er', 'rtl8139', 'ne2k_pci', 'e1000', 'pcnet')
EVENTS = ('RESET', 'SHUTDOWN')
DEFAULT_TIMEOUT = 60.0
DEFAULT_PAUSE = 0.25
TEXT_PAUSE = 0.12
KEY_HOLD = 0.06
MAX_LINES = 200000
SCREEN_POLL = 0.5
WIZARD_POLL = 1.0   # a 1024x768 screendump is 2.3 MB: one per second while an installer runs
PACE_FILE = 'pace.ppm'
PLACEHOLDER = re.compile(r'<<([a-z][a-z0-9_]{0,31})>>')
GROUP = re.compile(r'\(\?P<([a-z][a-z0-9_]{0,31})>')
BOOT_LINE = re.compile(r'L:CPU signature=')
# Lines that end any acceptance case at once: run.NORMAL_BOOT_NEGATIVE minus
# the request-specific ones a case may expect (L:SAFE=1 for a Safe mode boot).
BASE_FORBID = (r'^Ciuki VMM (?:panic|double fault)',
               r'^CIUKI_TEST ',
               r'^L:SELECT_SOURCE=(?!menu$)',
               r'^(?:CIUKI: |L:SELECT_ERROR)',
               r'^\[supervisor\] desktop (?:unavailable|stopped)')
# Diagnostic records of the desktop (apps/desktop/diag_record.h): the kernel
# prefixes "[user <task>] " (src/kernel/core/syscall.c sys_debug_write).
DESKTOP = r'\[user \d+\] \[desktop\]'
# fx-acc1: the desktop's program-window record (apps/desktop/input.c
# desk_window_record) and its focus record (focus_record): the page-driven
# wizard follows an installer's pages by their titles.
WINDOW_RECORD = re.compile(r'^\[user \d+\] \[desktop\] window pid=(?P<pid>\d+) window=(?P<window>\d+) owner=(?P<owner>\d+) '
                           r'modal=(?P<modal>[01]) state=(?P<state>[a-z]+) title=(?P<title>\S+)$')
FOCUS_RECORD = re.compile(r'^\[user \d+\] \[desktop\] focus pid=(?P<pid>\d+) window=(?P<window>\d+) ')
# sec-user-06: the trusted LOGON service and its UI (apps/logon/coordinator.c
# lgn_note, apps/logon/ui.c view_record; debug_write prefixes "[user N] ").
LOGON = r'\[user \d+\] \[logon\]'
LOGON_UI = r'\[user \d+\] \[logon-ui\]'
# Account names (the LOGON_UI typed-name grammar, lgn_name_syntax) and secrets
# (8..63 printable bytes, or empty for the ready image's blank CiukiUser).
ACCOUNT_NAME = re.compile(r'[A-Za-z0-9_-]{1,31}')

# ---- keys ----------------------------------------------------------------
# QAPI QKeyCode names (qapi/ui.json). Friendly aliases first.
ALIASES = {'enter': 'ret', 'return': 'ret', 'escape': 'esc', 'space': 'spc', 'win': 'meta_l',
           'logo': 'meta_l', 'del': 'delete', 'ins': 'insert', 'pageup': 'pgup', 'pagedown': 'pgdn',
           'backquote': 'grave_accent', 'grave': 'grave_accent', 'apps': 'menu'}
QCODES = set('abcdefghijklmnopqrstuvwxyz0123456789') | {
    'shift', 'shift_r', 'alt', 'alt_r', 'ctrl', 'ctrl_r', 'meta_l', 'meta_r', 'menu', 'esc', 'minus', 'equal',
    'backspace', 'tab', 'bracket_left', 'bracket_right', 'ret', 'semicolon', 'apostrophe', 'grave_accent',
    'backslash', 'comma', 'dot', 'slash', 'spc', 'caps_lock', 'up', 'down', 'left', 'right', 'home', 'end',
    'pgup', 'pgdn', 'insert', 'delete', 'print', 'pause'} | {'f%d' % n for n in range(1, 13)}
# US layout (the image's default keyboard, apps/desktop/settings.c): character -> chord.
US_PLAIN = {' ': 'spc', '-': 'minus', '=': 'equal', '[': 'bracket_left', ']': 'bracket_right', ';': 'semicolon',
            "'": 'apostrophe', '`': 'grave_accent', '\\': 'backslash', ',': 'comma', '.': 'dot', '/': 'slash'}
US_SHIFT = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8', '(': '9', ')': '0',
            '_': 'minus', '+': 'equal', '{': 'bracket_left', '}': 'bracket_right', ':': 'semicolon', '"': 'apostrophe',
            '~': 'grave_accent', '|': 'backslash', '<': 'comma', '>': 'dot', '?': 'slash'}


def chord(spec):
    """'ctrl+esc' -> ['ctrl', 'esc'] (pressed in order, released in reverse)."""
    if not isinstance(spec, str) or not spec:
        raise ValueError('key chord must be a non-empty string')
    keys = [ALIASES.get(k, k) for k in spec.lower().split('+')]
    if not 1 <= len(keys) <= 4 or any(k not in QCODES for k in keys) or len(set(keys)) != len(keys):
        raise ValueError('unknown key chord: ' + spec)
    return keys


def text_chords(text):
    """Typed text as US-layout chords; only printable ASCII."""
    if not isinstance(text, str) or not 1 <= len(text) <= 160:
        raise ValueError('typed text must be 1..160 characters')
    out = []
    for char in text:
        if 'a' <= char <= 'z' or '0' <= char <= '9':
            out.append([char])
        elif 'A' <= char <= 'Z':
            out.append(['shift', char.lower()])
        elif char in US_PLAIN:
            out.append([US_PLAIN[char]])
        elif char in US_SHIFT:
            out.append(['shift', US_SHIFT[char]])
        else:
            raise ValueError('character not typeable on the US layout: %r' % char)
    return out


def key_event(qcode, down):
    return {'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': qcode}}}


# ---- templates ---------------------------------------------------------------

def display_step(prefix, check, safe, settle):
    """The desktop's display record (apps/desktop/desktop.c display_start_record)."""
    p = prefix
    return {'name': p + '-display', 'check': check, 'timeout': 300, 'settle': settle,
            'wait': r'^%s display start mode=(?P<%s_mode>\d+) (?P<%s_width>\d+)x(?P<%s_height>\d+)x(?P<%s_bpp>\d+) '
                    r'saved=(?P<%s_saved>\d+) safe=%d$' % (DESKTOP, p, p, p, p, p, 1 if safe else 0)}


def logon_steps(prefix, check, user, password, safe=False, settle=8, timeout=900):
    """sec-user-06: a typed-name logon at the trusted Welcome screen
    (apps/logon/ui.c): the painted list view (accounts listed), Up twice
    (the item list wraps from the first tile through Turn Off Computer to
    "Type a user name"), Enter, the typed view, the name, Enter (to the
    password field), the password, Enter. Then LOGON's records: the
    authenticated uid, the session, the kernel's desktop record with its
    credential (security.c launch_record) and the running session, and the
    desktop's display record. A refusal or a failed session start ends the
    step. Captures: <prefix>_uid, _session, _desktop, _gid, _groups and the
    display captures. The password never reaches result.json (typed text is
    counted only) or a record."""
    p = prefix
    typed = [{'text': user}, {'keys': 'ret', 'pause': 0.5}]
    if password:
        typed.append({'text': password})
    typed.append({'keys': 'ret'})
    return [
        {'name': p + '-welcome', 'check': check, 'timeout': timeout, 'settle': 1,
         'wait': r'^%s view mode=welcome view=list items=\d+ accounts=[1-9]\d* status=\w+$' % LOGON_UI,
         'actions': [{'keys': ['up', 'up', 'ret'], 'pause': 0.5}],
         'note': 'Welcome: Up twice reaches "Type a user name", Enter'},
        {'name': p + '-typed', 'check': check, 'timeout': 60, 'settle': 0.5,
         'wait': r'^%s view mode=welcome view=typed ' % LOGON_UI, 'actions': typed,
         'note': 'typed name %s, Enter, the password, Enter' % user},
        {'name': p + '-auth', 'check': check, 'timeout': 120,
         'wait': r'^%s authenticated uid=(?P<%s_uid>\d+); starting session$' % (LOGON, p),
         'fail': [r'^%s authentication refused ' % LOGON, r'^%s session start failed: ' % LOGON]},
        {'name': p + '-desktop', 'check': check, 'timeout': 300,
         'wait': [r'^%s session (?P<%s_session>\d+) created uid=<<%s_uid>>$' % (LOGON, p, p),
                  r'^\[security\] session <<%s_session>> desktop pid=(?P<%s_desktop>\d+) pgid=\d+ uid=<<%s_uid>> '
                  r'gid=(?P<%s_gid>\d+) groups=(?P<%s_groups>[0-9,]+)$' % (p, p, p, p, p),
                  r'^%s session <<%s_session>> running uid=<<%s_uid>> start_ms=\d+$' % (LOGON, p, p)],
         'fail': [r'^%s session start failed: ' % LOGON]},
        display_step(p, check, safe, settle),
    ]


def boot_template(prefix, check, safe=False, settle=8, timeout=900, logon=None):
    """A boot as the user starts it from the loader menu (src/boot/ciukldr/menu.inc).

    Normal: `n` at SELECT_READY selects Normal at once (the menu's 3 s wait
    costs about 270 host seconds under TCG with -icount, run.py
    NORMAL_BOOT_LOADER_SECONDS); Safe mode: F5, the Windows 98 key, chooses
    Startup menu item 3 (startup.inc: CBI_F_SAFE_MODE|CBI_F_SAFE_STORAGE,
    record L:STARTUP=3). Then the kernel banner, the desktop process and the
    desktop's display record (apps/desktop/desktop.c display_start_record).
    sec-user-06: the desktop process is PID 1's ("[supervisor] desktop pid=")
    or, with the trusted LOGON service, the one LOGON's SESSION_LAUNCH
    published ("[security] session S desktop pid="), reached without input by
    the ready image's automatic logon. Safe mode never logs on automatically
    (apps/logon/coordinator.c), so boot-safe logs on as the blank CiukiUser;
    logon=(user, password) logs on at Welcome instead of waiting for the
    automatic logon (boot-logon).
    Captures: <prefix>_build, _desktop, _mode, _width, _height, _bpp, _saved
    (and with a logon _uid, _session, _gid, _groups)."""
    p = prefix
    kernel = ([r'^L:STARTUP=3$'] if safe else []) + [
        r'^L:SELECT_SOURCE=menu$', r'^Ciuki VMM F0 build (?P<%s_build>[!-~]+) - CiukiOS$' % p]
    steps = [
        {'name': p + '-loader', 'check': check, 'wait': r'^SELECT_READY$', 'timeout': timeout,
         'actions': [{'keys': 'f5' if safe else 'n'}],
         'note': 'loader menu: %s' % ('F5, Safe mode' if safe else 'N, Normal')},
        {'name': p + '-kernel', 'check': check, 'wait': kernel, 'timeout': 600,
         'fail': [r'^L:SAFE=1$', r'^L:STARTUP=[1245678]$'] if safe else [r'^L:SAFE=1$', r'^L:STARTUP=']},
    ]
    if safe and logon is None:
        logon = ('CiukiUser', '')
    if logon is not None:
        return steps + logon_steps(p, check, logon[0], logon[1], safe, settle, timeout)
    return steps + [
        {'name': p + '-desktop', 'check': check, 'timeout': timeout,
         'wait': r'^(?:\[supervisor\]|\[security\] session (?P<%s_session>\d+)) desktop pid=(?P<%s_desktop>\d+) pgid=\d+' % (p, p)},
        display_step(p, check, safe, settle),
    ]


TEMPLATES = {'boot': lambda p, c, s, t, a: boot_template(p, c, False, s, t),
             'boot-safe': lambda p, c, s, t, a: boot_template(p, c, True, s, t),
             'boot-logon': lambda p, c, s, t, a: boot_template(p, c, False, s, t, logon=a),
             'boot-safe-logon': lambda p, c, s, t, a: boot_template(p, c, True, s, t, logon=a),
             'logon': lambda p, c, s, t, a: logon_steps(p, c, a[0], a[1], False, s, t)}
LOGON_TEMPLATES = ('boot-logon', 'boot-safe-logon', 'logon')


def expand_steps(steps):
    """Replace template steps by their expansion; other steps are copied."""
    out = []
    for index, step in enumerate(steps if isinstance(steps, list) else []):
        if isinstance(step, dict) and 'template' in step:
            refuse(not set(step) - TEMPLATE_KEYS, 'step %d: template keys %s' % (index, ', '.join(sorted(TEMPLATE_KEYS))))
            refuse(step['template'] in TEMPLATES, 'step %d: template %s' % (index, ' | '.join(TEMPLATES)))
            prefix = step.get('prefix')
            refuse(isinstance(prefix, str) and re.fullmatch('[a-z][a-z0-9]{0,7}', prefix),
                   'step %d: template prefix [a-z][a-z0-9]{0,7}' % index)
            settle, timeout = step.get('settle', 8), step.get('timeout', 900)
            refuse(type(settle) in (int, float) and 0 <= settle <= 120 and type(timeout) is int and 60 <= timeout <= 3600,
                   'step %d: template settle 0..120, timeout 60..3600' % index)
            account = None
            if step['template'] in LOGON_TEMPLATES:
                user, password = step.get('user'), step.get('password', '')
                refuse(isinstance(user, str) and ACCOUNT_NAME.fullmatch(user), 'step %d: template user [A-Za-z0-9_-]{1,31}' % index)
                refuse(isinstance(password, str) and (not password or 8 <= len(password) <= 63),
                       'step %d: template password empty or 8..63 characters' % index)
                for text in (user, password):
                    if text:
                        try:
                            text_chords(text)
                        except ValueError as error:
                            raise res.Refusal('acceptance case: step %d: %s' % (index, error)) from error
                account = (user, password)
            else:
                refuse('user' not in step and 'password' not in step, 'step %d: user/password only with %s' % (index, ', '.join(LOGON_TEMPLATES)))
            out.extend(TEMPLATES[step['template']](prefix, step.get('check', 'main'), settle, timeout, account))
        else:
            out.append(step)
    return out


# ---- validation --------------------------------------------------------------

def refuse(condition, message):
    if not condition:
        raise res.Refusal('acceptance case: ' + message)


def compile_pattern(pattern, where):
    refuse(isinstance(pattern, str) and pattern.startswith('^'), where + ': patterns are line-anchored regular expressions (^...)')
    try:
        re.compile(PLACEHOLDER.sub('x', pattern))
    except re.error as error:
        raise res.Refusal('acceptance case: %s: invalid pattern %r: %s' % (where, pattern, error)) from error
    return pattern


def patterns(value, where):
    if value is None:
        return []
    items = [value] if isinstance(value, str) else value
    refuse(isinstance(items, list) and items, where + ': a pattern or a non-empty list of patterns')
    return [compile_pattern(p, where) for p in items]


def media_var(name):
    return 'media_%s_root' % name.replace('-', '_')


def validate_action(action, where):
    refuse(isinstance(action, dict) and action and not set(action) - ACTION_KEYS, where + ': action keys ' + ', '.join(sorted(ACTION_KEYS)))
    kinds = [k for k in ('keys', 'text', 'media', 'screendump', 'sleep', 'mouse', 'power_cut') if k in action]
    refuse(len(kinds) == 1, where + ': one of keys, text, media, screendump, sleep, mouse or power_cut per action')
    if 'mouse' in action:
        mouse = action['mouse']
        refuse(isinstance(mouse, dict) and set(mouse) <= {'dx', 'dy', 'button'} and mouse,
               where + ': mouse dx/dy/button')
        refuse(all(type(mouse.get(axis, 0)) is int and -32767 <= mouse.get(axis, 0) <= 32767 for axis in ('dx', 'dy')),
               where + ': bounded relative mouse motion')
        refuse(mouse.get('button') in (None, 'left', 'middle', 'right'), where + ': mouse button')
    # sec-user-06 (SU-13): a host reset through QMP system_reset, the runner's
    # power cut: the guest loses what it had not written, the overlay keeps
    # what reached it (https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html).
    refuse(action.get('power_cut', True) is True, where + ': power_cut is true')
    if 'keys' in action:
        keys = action['keys'] if isinstance(action['keys'], list) else [action['keys']]
        refuse(keys and len(keys) <= 64, where + ': 1..64 key chords')
        for k in keys:
            try:
                chord(k)
            except ValueError as error:
                raise res.Refusal('acceptance case: %s: %s' % (where, error)) from error
    if 'text' in action:
        try:
            text_chords(action['text'])
        except ValueError as error:
            raise res.Refusal('acceptance case: %s: %s' % (where, error)) from error
    if 'media' in action:
        media = action['media']
        refuse(isinstance(media, dict) and set(media) <= {'insert', 'eject'} and len(media) == 1, where + ': media {"insert": <name>} or {"eject": true}')
        refuse(media.get('eject', True) is True, where + ': eject is true')
    if 'screendump' in action:
        refuse(isinstance(action['screendump'], str) and re.fullmatch('[a-z0-9-]{1,40}', action['screendump']), where + ': screendump name [a-z0-9-]')
    for key, low, high in (('pause', 0, 30), ('sleep', 0, 600), ('hold', 0.01, 2)):
        if key in action:
            refuse(type(action[key]) in (int, float) and low <= action[key] <= high, '%s: %s %s..%s seconds' % (where, key, low, high))


def validate_wizard(spec, where):
    refuse(isinstance(spec, dict) and not set(spec) - WIZARD_KEYS, where + ': wizard keys ' + ', '.join(sorted(WIZARD_KEYS)))
    keys = spec.get('keys', [])
    refuse(isinstance(keys, list) and len(keys) <= 64, where + ': wizard keys is a list of up to 64 chords')
    for k in [*keys, spec.get('then', 'ret'), *spec.get('close_keys', [])]:
        try:
            chord(k)
        except ValueError as error:
            raise res.Refusal('acceptance case: %s: wizard: %s' % (where, error)) from error
    refuse(isinstance(spec.get('close_keys', []), list), where + ': wizard close_keys is a list')
    for key, low, high in (('idle', 0.5, 120), ('start_after', 0, 600)):
        if key in spec:
            refuse(type(spec[key]) in (int, float) and low <= spec[key] <= high, '%s: wizard %s %s..%s s' % (where, key, low, high))
    for key in ('max', 'max_closes'):
        if key in spec:
            refuse(type(spec[key]) is int and 0 <= spec[key] <= 64, '%s: wizard %s 0..64' % (where, key))
    for key in ('focus', 'press_when', 'close_when'):
        if key in spec:
            compile_pattern(spec[key], where + ' wizard ' + key)
    refuse(('press_when' not in spec and 'close_when' not in spec) or 'focus' in spec or 'pages' in spec,
           where + ': wizard press_when/close_when need a focus pattern')
    pages = spec.get('pages')
    if pages is not None:
        # fx-acc1: page-driven wizard. Each shown window of the installer's
        # process that has an owner (its wizard pages, its progress window)
        # is a page, named by the first spec whose title expression matches
        # the record's title token (and its modal flag when given).
        refuse(isinstance(pages, list) and 1 <= len(pages) <= 24, where + ': wizard pages is a list of 1..24 page specs')
        refuse(isinstance(spec.get('pid'), str) and re.fullmatch('[a-z][a-z0-9_]{0,31}', spec['pid']),
               where + ': wizard pages need pid, the variable holding the installer process id')
        refuse('press_when' not in spec and 'keys' not in spec and 'then' not in spec and 'max' not in spec,
               where + ': wizard pages replace press_when, keys, then and max')
        names = set()
        for page in pages:
            refuse(isinstance(page, dict) and not set(page) - PAGE_KEYS and isinstance(page.get('name'), str)
                   and re.fullmatch('[a-z0-9-]{1,24}', page['name']) and page['name'] not in names,
                   where + ': wizard page keys ' + ', '.join(sorted(PAGE_KEYS)) + ' with a unique name [a-z0-9-]')
            names.add(page['name'])
            refuse(isinstance(page.get('title'), str) and page['title'], where + ': wizard page %s: title expression' % page['name'])
            try:
                re.compile(page['title'])
            except re.error as error:
                raise res.Refusal('acceptance case: %s: wizard page %s: invalid title expression: %s' % (where, page['name'], error)) from error
            refuse(page.get('modal') in (None, True, False), where + ': wizard page %s: modal is a boolean' % page['name'])
            keys = page.get('keys', [])
            refuse(isinstance(keys, list) and len(keys) <= 8, where + ': wizard page %s: keys is a list of up to 8 chords' % page['name'])
            for k in keys:
                try:
                    chord(k)
                except ValueError as error:
                    raise res.Refusal('acceptance case: %s: wizard page %s: %s' % (where, page['name'], error)) from error
            refuse(type(page.get('timeout', 60)) in (int, float) and 10 <= page.get('timeout', 60) <= 3600,
                   where + ': wizard page %s: timeout 10..3600 s' % page['name'])
        for key, low, high in (('page_timeout', 10, 3600), ('retry', 5, 600)):
            if key in spec:
                refuse(type(spec[key]) in (int, float) and low <= spec[key] <= high, '%s: wizard %s %s..%s s' % (where, key, low, high))
        refuse(type(spec.get('max_retries', 2)) is int and 0 <= spec.get('max_retries', 2) <= 5, where + ': wizard max_retries 0..5')
    region = spec.get('region')
    refuse(region is None or isinstance(region, list) and len(region) == 4 and all(type(v) is int and v >= 0 for v in region),
           where + ': wizard region [x, y, w, h]')


def validate_case(case, root=ROOT):
    if 'guests' in case:
        import lan_runner
        return lan_runner.validate(case, root)
    refuse(case.get('kind') == KIND and case.get('probe') == KIND, 'kind and probe are "acceptance"')
    extra = sorted(set(case) - CASE_KEYS)
    refuse(not extra, 'refused keys: ' + ', '.join(extra))
    refuse(isinstance(case.get('id'), str) and re.fullmatch('[a-z0-9-]{1,48}', case['id']), 'id [a-z0-9-]')
    refuse(type(case.get('item')) is int and 1 <= case['item'] <= 20, 'item is the trial list number')
    refuse(isinstance(case.get('title'), str) and case['title'], 'title')
    refuse(type(case.get('timeout')) is int and 30 <= case['timeout'] <= 7200, 'timeout 30..7200 s')
    refuse(case.get('expected') == {'evidence_kind': KIND}, 'expected is {"evidence_kind": "acceptance"}')
    refuse(type(case.get('blocking', True)) is bool, 'blocking is a boolean')
    refuse(case.get('gate') is None or type(case['gate']) is bool or isinstance(case['gate'], str), 'gate: true or a check name')
    devices = case.get('devices', {})
    refuse(isinstance(devices, dict) and set(devices) <= {'optical', 'network'}, 'devices: optical, network')
    refuse(devices.get('optical') in (None, 'empty'), 'devices.optical is "empty" (media come from steps)')
    refuse(devices.get('network') in NICS, 'devices.network: ' + ', '.join(n for n in NICS if n))
    if 'dos_game' in case:
        spec = case['dos_game']
        refuse(isinstance(spec, dict) and set(spec) == {'title', 'role'} and spec['role'] in ('single', 'host', 'join'), 'dos_game title/role')
        try:
            title, _ = dos_games.title_for(spec['title'], root)
        except (ValueError, OSError) as error:
            raise res.Refusal(str(error)) from error
        refuse(spec['role'] == 'single' or title['lan'] is not None, 'IPX title required')
        recipe = dos_games.acceptance_case(title, spec['role'])
        refuse({k: v for k, v in case.items() if k not in ('_run_id', '_prune_keep', '_lan')} == recipe,
               'DOS title case must use the complete menu/screen/audio/gameplay/exit recipe (dos_games.py write-cases)')
    if '_lan' in case:
        import lan_runner
        lan_runner.validate_network(case['_lan'])
        refuse(devices.get('network') is not None and case.get('dos_game', {}).get('role') in ('host', 'join'), 'LAN network requires paired DOS title')
    media = case.get('media', {})
    refuse(isinstance(media, dict) and len(media) <= 6, 'media: at most 6 generated images')
    for name, spec in media.items():
        refuse(re.fullmatch('[a-z0-9-]{1,16}', name) and isinstance(spec, dict), 'media names [a-z0-9-]')
        refuse(spec.get('generator') == 'xorriso' and set(spec) <= {'generator', 'level', 'joliet', 'volume_sequence', 'label', 'seed', 'content'},
               'media are xorriso images: level, joliet, volume_sequence, label, content')
        # fx-acc1: content "is5" is the InstallShield 5 test medium (iso_fixtures.is5_files).
        refuse(spec.get('content') in (None, 'is5'), 'media content: is5 or the default fixture tree')
        refuse(spec.get('volume_sequence') is None or type(spec['volume_sequence']) is int and 0 <= spec['volume_sequence'] <= 0xFFFF,
               'media volume_sequence 0..65535')
    refuse(not media or devices.get('optical') == 'empty', 'media need devices.optical "empty"')
    refuse(type(case.get('resets', 0)) is int and 0 <= case.get('resets', 0) <= 3, 'resets 0..3')
    patterns(case.get('forbid'), 'forbid')
    pending = case.get('pending')
    if pending is not None:
        refuse(isinstance(pending, dict) and set(pending) <= {'directive', 'requires', 'reason', 'blocking'} and pending.get('reason')
               and isinstance(pending.get('requires', []), list), 'pending: directive, requires (paths), reason')
        refuse(type(pending.get('blocking', False)) is bool, 'pending.blocking: boolean')
    steps = expand_steps(case.get('steps', []))
    refuse(isinstance(case.get('steps', []), list) and (steps or pending), 'steps (or a pending declaration)')
    names, captured, checks = set(), {media_var(n) for n in media}, set()
    reset_armed = shutdown_armed = False
    for index, step in enumerate(steps):
        where = 'step %d' % index
        refuse(isinstance(step, dict) and not set(step) - STEP_KEYS, where + ': keys ' + ', '.join(sorted(STEP_KEYS)))
        name = step.get('name')
        refuse(isinstance(name, str) and re.fullmatch('[a-z0-9-]{1,40}', name) and name not in names, where + ': unique name [a-z0-9-]')
        names.add(name); where = 'step ' + name
        refuse(isinstance(step.get('check', 'main'), str) and re.fullmatch('[a-z0-9-]{1,24}', step.get('check', 'main')),
               where + ': check name [a-z0-9-]')
        checks.add(step.get('check', 'main'))
        waits = patterns(step.get('wait'), where + ' wait')
        fails = patterns(step.get('fail'), where + ' fail')
        refuse(step.get('since', 'cursor') in ('cursor', 'boot', 'case'), where + ': since cursor|boot|case')
        refuse(step.get('wait_event') in (None, *EVENTS), where + ': wait_event RESET|SHUTDOWN')
        refuse(not (step.get('wait') and step.get('wait_event')), where + ': wait or wait_event')
        refuse(step.get('arm') in (None, 'reset', 'shutdown'), where + ': arm reset|shutdown')
        reset_armed |= step.get('arm') == 'reset'
        # sec-user-06: a power cut (QMP system_reset) arms its own reset.
        reset_armed |= any('power_cut' in a for a in [*step.get('actions', []), *step.get('fallback', {}).get('actions', [])])
        shutdown_armed |= step.get('arm') == 'shutdown'
        refuse(step.get('wait_event') != 'RESET' or reset_armed, where + ': wait_event RESET needs an earlier arm reset')
        refuse(step.get('wait_event') != 'SHUTDOWN' or shutdown_armed, where + ': wait_event SHUTDOWN needs an earlier arm shutdown')
        for key, high in (('timeout', 3600), ('settle', 120)):
            if key in step:
                refuse(type(step[key]) in (int, float) and 0 <= step[key] <= high, '%s: %s 0..%s s' % (where, key, high))
        for action in step.get('actions', []):
            validate_action(action, where)
        fallback = step.get('fallback')
        if fallback is not None:
            refuse(isinstance(fallback, dict) and set(fallback) <= {'actions', 'timeout', 'counts_as'} and fallback.get('actions'),
                   where + ': fallback actions, timeout, counts_as')
            refuse(fallback.get('counts_as', 'fail') in ('pass', 'fail'), where + ': fallback counts_as pass|fail')
            refuse(type(fallback.get('timeout', 0)) in (int, float) and 0 <= fallback.get('timeout', 0) <= 3600,
                   where + ': fallback timeout 0..3600 s')
            refuse(step.get('wait') or step.get('wait_event'), where + ': a fallback needs a wait')
            for action in fallback['actions']:
                validate_action(action, where + ' fallback')
        if 'wizard' in step:
            validate_wizard(step['wizard'], where)
            refuse(step.get('wait'), where + ': a wizard needs the wait that ends it')
        settle = step.get('screen_settle')
        refuse(settle is None or isinstance(settle, dict) and set(settle) <= {'stable', 'timeout', 'changed', 'region'},
               where + ': screen_settle stable, timeout, changed, region')
        screen = step.get('screen')
        refuse(screen is None or isinstance(screen, dict) and set(screen) <= {'geometry', 'desktop', 'region', 'sha256'}, where + ': screen geometry, desktop, region, sha256')
        if screen and 'sha256' in screen:
            refuse(isinstance(screen['sha256'], str) and re.fullmatch('[0-9a-f]{64}', screen['sha256']), where + ': screen SHA-256')
            region = screen.get('region')
            refuse(isinstance(region, list) and len(region) == 4 and all(type(v) is int and 0 <= v <= 2048 for v in region) and min(region[2:]) > 0,
                   where + ': bounded nonempty screen region')
        refuse(step.get('on_fail', 'stop') in ('stop', 'continue'), where + ': on_fail stop|continue')
        refuse(type(step.get('required', True)) is bool, where + ': required boolean')
        refuse('known_failure' not in step or isinstance(step['known_failure'], dict) and step['known_failure'].get('directive')
               and step['known_failure'].get('on', 'any') in KNOWN_ON,
               where + ': known_failure {"directive": ..., "on": any|pattern|timeout|screen}')
        for key in ('requires', 'unless'):
            refuse(isinstance(step.get(key, []), list), where + ': %s is a list of step names' % key)
            for need in step.get(key, []):
                refuse(need in names and need != name, where + ': %s names an earlier step: %s' % (key, need))
        if step.get('wizard', {}).get('pid'):
            refuse(step['wizard']['pid'] in captured, where + ': wizard pid <<%s>> is not captured by an earlier step' % step['wizard']['pid'])
        texts = [*waits, *fails, *(step.get('wizard', {}).get(k) for k in ('press_when', 'close_when') if step.get('wizard', {}).get(k))]
        own = set()
        for pattern in waits:
            for match in PLACEHOLDER.finditer(pattern):
                refuse(match[1] in captured or match[1] in own, where + ': <<%s>> is not captured by an earlier wait' % match[1])
            own |= set(GROUP.findall(pattern))
        for pattern in texts[len(waits):]:
            for match in PLACEHOLDER.finditer(pattern):
                refuse(match[1] in captured | own, where + ': <<%s>> is not captured by an earlier wait' % match[1])
        captured |= own
    cuts = sum(1 for s in steps for a in [*s.get('actions', []), *s.get('fallback', {}).get('actions', [])] if 'power_cut' in a)
    refuse(cuts <= case.get('resets', 0), 'each power_cut needs one of the case\'s resets')
    for media_name in (a['media'].get('insert') for s in steps for a in [*s.get('actions', []), *s.get('fallback', {}).get('actions', [])] if 'media' in a):
        refuse(media_name is None or media_name in media, 'media insert names a declared image: ' + str(media_name))
    for key, allowed in (('assertions', ASSERTION_KEYS), ('figures', FIGURE_KEYS)):
        refuse(isinstance(case.get(key, []), list), key + ' is a list')
        seen = set()
        for item in case.get(key, []):
            refuse(isinstance(item, dict) and not set(item) - allowed and isinstance(item.get('name'), str) and item['name'] not in seen,
                   key + ': unique names, keys ' + ', '.join(sorted(allowed)))
            seen.add(item['name'])
            compile_pattern(item.get('pattern'), key + ' ' + item['name'])
            for match in PLACEHOLDER.finditer(item['pattern']):
                refuse(match[1] in captured, '%s %s: <<%s>> is never captured' % (key, item['name'], match[1]))
            for bound in ('from', 'to'):
                refuse(item.get(bound) in (None, 'end', 'start') or item.get(bound) in names, '%s %s: %s names a step' % (key, item['name'], bound))
            if key == 'figures':
                refuse(item.get('reducer', 'lines') in REDUCERS, 'figure reducer: ' + ', '.join(REDUCERS))
                refuse(type(item.get('page', True)) is bool, 'figure %s: page is a boolean' % item['name'])
            else:
                refuse(('min' in item) or ('max' in item), 'assertion %s: min and/or max' % item['name'])
                refuse('known_failure' not in item or isinstance(item['known_failure'], dict) and item['known_failure'].get('directive'),
                       'assertion %s: known_failure {"directive": ...}' % item['name'])
                checks.add(item.get('check', 'main'))
    disk = case.get('disk')
    if disk is not None:
        refuse(isinstance(disk, dict) and set(disk) <= {'check', 'files', 'menu_link_target', 'menu_links', 'required'} and disk.get('files'),
               'disk: check, files, menu_link_target, menu_links, required')
        refuse(type(disk.get('menu_links', True)) is bool and (disk.get('menu_links', True) or 'menu_link_target' not in disk),
               'disk: menu_links is a boolean; false (no Start-menu link expected) excludes menu_link_target')
        for row in disk['files']:
            refuse(isinstance(row, dict) and set(row) <= {'path', 'size', 'sha256'} and isinstance(row.get('path'), str), 'disk.files rows')
        checks.add(disk.get('check', 'disk'))
    items = case.get('items')
    if items is not None:
        refuse(isinstance(items, dict) and items and all(re.fullmatch('[1-9][0-9]?', k) and isinstance(v, list) and v for k, v in items.items()),
               'items maps trial list numbers to check names')
        refuse(str(case['item']) in items, 'items includes the case item')
        for number, names_ in items.items():
            refuse(all(n in checks for n in names_), 'items %s: unknown check' % number)
    if isinstance(case.get('gate'), str):
        refuse(case['gate'] in checks, 'gate names a check')
    return KIND


def selector(case):
    """Acceptance cases are normal boots: no fw_cfg request."""
    return None


def qemu_args(case):
    """Extra QEMU arguments for the declared devices (appended by run.qemu_args)."""
    devices = case.get('devices', {})
    args = []
    if devices.get('network'):
        # The T23's NIC model on a restricted user network: DHCP from slirp,
        # no route to the host or the internet.
        if '_lan' in case:
            import lan_runner
            lan_runner.validate_network(case['_lan'])
            network = case['_lan']
            netdev = 'socket,id=ciuknet0,mcast=%s:%d,localaddr=127.0.0.1' % (network['group'], network['port'])
            mac = network['mac']
        else:
            netdev, mac = 'user,id=ciuknet0,restrict=on', '52:54:00:12:34:56'
        args += ['-netdev', netdev, '-device', devices['network'] + ',netdev=ciuknet0,mac=' + mac + ',romfile=']
    if devices.get('optical') == 'empty':
        # The T23 CD drive is the secondary master (channel 1 unit 0). A
        # backend without a file is an empty drive (no medium); the explicit
        # ide-cd also suppresses QEMU's default CD-ROM, and QMP media actions
        # address the backend by its id.
        args += ['-drive', 'if=none,id=ciuki-optical,readonly=on',
                 '-device', 'ide-cd,drive=ciuki-optical,bus=ide.1,unit=0']
    return args


# ---- timeline ----------------------------------------------------------------

class Timeline:
    """Serial lines of an acceptance boot sequence (run.py parser interface).

    Lines are kept with their host arrival time and boot number; a boot starts
    at each loader banner. A forbidden line is recorded as the failure; the
    runner raises it after a one-second grace that collects what follows."""
    def __init__(self, run_id, probe=KIND, forbid=(), clock=None):
        self.run_id = run_id; self.probe = probe; self.clock = clock
        self.lines = []; self.boot = 0; self.boot_starts = []; self.dropped = 0
        self.records = []; self.terminal = None; self.not_run_subcases = []
        self.forbid = [re.compile(p) for p in (*BASE_FORBID, *forbid)]
        self.failure = None

    def feed(self, raw):
        text = raw.removesuffix(b'\n').removesuffix(b'\r').decode('ascii', 'backslashreplace')
        if BOOT_LINE.match(text):
            self.boot += 1; self.boot_starts.append(len(self.lines))
        row = {'i': len(self.lines), 't': round(self.clock(), 3) if self.clock else None, 'boot': self.boot, 'text': text}
        if len(self.lines) < MAX_LINES:
            self.lines.append(row)
        else:
            self.dropped += 1
        if self.failure is None:
            for pattern in self.forbid:
                if pattern.match(text):
                    self.failure = {'line': row['i'] + 1, 'text': text[:240], 'pattern': pattern.pattern}
                    break
        return None

    def boot_start(self):
        return self.boot_starts[-1] if self.boot_starts else 0

    def find(self, pattern, start, end=None):
        compiled = pattern if hasattr(pattern, 'match') else re.compile(pattern)
        for row in self.lines[start:end]:
            match = compiled.match(row['text'])
            if match:
                return row, match
        return None, None

    def check(self, expected=None):
        if self.failure:
            raise EvidenceError(self.failure_reason())
        return True

    def failure_reason(self):
        f = self.failure
        return 'forbidden serial line %d: %s (pattern %s)' % (f['line'], f['text'], f['pattern'])


# ---- session -----------------------------------------------------------------

class StepFailure(EvidenceError):
    """A step failed: kind is pattern (a `fail` line), timeout, screen or other."""
    def __init__(self, message, kind='other'):
        super().__init__(message)
        self.kind = kind


KNOWN_ON = ('any', 'pattern', 'timeout', 'screen')


def screen_hash(read_ppm, path, region=None):
    width, height, pixels = read_ppm(path)
    x, y, w, h = region or (0, 32, width, max(0, height - 64))
    x, y = min(x, width), min(y, height)
    w, h = min(w, width - x), min(h, height - y)
    digest = hashlib.sha256()
    for row in range(y, y + h):
        digest.update(pixels[(row * width + x) * 3:(row * width + x + w) * 3])
    return digest.hexdigest()


class Session:
    """Drives one acceptance case through run.py's loop (tick per 25 ms turn)."""
    def __init__(self, case, run_id, directory, root=ROOT, read_ppm=None, observe_screen=None):
        self.case = case; self.run_id = run_id; self.directory = Path(directory); self.root = Path(root)
        self.read_ppm = read_ppm; self.observe_screen = observe_screen
        self.parser = Timeline(run_id, forbid=patterns(case.get('forbid'), 'forbid'))
        self.steps = expand_steps(case.get('steps', []))
        self.state = [{'name': s['name'], 'check': s.get('check', 'main'), 'status': 'pending', 'required': s.get('required', True)}
                      for s in self.steps]
        self.index = 0; self.phase = 'start'; self.cursor = 0; self.vars = {}; self.ctx = {}
        self.media = {}; self.events_seen = 0; self.resets = 0; self.reset_armed = False; self.reset_base = None
        self.shutdown_armed = False; self.shutdown = False; self.power_cut = False; self.cut_stamp = None
        self.done = not self.steps; self.failed = None; self.observations = []; self.launched = None
        self.screens = []

    # -- run.py hooks --
    def precheck(self):
        if 'dos_game' in self.case:
            title, _, _ = dos_games.precheck(self.case['dos_game'], self.root)
            role = self.case['dos_game']['role']
            # The placeholder screen cannot pass without a reviewed calibration.
            for step in self.steps:
                if step['name'] == 'gameplay':
                    step['screen'] = {'desktop': False, **title['calibration'][role]}
                if step['name'] == 'enter-game' and role != 'single':
                    step['actions'] = [{'keys': title['lan'][role + '_input'], 'pause': 1}]
        pending = self.case.get('pending')
        if pending:
            missing = [p for p in pending.get('requires', []) if not (self.root / p).exists()]
            if missing or not pending.get('requires') or not self.steps:
                prefix = 'required pending' if pending.get('blocking') else 'pending'
                raise EvidenceNotRun('%s %s: %s%s' % (prefix, pending.get('directive', '-'), pending['reason'],
                                                            ' (missing %s)' % ', '.join(missing) if missing else ''))

    def prepare(self):
        """Generate the case's ISO images in the run directory (never real media)."""
        for name, spec in sorted(self.case.get('media', {}).items()):
            manifest = iso_fixtures.generate({k: v for k, v in spec.items() if k != 'generator'}, self.directory, media_name=name)
            self.media[name] = manifest
            self.vars[media_var(name)] = str(len(manifest['root_entries']))
        return [{'name': n, **{k: m[k] for k in ('path', 'size', 'sha256', 'label', 'joliet', 'level', 'root_entries')},
                 **({'volume_sequence': m['volume_sequence']} if 'volume_sequence' in m else {})} for n, m in sorted(self.media.items())]

    def media_unchanged(self):
        """The generated images are read-only media: their bytes never change."""
        changed = []
        for name, manifest in self.media.items():
            try:
                if iso_fixtures.sha(manifest['path']) != manifest['sha256']:
                    changed.append(name)
            except OSError:
                changed.append(name)
        return changed

    def pending(self):
        if self.index >= len(self.steps):
            return None
        step = self.steps[self.index]
        return {'step': step['name'], 'phase': self.phase, 'wait': step.get('wait'), 'wait_event': step.get('wait_event'),
                'wait_index': self.ctx.get('wait_index'), 'cursor_line': self.cursor + 1, 'lines': len(self.parser.lines)}

    def substitute(self, pattern, strict=True):
        missing = []
        def value(match):
            if match[1] not in self.vars:
                missing.append(match[1]); return 'x'
            return re.escape(self.vars[match[1]])
        compiled = re.compile(PLACEHOLDER.sub(value, pattern))
        if missing:
            if strict:
                raise StepFailure('pattern uses <<%s>> before it was captured' % missing[0])
            return None
        return compiled

    def events(self, events):
        """QMP guest events: a RESET or SHUTDOWN is legal only once a step armed it."""
        for event in events[self.events_seen:]:
            self.events_seen += 1
            name, data = event.get('event'), event.get('data', {})
            reason = str(data.get('reason'))
            stamp = event.get('timestamp', {})
            stamp = stamp.get('seconds', 0) + stamp.get('microseconds', 0) / 1e6
            if name == 'RESET' and reason == 'guest-reset' and self.cut_stamp is not None and 0 <= stamp - self.cut_stamp <= 3:
                # SeaBIOS's own hard reboot right after a host system_reset
                # (run.py unexpected_resets): part of the power cut.
                self.cut_stamp = None
                self.observations.append({'event': 'RESET', 'reason': reason, 'line': len(self.parser.lines), 'after_power_cut': True})
                continue
            if name == 'RESET':
                host_cut = self.power_cut and reason.startswith('host')
                if not reason.startswith('guest') and not host_cut:
                    raise StepFailure('unexpected reset (%s)' % reason)
                self.power_cut = False
                if host_cut:
                    self.cut_stamp = stamp
                if not self.reset_armed or self.resets >= self.case.get('resets', 0):
                    raise StepFailure('unexpected guest reset (%s)' % reason)
                self.resets += 1; self.reset_armed = False
                self.observations.append({'event': 'RESET', 'reason': reason, 'line': len(self.parser.lines)})
            elif name == 'SHUTDOWN':
                if reason == 'guest-shutdown' and self.shutdown_armed:
                    self.shutdown = True
                    self.observations.append({'event': 'SHUTDOWN', 'reason': reason, 'line': len(self.parser.lines)})
                else:
                    raise StepFailure('unexpected shutdown (%s)' % reason)

    def tick(self, now, qmp):
        if self.done or self.failed:
            return
        if self.launched is None:
            self.launched = now
        try:
            self.events(qmp.events)
        except StepFailure as failure:
            # An unexpected reset or power-off ends the case whatever the step allows.
            self.abandon(str(failure))
            self.failed = self.failed or str(failure)
            return
        try:
            while not self.done and not self.failed and self._advance(now, qmp):
                pass
        except StepFailure as failure:
            self._fail_step(now, qmp, str(failure), failure.kind)

    # -- step machine --
    def _record(self, **fields):
        self.state[self.index].update(fields)

    def _rel(self, now):
        return round(now - self.launched, 3)

    def _window_start(self, step):
        since = step.get('since', 'cursor')
        return 0 if since == 'case' else self.parser.boot_start() if since == 'boot' else self.cursor

    def _next(self):
        # A step's window ends with the lines received until it finished.
        self._record(last_line=max(len(self.parser.lines), self.state[self.index].get('first_line', 0)))
        self.index += 1; self.phase = 'start'; self.ctx = {}
        if self.index >= len(self.steps):
            self.done = True

    def _advance(self, now, qmp):
        """One state transition; True when another may follow in this turn."""
        step = self.steps[self.index]; phase = self.phase; ctx = self.ctx
        if phase == 'start':
            unmet = [n for n in step.get('requires', []) if self._status(n) != 'pass']
            if unmet:
                self._record(status='not_run', reason='requires ' + ', '.join(unmet), first_line=self.cursor)
                self._next(); return True
            passed = [n for n in step.get('unless', []) if self._status(n) == 'pass']
            if passed:
                # A fallback path for an earlier step: not needed, not counted.
                self._record(status='skipped', reason='unless %s passed' % ', '.join(passed), first_line=self.cursor)
                self._next(); return True
            start = self._window_start(step)
            self.ctx = ctx = {'deadline': now + step.get('timeout', DEFAULT_TIMEOUT), 'wait_index': 0, 'wait_from': start,
                              'fail_from': start, 'scan': {}, 'queue': [], 'due': now, 'fallback': False}
            self._record(started=self._rel(now), first_line=start)
            if step.get('arm') == 'reset':
                self.reset_armed = True; self.reset_base = self.resets
            elif step.get('arm') == 'shutdown':
                self.shutdown_armed = True
            self.phase = 'wait'
            return True
        if phase == 'wait':
            if not self._waited(step, now, qmp):
                return False
            self.phase = 'settle'; ctx['due'] = now + step.get('settle', 0)
            return True
        if phase == 'fallback':
            if ctx['queue']:
                if now < ctx['due']:
                    return False
                ctx['due'] = now + self._perform(ctx['queue'].pop(0), qmp, now)
                return False
            if now < ctx['due']:
                return False
            self.phase = 'wait'
            return True
        if phase == 'settle':
            if now < ctx['due']:
                return False
            ctx['queue'] = self._expand(step.get('actions', []))
            if ctx['queue']:
                self.cursor = max(self.cursor, len(self.parser.lines))
                if step.get('screen_settle', {}).get('changed'):
                    ctx['baseline'] = self._pace_hash(qmp, step['screen_settle'].get('region'))
            self.phase = 'actions'; ctx['due'] = now
            return True
        if phase == 'actions':
            if ctx['queue']:
                if now < ctx['due']:
                    return False
                ctx['due'] = now + self._perform(ctx['queue'].pop(0), qmp, now)
                return bool(ctx['queue']) and now >= ctx['due']
            if now < ctx['due']:
                return False
            if step.get('screen_settle'):
                spec = step['screen_settle']
                ctx['screen'] = {'deadline': now + spec.get('timeout', 20), 'next': now, 'last': None, 'stable_since': None}
                self.phase = 'screen_settle'
            else:
                self.phase = 'screen'
            return True
        if phase == 'screen_settle':
            if not self._settled(step['screen_settle'], qmp, now):
                return False
            self.phase = 'screen'
            return True
        if phase == 'screen':
            if step.get('screen'):
                self._check_screen(step, qmp)
            status = 'fail' if ctx.get('fallback') and step.get('fallback', {}).get('counts_as', 'fail') == 'fail' else 'pass'
            self._record(status=status, finished=self._rel(now), **({'fallback_used': True} if ctx.get('fallback') else {}))
            if status == 'fail':
                self._record(reason='passed only through its fallback')
                if step.get('required', True) and step.get('on_fail', 'stop') == 'stop':
                    self._stop('step %s passed only through its fallback' % step['name'])
                    return False
            self._next()
            return True
        raise AssertionError(phase)

    def _status(self, name):
        return next((s['status'] for s in self.state if s['name'] == name), None)

    def _scan(self, key, compiled, start):
        """First match of compiled at or after start, scanning only new lines."""
        ctx = self.ctx
        at = max(start, ctx['scan'].get(key, start))
        lines = self.parser.lines
        for row in lines[at:]:
            match = compiled.match(row['text'])
            if match:
                ctx['scan'][key] = row['i']
                return row, match
        ctx['scan'][key] = len(lines)
        return None, None

    def _waited(self, step, now, qmp):
        ctx = self.ctx
        if step.get('wait_event'):
            done = self.shutdown if step['wait_event'] == 'SHUTDOWN' else \
                self.reset_base is not None and self.resets > self.reset_base
            if done:
                # The cursor stays: the next boot's lines may already have
                # arrived before the QMP event was read.
                self._record(event=step['wait_event'])
                return True
            return self._timeout(step, now, qmp)
        for index, pattern in enumerate(patterns(step.get('fail'), 'fail')):
            compiled = self.substitute(pattern, strict=False)
            if compiled is None:
                continue
            row, _ = self._scan(('fail', index), compiled, ctx['fail_from'])
            if row:
                raise StepFailure('step %s: failure line %d: %s' % (step['name'], row['i'] + 1, row['text'][:200]), 'pattern')
        waits = patterns(step.get('wait'), 'wait')
        while ctx['wait_index'] < len(waits):
            compiled = self.substitute(waits[ctx['wait_index']])
            row, match = self._scan(('wait', ctx['wait_index']), compiled, ctx['wait_from'])
            if not row:
                if step.get('wizard'):
                    self._wizard(step, now, qmp)
                return self._timeout(step, now, qmp)
            self.vars.update({k: v for k, v in match.groupdict().items() if v is not None})
            self.state[self.index].setdefault('matched', []).append({'line': row['i'] + 1, 'text': row['text'][:200], 't': row['t']})
            ctx['wait_from'] = row['i'] + 1; self.cursor = max(self.cursor, row['i'] + 1); ctx['wait_index'] += 1
        return True

    def _timeout(self, step, now, qmp):
        ctx = self.ctx
        if now < ctx['deadline']:
            return False
        fallback = step.get('fallback')
        if fallback and not ctx['fallback']:
            ctx['fallback'] = True
            self._record(fallback_started=self._rel(now))
            ctx['queue'] = self._expand(fallback['actions'])
            self.cursor = max(self.cursor, len(self.parser.lines))
            ctx['deadline'] = now + fallback.get('timeout', step.get('timeout', DEFAULT_TIMEOUT))
            ctx['due'] = now
            self.phase = 'fallback'
            return False
        waits = patterns(step.get('wait'), 'wait')
        what = step.get('wait_event') or (waits[ctx['wait_index']] if ctx['wait_index'] < len(waits) else '?')
        raise StepFailure('step %s: timeout after %.0f s waiting for %s' % (
            step['name'], step.get('timeout', DEFAULT_TIMEOUT) + (fallback.get('timeout', step.get('timeout', DEFAULT_TIMEOUT)) if fallback else 0), what),
            'timeout')

    def _expand(self, actions):
        out = []
        for action in actions:
            pause = action.get('pause', DEFAULT_PAUSE)
            if 'keys' in action:
                keys = action['keys'] if isinstance(action['keys'], list) else [action['keys']]
                out += [{'chord': chord(k), 'pause': pause, 'hold': action.get('hold', KEY_HOLD)} for k in keys]
            elif 'text' in action:
                out += [{'chord': c, 'pause': action.get('pause', TEXT_PAUSE), 'hold': action.get('hold', KEY_HOLD), 'typed': True}
                        for c in text_chords(action['text'])]
            elif 'media' in action:
                out.append({'media': action['media'], 'pause': pause})
            elif 'mouse' in action:
                out.append({'mouse': action['mouse'], 'pause': pause, 'hold': action.get('hold', KEY_HOLD)})
            elif 'screendump' in action:
                out.append({'screendump': action['screendump'], 'pause': pause})
            elif 'sleep' in action:
                out.append({'sleep': action['sleep']})
            elif 'power_cut' in action:
                out.append({'power_cut': True, 'pause': pause})
        return out

    def _press(self, keys, qmp):
        qmp.input_events([key_event(k, True) for k in keys])
        time.sleep(KEY_HOLD)
        qmp.input_events([key_event(k, False) for k in reversed(keys)])

    def _perform(self, item, qmp, now):
        """Send one queued stimulus; returns the pause before the next one."""
        log = self.state[self.index].setdefault('stimuli', [])
        if 'chord' in item:
            qmp.input_events([key_event(k, True) for k in item['chord']])
            time.sleep(item['hold'])
            qmp.input_events([key_event(k, False) for k in reversed(item['chord'])])
            if item.get('typed') and log and log[-1].get('typed') is not None:
                log[-1]['typed'] += 1
            elif len(log) < 200:
                log.append({'t': self._rel(now), 'keys': '+'.join(item['chord'])} if not item.get('typed') else
                           {'t': self._rel(now), 'typed': 1})
            return item['pause']
        if 'media' in item:
            if 'eject' in item['media']:
                command, args = 'eject', {'device': 'ciuki-optical', 'force': True}
            else:
                image = self.media[item['media']['insert']]['path']
                command, args = 'blockdev-change-medium', {'device': 'ciuki-optical', 'filename': image,
                                                           'format': 'raw', 'read-only-mode': 'read-only'}
            qmp.command(command, args)
            log.append({'t': self._rel(now), 'qmp': command, **({'media': item['media']['insert']} if 'insert' in item['media'] else {})})
            return item['pause']
        if 'mouse' in item:
            mouse = item['mouse']
            events = [{'type': 'rel', 'data': {'axis': axis, 'value': mouse[key]}}
                      for axis, key in (('x', 'dx'), ('y', 'dy')) if mouse.get(key)]
            if events:
                qmp.input_events(events)
            if mouse.get('button'):
                qmp.input_events([{'type': 'btn', 'data': {'button': mouse['button'], 'down': True}}])
                time.sleep(item['hold'])
                qmp.input_events([{'type': 'btn', 'data': {'button': mouse['button'], 'down': False}}])
            log.append({'t': self._rel(now), 'mouse': mouse})
            return item['pause']
        if 'screendump' in item:
            path = self._screendump(qmp, item['screendump'])
            log.append({'t': self._rel(now), 'screendump': path.name})
            return item['pause']
        if 'sleep' in item:
            log.append({'t': self._rel(now), 'sleep': item['sleep']})
            return item['sleep']
        if 'power_cut' in item:
            # The cut arms its own reset (counted against the case's resets).
            self.power_cut = True; self.reset_armed = True; self.reset_base = self.resets
            qmp.command('system_reset', {})
            log.append({'t': self._rel(now), 'qmp': 'system_reset'})
            return item['pause']
        raise AssertionError(item)

    def _screendump(self, qmp, name):
        path = self.directory / ('screen-%02d-%s.ppm' % (self.index, name))
        qmp.command('screendump', {'filename': str(path)})
        if path.name not in self.screens:
            self.screens.append(path.name)
        return path

    def _pace_hash(self, qmp, region=None):
        """Pacing only: a hash of the screen (the work area by default); None
        when the dump cannot be read. The file is deleted at once."""
        path = self.directory / PACE_FILE
        try:
            qmp.command('screendump', {'filename': str(path)})
            return screen_hash(self.read_ppm, path, region)
        except (OSError, ValueError, IndexError):
            return None
        finally:
            path.unlink(missing_ok=True)

    def _settled(self, spec, qmp, now):
        """The screen differs from the pre-action baseline (when `changed`)
        and has not changed for `stable` seconds, or the bound passed."""
        s = self.ctx['screen']
        if now >= s['deadline']:
            self._record(screen_settle='timeout')
            return True
        if now < s['next']:
            return False
        s['next'] = now + SCREEN_POLL
        current = self._pace_hash(qmp, spec.get('region'))
        if current is None:
            return False
        if spec.get('changed') and current == self.ctx.get('baseline'):
            s['stable_since'] = None
            return False
        if current != s['last']:
            s['last'] = current; s['stable_since'] = now
            return False
        if now - s['stable_since'] >= spec.get('stable', 2.0):
            self._record(screen_settle='stable')
            return True
        return False

    def _still(self, w, spec, now, qmp):
        """Pacing: True once the screen has not changed for `idle` seconds
        since its last change and the wizard's last action."""
        if now < w['next']:
            return False
        w['next'] = now + WIZARD_POLL
        current = self._pace_hash(qmp, spec.get('region'))
        if current is None:
            return False
        if current != w['hash']:
            w['hash'] = current; w['last_change'] = now
            return False
        return now - max(w['last_change'], w['last_action']) >= spec.get('idle', 6)

    @staticmethod
    def _page_spec(spec, window):
        for page in spec['pages']:
            if re.search(page['title'], window['title']) and page.get('modal') in (None, window['modal']):
                return page
        return None

    def _wizard_pages(self, step, now, qmp):
        """fx-acc1: drive an installer page by page. A page is a shown
        window of the installer's process with an owner (its wizard pages and
        progress window; the frame and helper windows have none), the newest
        one shown; its title token names it through the first matching page
        spec. Once the screen is still and the installer's process holds the
        focus, the page's keys are pressed once, again after `retry` seconds
        up to `max_retries` times; a page that stays longer than its timeout
        fails the step naming that page. Files windows the installer opens
        over its last page are closed through close_when as before."""
        spec = step['wizard']; ctx = self.ctx
        w = ctx.setdefault('wizard', {'closes': 0, 'last_action': now, 'last_change': now, 'hash': None,
                                      'next': now + spec.get('start_after', 0), 'focus': None, 'focus_pid': None,
                                      'scanned': self.state[self.index].get('first_line', self.cursor),
                                      'windows': {}, 'page': None, 'since': now, 'presses': 0, 'last_press': None,
                                      'previous': None})
        pid = self.vars.get(spec['pid'])
        close = spec.get('close_when') and self.substitute(spec['close_when'], strict=False)
        lines = self.parser.lines
        for row in lines[w['scanned']:]:
            match = WINDOW_RECORD.match(row['text'])
            if match and match['pid'] == pid:
                if match['state'] == 'closed':
                    w['windows'].pop(match['window'], None)
                else:
                    w['windows'][match['window']] = {'window': match['window'], 'owner': match['owner'], 'modal': match['modal'] == '1',
                                                     'state': match['state'], 'title': match['title'], 'line': row['i']}
                continue
            match = FOCUS_RECORD.match(row['text'])
            if match:
                w['focus'] = row['text']; w['focus_pid'] = match['pid']
        w['scanned'] = len(lines)
        shown = [v for v in w['windows'].values() if v['state'] == 'shown' and v['owner'] != '0']
        window = max(shown, key=lambda v: v['line']) if shown else None
        page = self._page_spec(spec, window) if window else None
        identity = (window['window'], page['name'] if page else None) if window else None
        log = self.state[self.index].setdefault('pages', [])
        if identity != w['page']:
            if w['page'] is not None and w['page'][0] is not None:
                w['previous'] = w['page_label']
            w['page'] = identity; w['since'] = now; w['presses'] = 0; w['last_press'] = None
            w['page_label'] = ('%s (title %s, window %s)' % (page['name'] if page else 'unknown page', window['title'], window['window'])
                               if window else None)
            if window and len(log) < 64:
                log.append({'t': self._rel(now), 'page': page['name'] if page else None, 'title': window['title'],
                            'window': int(window['window']), 'modal': window['modal']})
        limit = (page or {}).get('timeout', spec.get('page_timeout', 300)) if window else \
            spec.get('page_timeout', 300) + (spec.get('start_after', 0) if w['previous'] is None else 0)
        if now - w['since'] > limit:
            if window:
                what = 'wizard stopped on page %s' % w['page_label']
            elif w['previous']:
                what = 'wizard: no installer page shown after page %s' % w['previous']
            else:
                what = 'wizard: no installer page shown'
            raise StepFailure('step %s: %s for %.0f s' % (step['name'], what, now - w['since']), 'timeout')
        if not self._still(w, spec, now, qmp):
            return
        text = w['focus'] or ''
        actions = self.state[self.index].setdefault('wizard', [])
        if close and close.match(text) and w['closes'] < spec.get('max_closes', 8):
            keys, kind = spec.get('close_keys', ['alt+f4']), 'close'
            w['closes'] += 1
        elif page and page.get('keys') and w['focus_pid'] == pid and \
                (w['presses'] == 0 or (w['presses'] <= spec.get('max_retries', 2) and now - w['last_press'] >= spec.get('retry', 45))):
            keys, kind = page['keys'], 'press'
            w['presses'] += 1; w['last_press'] = now
        else:
            return
        for k in keys:
            self._press(chord(k), qmp)
        w['last_action'] = now
        if len(actions) < 64:
            actions.append({'t': self._rel(now), 'kind': kind, 'keys': keys, 'focus': text[:160],
                            'page': page['name'] if page and kind == 'press' else None, 'idle': round(now - w['last_change'], 1)})

    def _wizard(self, step, now, qmp):
        """Press the installer's next key once the screen has stayed still."""
        if step['wizard'].get('pages'):
            return self._wizard_pages(step, now, qmp)
        spec = step['wizard']; ctx = self.ctx
        w = ctx.setdefault('wizard', {'presses': 0, 'closes': 0, 'last_action': now, 'last_change': now,
                                      'hash': None, 'next': now + spec.get('start_after', 0), 'focus': None, 'scanned': 0})
        if spec.get('focus'):
            focus = self.substitute(spec['focus'], strict=False)
            if focus is not None:
                for row in self.parser.lines[w['scanned']:]:
                    if focus.match(row['text']):
                        w['focus'] = row['text']
                w['scanned'] = len(self.parser.lines)
        if now < w['next']:
            return
        w['next'] = now + WIZARD_POLL
        current = self._pace_hash(qmp, spec.get('region'))
        if current is None:
            return
        if current != w['hash']:
            w['hash'] = current; w['last_change'] = now
            return
        if now - max(w['last_change'], w['last_action']) < spec.get('idle', 6):
            return
        text = w['focus'] or ''
        close = spec.get('close_when') and self.substitute(spec['close_when'], strict=False)
        press = spec.get('press_when') and self.substitute(spec['press_when'], strict=False)
        log = self.state[self.index].setdefault('wizard', [])
        if close and close.match(text) and w['closes'] < spec.get('max_closes', 8):
            keys, kind = spec.get('close_keys', ['alt+f4']), 'close'
            w['closes'] += 1
        elif (not spec.get('press_when') or (press and press.match(text))) and w['presses'] < spec.get('max', 12):
            keys = [spec['keys'][w['presses']] if w['presses'] < len(spec.get('keys', [])) else spec.get('then', 'ret')]
            kind = 'press'; w['presses'] += 1
        else:
            return
        for k in keys:
            self._press(chord(k), qmp)
        w['last_action'] = now
        if len(log) < 64:
            log.append({'t': self._rel(now), 'kind': kind, 'keys': keys, 'focus': text[:160],
                        'idle': round(now - w['last_change'], 1)})

    def _check_screen(self, step, qmp):
        path = self._screendump(qmp, step['name'])
        spec = step['screen']
        if spec.get('desktop', True) and self.observe_screen:
            observed = self.observe_screen(path, {'geometry': spec.get('geometry')})
        else:
            width, height, _ = self.read_ppm(path)
            observed = {'width': width, 'height': height, 'failures': []}
            if spec.get('geometry') and [width, height] != list(spec['geometry']):
                observed['failures'].append('geometry %dx%d differs from %dx%d' % (width, height, *spec['geometry']))
        self._record(screen={k: v for k, v in observed.items() if k in ('width', 'height', 'failures', 'regions', 'ppm_sha256')},
                     screen_file=path.name)
        if observed['failures']:
            raise StepFailure('step %s: screen check failed: %s' % (step['name'], ', '.join(observed['failures'])), 'screen')
        if 'sha256' in spec:
            width, height, _ = self.read_ppm(path)
            x, y, w, h = spec['region']
            if x + w > width or y + h > height:
                raise StepFailure('gameplay region outside screen', 'screen')
            actual = screen_hash(self.read_ppm, path, spec['region'])
            self._record(gameplay_sha256=actual, expected_sha256=spec['sha256'])
            if actual != spec['sha256']:
                raise StepFailure('gameplay frame does not match reviewed checkpoint', 'screen')

    def _fail_step(self, now, qmp, reason, kind='other'):
        """A blocking failure stops the script unless on_fail is continue; a
        known failure (known_failure, matching its `on` kind) and a failure
        of a step that is not required are recorded and the script goes on."""
        if self.index >= len(self.steps):
            self._stop(reason)
            return
        step = self.steps[self.index]
        self._record(status='fail', reason=reason, failure_kind=kind, finished=self._rel(now))
        try:
            self._screendump(qmp, 'fail')
        except (OSError, RuntimeError, ValueError, TimeoutError):
            pass
        known = step.get('known_failure')
        if known and known.get('on', 'any') in ('any', kind):
            self._record(status='known_fail', known_failure=known)
        elif step.get('required', True) and step.get('on_fail', 'stop') == 'stop':
            self._stop(reason)
            return
        self._next()

    def _stop(self, reason):
        self.failed = reason
        for row in self.state[self.index + 1:]:
            if row['status'] == 'pending':
                row.update(status='not_run', reason='an earlier step failed')

    def abandon(self, reason):
        """The run ended (deadline, forbidden line, QEMU exit) inside a step."""
        if self.index < len(self.steps) and self.state[self.index]['status'] == 'pending':
            self.state[self.index].update(status='fail', reason=reason)
            self._stop(reason)

    # -- after the run --
    def window(self, bound, end):
        """A timeline index: `from` a step is where its wait began, `to` a step
        is where it ended (its last match or the start of its actions); None
        when that step never ran."""
        if bound in (None, 'start', 'end'):
            return len(self.parser.lines) if bound == 'end' or (bound is None and end) else 0
        row = next((s for s in self.state if s['name'] == bound), None)
        if not row or row['status'] == 'not_run' or 'first_line' not in row:
            return None
        return row.get('last_line', len(self.parser.lines)) if end else row['first_line']

    def evaluate(self):
        """Assertions and figures over the recorded timeline."""
        out = {'assertions': [], 'figures': {}}
        for item in self.case.get('assertions', []):
            start, end = self.window(item.get('from'), False), self.window(item.get('to'), True)
            row = {'name': item['name'], 'check': item.get('check', 'main'), 'required': item.get('required', True)}
            if item.get('known_failure'):
                row['known_failure'] = item['known_failure']
            pattern = self.substitute(item['pattern'], strict=False)
            if start is None or end is None or pattern is None:
                row.update(status='not_run', reason='window step did not run' if pattern is not None else 'placeholder never captured')
                out['assertions'].append(row); continue
            hits = [r for r in self.parser.lines[start:end] if pattern.match(r['text'])]
            ok = len(hits) >= item.get('min', 0) and ('max' not in item or len(hits) <= item['max'])
            row.update(status='pass' if ok else ('known_fail' if item.get('known_failure') else 'fail'), count=len(hits),
                       window=[start + 1, end], first=[{'line': h['i'] + 1, 'text': h['text'][:200]} for h in hits[:3]])
            out['assertions'].append(row)
        for item in self.case.get('figures', []):
            start, end = self.window(item.get('from'), False), self.window(item.get('to'), True)
            pattern = self.substitute(item['pattern'], strict=False)
            if start is None or end is None or pattern is None:
                out['figures'][item['name']] = {'check': item.get('check', 'main'), 'status': 'not_run'}; continue
            hits = [r for r in self.parser.lines[start:end] if pattern.match(r['text'])]
            value = REDUCER_FUNCTIONS[item.get('reducer', 'lines')](hits)
            out['figures'][item['name']] = {'check': item.get('check', 'main'), 'window': [start + 1, end], **value,
                                            **({'page': False} if item.get('page') is False else {})}
        return out

    def finish(self, result, disk=None):
        evaluation = self.evaluate()
        checks = {}
        rank = {'pass': 0, 'not_run': 1, 'known_fail': 2, 'fail': 3}
        def note(check, status, required, known):
            if status == 'skipped':
                return
            row = checks.setdefault(check, {'status': 'pass', 'items': 0})
            row['items'] += 1
            effective = status if required or status in ('pass', 'not_run', 'known_fail') else 'fail_optional'
            if effective == 'fail_optional':
                row['optional_failures'] = row.get('optional_failures', 0) + 1; return
            if rank.get(effective, 3) > rank[row['status']]:
                row['status'] = effective
        for state in self.state:
            note(state['check'], state['status'], state['required'], state['status'] == 'known_fail')
        for row in evaluation['assertions']:
            note(row['check'], row['status'], row['required'], 'known_failure' in row)
        if disk is not None:
            note(disk['check'], disk['status'], disk['required'], False)
        failing = sorted(c for c, r in checks.items() if r['status'] == 'fail')
        summary = {'item': self.case['item'], 'title': self.case['title'], 'blocking': self.case.get('blocking', True),
                   'steps': self.state, 'checks': checks, 'variables': self.vars, 'observations': self.observations,
                   'media': {n: {'path': Path(m['path']).name, 'root_entries': m['root_entries'], 'label': m['label'],
                                 'joliet': m['joliet'], **({'volume_sequence': m['volume_sequence']} if 'volume_sequence' in m else {})}
                             for n, m in self.media.items()},
                   'boots': self.parser.boot, 'serial_lines': len(self.parser.lines), 'dropped_lines': self.parser.dropped,
                   'kept_screens': self.screens, **evaluation}
        if self.case.get('items'):
            summary['items'] = {number: {'checks': names, 'status': max((checks.get(n, {'status': 'not_run'})['status'] for n in names),
                                                                        key=lambda s: rank.get(s, 3))}
                                for number, names in sorted(self.case['items'].items(), key=lambda kv: int(kv[0]))}
        if disk is not None:
            summary['disk'] = disk
        if self.failed:
            summary['stopped'] = self.failed
        changed = self.media_unchanged()
        if changed:
            summary['media_changed'] = changed
        result['acceptance'] = summary
        if result.get('outcome') == 'pass' and (failing or changed or self.failed):
            result['outcome'] = 'fail'
            result['reason'] = ('acceptance checks failed: ' + ', '.join(failing)) if failing else \
                ('generated media changed: ' + ', '.join(changed)) if changed else 'acceptance stopped: ' + str(self.failed)
        return summary


# ---- reducers ------------------------------------------------------------------

FIELD = re.compile(r'([a-z_]+)=([^ ]+)')


def fields(text):
    return dict(FIELD.findall(text))


def reduce_dosvm_perf(hits):
    """[dosvm] perf / perf_port / perf_cost lines (dosvm_perf.inc perf_periodic,
    every 2 s while the VM runs): the window counters the T23 logs use."""
    samples, ports, costs = [], {}, {}
    for row in hits:
        text = row['text']; f = fields(text)
        try:
            if text.startswith('[dosvm] perf vm='):
                samples.append({k: int(f[k]) for k in ('exits', 'gp', 'pf', 'db', 'timer', 'pm', 'busy_pm') if k in f})
            elif text.startswith('[dosvm] perf_port '):
                p = ports.setdefault(f['port'], [0, 0]); p[0] += int(f['exits']); p[1] += int(f['us'])
            elif text.startswith('[dosvm] perf_cost '):
                c = costs.setdefault(f['cause'], [0, 0]); c[0] += int(f['exits']); c[1] += int(f['us'])
        except (KeyError, ValueError):
            continue
    def stat(key, how):
        values = [s[key] for s in samples if key in s]
        return how(values) if values else None
    return {'samples': len(samples),
            'exits_median': stat('exits', statistics.median), 'exits_max': stat('exits', max),
            'busy_pm_median': stat('busy_pm', statistics.median), 'busy_pm_max': stat('busy_pm', max),
            'active_samples': sum(1 for s in samples if s.get('exits', 0) > 0),
            'top_ports': [{'port': p, 'exits': v[0], 'us': v[1]} for p, v in sorted(ports.items(), key=lambda x: -x[1][1])[:5]],
            'top_costs': [{'cause': c, 'exits': v[0], 'us': v[1]} for c, v in sorted(costs.items(), key=lambda x: -x[1][1])[:5]],
            'first_line': hits[0]['i'] + 1 if hits else None, 'last_line': hits[-1]['i'] + 1 if hits else None}


REDUCER_FUNCTIONS = {
    'dosvm_perf': reduce_dosvm_perf,
    'lines': lambda hits: {'count': len(hits), 'lines': [{'line': h['i'] + 1, 'text': h['text'][:200]} for h in hits[:20]]},
    'count': lambda hits: {'count': len(hits)},
    'first': lambda hits: {'count': len(hits), 'line': hits[0]['i'] + 1 if hits else None, 'text': hits[0]['text'][:200] if hits else None},
    'last': lambda hits: {'count': len(hits), 'line': hits[-1]['i'] + 1 if hits else None, 'text': hits[-1]['text'][:200] if hits else None},
}


# ---- disk check ------------------------------------------------------------------

def disk_check(spec, delta):
    """Independent overlay evidence (win32_inventory.delta): installed files and
    a Start-menu link, compared case-insensitively."""
    if delta.get('error'):
        return {'check': spec.get('check', 'disk'), 'required': spec.get('required', True), 'status': 'fail',
                'error': delta['error'], 'files': [], 'menu_links': [], 'link_targets': [], 'menu_link_ok': False}
    written = {r['path'].replace('/', '\\').casefold(): r for r in [*delta.get('added', []), *delta.get('changed', [])]
               if r.get('kind') == 'file'}
    rows, ok = [], True
    for want in spec['files']:
        got = written.get(want['path'].replace('/', '\\').casefold())
        good = bool(got) and all(got.get(k) == want[k] for k in ('size', 'sha256') if k in want)
        rows.append({'path': want['path'], 'present': bool(got), 'size': got and got.get('size'), 'match': good})
        ok = ok and good
    links = delta.get('menu_links', [])
    targets = [l for l in delta.get('links', []) if l['path'] in links]
    wanted = spec.get('menu_link_target')
    link_ok = bool(links) and (not wanted or any(wanted.casefold() in (l.get('target') or '').casefold() or l.get('target') == '<foreign>'
                                                  for l in targets))
    if spec.get('menu_links') is False:
        link_ok = True   # fx-acc1: a case that installs no Start-menu link (the IS5 test engine)
    return {'check': spec.get('check', 'disk'), 'required': spec.get('required', True),
            'status': 'pass' if ok and link_ok else 'fail', 'files': rows, 'menu_links': links[:20],
            'link_targets': targets[:20], 'menu_link_ok': link_ok}


# ---- NetSurf site cases (f6:browser probe cases with browser_sites) ----------------

def browser_sites_report(result, serial_text):
    """Fetch and layout records of an f6:browser case served from site snapshots.

    Same-site transport failures (a `[netsurf] fetch url=https://fixture.test/...
    stage=error` line) fail the case; other hosts (fonts, analytics) cannot be
    reached from the restricted user network and are recorded only. HTTP 404s
    are paths the snapshot does not hold (recorded, not a CiukiOS failure)."""
    requests = result.get('browser_fixture', {}).get('requests', [])
    site = [r for r in requests if r.get('site')]
    fetch = re.compile(r'^(?:\[console\] )?\[netsurf\] fetch url=(\S+) stage=(\S+) code=([0-9]+)')
    errors, others, layout, done = [], [], [], 0
    for line in serial_text.splitlines():
        line = line.rstrip('\r')
        match = fetch.match(line)
        if match and match[2] == 'error':
            (errors if '//fixture.test/' in match[1] else others).append(line[:200])
        elif match and match[2] == 'done':
            done += 1
        if re.match(r'^(?:\[console\] )?\[netsurf\] (?:layout slow|exit reason=)', line):
            layout.append(line[:200])
    report = {'site_requests': len(site), 'served': sum(1 for r in site if r.get('status') == 200),
              'missing_from_snapshot': sorted({r['path'] for r in site if r.get('status') == 404})[:40],
              'aliases': sorted({'%s->%s' % (r['path'], r['alias']) for r in site if r.get('alias')}),
              'fetches_done': done, 'fixture_fetch_errors': errors[:20], 'other_host_fetch_errors': len(others),
              'other_host_examples': others[:5], 'layout_records': layout[:20],
              'layout_rejections': sum(1 for l in layout if 'accepted=0' in l)}
    result['browser_sites'] = report
    if errors and result.get('outcome') == 'pass':
        result['outcome'] = 'fail'; result['reason'] = 'NetSurf fetch errors on fixture-served site resources'
    return report


# ---- summary ---------------------------------------------------------------------

def outcome_label(row):
    """PASS, PASS* (known failures only), FAIL, NOT_RUN for one case row."""
    outcome = (row.get('outcome') or 'not_run').upper()
    acc = (row.get('evidence') or {}).get('acceptance') or {}
    if outcome == 'PASS' and any(c['status'] == 'known_fail' for c in acc.get('checks', {}).values()):
        return 'PASS*'
    return outcome


def describe(row):
    """Key figures of one case for the one-page summary."""
    evidence = row.get('evidence') or {}
    out = []
    acc = evidence.get('acceptance')
    if acc:
        checks = ', '.join('%s %s' % (name, check['status'].upper()) for name, check in acc.get('checks', {}).items())
        if checks:
            out.append('checks: ' + checks)
        for figure, value in acc.get('figures', {}).items():
            if value.get('page') is False:
                continue   # in result.json only
            if value.get('status') == 'not_run':
                out.append('%s: not run' % figure)
            elif 'samples' in value:
                ports = ' '.join('%s:%d/%dus' % (p['port'], p['exits'], p['us']) for p in value.get('top_ports', [])[:3])
                costs = ' '.join('%s:%dus' % (c['cause'], c['us']) for c in value.get('top_costs', [])[:2])
                out.append('%s: samples=%s exits med=%s max=%s busy_pm med=%s ports %s cost %s' % (
                    figure, value['samples'], value['exits_median'], value['exits_max'], value['busy_pm_median'], ports or '-', costs or '-'))
            elif 'lines' in value:
                out.append('%s: %d line(s)%s' % (figure, value['count'], ', first: ' + value['lines'][0]['text'][:110] if value['count'] else ''))
            elif 'text' in value:
                out.append('%s: %s' % (figure, (value['text'] or 'none')[:120]))
            else:
                out.append('%s: %s' % (figure, value.get('count')))
        for step in [s for s in acc.get('steps', []) if s['status'] in ('fail', 'known_fail')][:4]:
            out.append('step %s %s: %s%s' % (step['name'], step['status'].upper(), step.get('reason', '')[:150],
                                             ' (directive %s)' % step['known_failure']['directive'] if step.get('known_failure') else ''))
        for assertion in acc.get('assertions', []):
            if assertion['status'] != 'pass':
                out.append('assertion %s %s (count %s)%s' % (assertion['name'], assertion['status'].upper(), assertion.get('count'),
                           ' directive: ' + str(assertion['known_failure'].get('directive')) if assertion.get('known_failure') else ''))
        if acc.get('disk'):
            d = acc['disk']
            out.append('disk %s: files %s, menu links %d%s' % (d['status'].upper(), ', '.join(
                '%s=%s' % (Path(f['path'].replace('\\', '/')).name, 'ok' if f['match'] else 'missing' if not f['present'] else 'differs')
                for f in d['files']), len(d.get('menu_links', [])), ' error: ' + d['error'][:80] if d.get('error') else ''))
        if acc.get('stopped'):
            out.append('stopped: ' + acc['stopped'][:180])
    sites = evidence.get('browser_sites')
    if sites:
        page = next((r for r in evidence.get('observed', []) if r.get('case') == 'page'), None)
        if page:
            out.append('page status=%s bytes=%s tls=%s' % (page.get('status'), page.get('bytes'), page.get('tls')))
        out.append('fixture: %d site requests, %d served, %d not in snapshot, %d fetches done, %d fixture fetch errors, '
                   '%d other-host errors, %d layout rejections' % (
                       sites['site_requests'], sites['served'], len(sites['missing_from_snapshot']), sites.get('fetches_done', 0),
                       len(sites['fixture_fetch_errors']), sites['other_host_fetch_errors'], sites['layout_rejections']))
    if row.get('outcome') != 'pass' and (evidence.get('reason') or row.get('reason')):
        out.append('reason: ' + str(evidence.get('reason') or row.get('reason'))[:200])
    return out


def item_rows(row):
    """(item number, label, title) per trial-list item a case row covers."""
    acc = (row.get('evidence') or {}).get('acceptance') or {}
    label = outcome_label(row)
    if acc.get('items'):
        out = []
        for number, info in acc['items'].items():
            status = info['status']
            text = {'pass': 'PASS', 'known_fail': 'PASS*', 'not_run': 'NOT_RUN', 'fail': 'FAIL'}.get(status, 'FAIL')
            if label == 'NOT_RUN':
                text = 'NOT_RUN'
            out.append((int(number), text, ', '.join(info['checks'])))
        return out
    item = acc.get('item') or row.get('item')
    return [(item, label, acc.get('title') or row.get('title') or '')]


def verdict(rows):
    """BLOCKED (a blocking failure), INCOMPLETE (a case not run that is not
    pending), ACCEPTED otherwise."""
    blocking, missing = [], []
    for row in rows:
        acc = (row.get('evidence') or {}).get('acceptance') or {}
        label = outcome_label(row)
        if label == 'FAIL' and row.get('blocking', acc.get('blocking', True)):
            blocking.append(row['case'])
        elif label == 'NOT_RUN' and not str((row.get('evidence') or {}).get('reason') or row.get('reason') or '').startswith('pending'):
            missing.append(row['case'])
    return ('BLOCKED' if blocking else 'INCOMPLETE' if missing or not rows else 'ACCEPTED'), blocking, missing


def write_summary(suite, rows, out, image=None):
    """One page: per case PASS/FAIL, key figures and the evidence path."""
    lines = []
    first = next((r.get('evidence') for r in rows if r.get('evidence')), {}) or {}
    image = image or first.get('image', {})
    manifest = first.get('build_manifest', {})
    lines.append('CiukiOS acceptance %s  -  %s' % (suite, time.strftime('%Y-%m-%d %H:%M:%S %Z')))
    lines.append('image %s  build %s%s' % (image.get('sha256', 'unknown'), manifest.get('revision', 'unknown'),
                                          ' (dirty)' if manifest.get('dirty') is True else ''))
    state, blocking, missing = verdict(rows)
    counts = {}
    for row in rows:
        counts[outcome_label(row)] = counts.get(outcome_label(row), 0) + 1
    lines.append('verdict %s: %s' % (state, ', '.join('%s %d' % (k, v) for k, v in sorted(counts.items()))))
    if blocking:
        lines.append('blocking failures: ' + ', '.join(blocking))
    if missing:
        lines.append('not run: ' + ', '.join(missing))
    items = sorted((i for row in rows for i in item_rows(row) if i[0]), key=lambda x: x[0])
    if items:
        lines.append('items: ' + '  '.join('%d %s' % (number, text) for number, text, _ in items))
    lines.append('')
    for row in rows:
        evidence = row.get('evidence') or {}
        acc = evidence.get('acceptance') or {}
        numbers = sorted({n for n, _, _ in item_rows(row) if n})
        title = acc.get('title') or row.get('title') or ''
        elapsed = (evidence.get('timing') or {}).get('host_monotonic_elapsed_seconds')
        lines.append('[%s] %-8s %s%s%s' % (','.join(str(n) for n in numbers) or '-', outcome_label(row), row['case'],
                                           ' - ' + title if title else '', ' (%.0f s)' % elapsed if elapsed else ''))
        if acc.get('items') and len(acc['items']) > 1:
            for number, text, checks in item_rows(row):
                lines.append('      item %d %s (%s)' % (number, text, checks))
        for text in describe(row):
            lines.append('      ' + text)
        result_path = row.get('result')
        if result_path:
            directory = Path(result_path).parent
            lines.append('      evidence: %s' % (directory / 'serial.log'))
            kept = sorted(p.name for p in directory.glob('*.ppm')) if directory.is_dir() else []
            if kept:
                lines.append('      screens: ' + ', '.join(kept[:6]) + (' (+%d)' % (len(kept) - 6) if len(kept) > 6 else ''))
            if (directory / 'run.qcow2').is_file():
                lines.append('      overlay kept: %s' % (directory / 'run.qcow2'))
    text = '\n'.join(lines) + '\n'
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(text)
    return text


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(description='fx-accept helpers (no QEMU)')
    sub = ap.add_subparsers(dest='command', required=True)
    s = sub.add_parser('summarize', help='write the one-page summary from a suite summary.json')
    s.add_argument('summary', type=Path)
    s.add_argument('--out', type=Path)
    v = sub.add_parser('validate', help='validate a suite file without QEMU and print its plan')
    v.add_argument('suite')
    args = ap.parse_args(argv)
    if args.command == 'summarize':
        data = json.loads(args.summary.read_text())
        print(write_summary(data['suite'], data['cases'], args.out or args.summary.with_name('summary.txt')), end='')
        return 0
    import run as runner
    suite = runner.load_suite(args.suite)
    for case in suite['cases']:
        kind = runner.case_kind(case)
        steps = expand_steps(case.get('steps', [])) if kind == KIND else []
        print('%-28s %-11s item %-2s %4d s %3d steps  %s' % (case['id'], kind, case.get('item', '-'), case.get('timeout', 0),
                                                         len(steps), case.get('title', '')))
    print('%s: %d cases valid' % (args.suite, len(suite['cases'])))
    return 0


if __name__ == '__main__':
    sys.exit(main())
