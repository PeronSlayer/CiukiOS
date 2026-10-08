"""Launch an installed TestGames shortcut through real Files double-clicks.

Reusable by the current WindowVM/Utilities gates. This deliberately exercises
Files' working-directory change and an empty game argument string, unlike a
Run field containing a command plus -warp. No RAM writes or fixture files.
"""
from pathlib import Path
import re
import struct
import subprocess
import time

from qemu_test_desktop_apps import Gate
from qemu_test_installed_hdd import FAT16

FILES_WINDOW = 8
TESTGAMES = r'C:\DESKTOP\TestGames'


def address_fields(memory, field_offset=None):
    """Locate Files' real near-pointer field; reject ambiguous observations."""
    found = []
    offsets = range(0, len(memory) - 12, 2) if field_offset is None else (field_offset,)
    for offset in offsets:
        if not 0 <= offset <= len(memory) - 12:
            continue
        pointer, capacity, length, cursor, selected, scroll = struct.unpack_from('<6H', memory, offset)
        if capacity != 260 or not (0x180 <= pointer <= len(memory) - capacity):
            continue
        if length >= capacity or cursor > length or selected > 1 or scroll > length:
            continue
        text = memory[pointer:pointer + capacity].split(b'\0', 1)[0]
        if len(text) != length or any(value < 32 for value in text):
            continue
        # Discovery requires the initialized drive/path. Once bound to this
        # exact structure, typing necessarily passes through "c" and "c:".
        if field_offset is None and not re.match(rb'[A-Za-z]:\\', text):
            continue
        found.append(dict(offset=offset, pointer=pointer, length=length, cursor=cursor,
                          selected=selected, text=text.decode('cp437')))
    return found


def observed_double_click(send_button, observe, executed, *, attempts=3,
                          timeout=30, clock=time.monotonic, sleep=time.sleep):
    """Send real edges only after the shell acknowledges their predecessors.

    The BIOS clock bounds the interval conservatively: the first app callback
    cannot precede the sample taken before its injected press. An expired
    pair is released before retrying; no completed-paint wait extends it.
    """
    deadline = clock() + timeout
    events = []

    def edge(button, description):
        edge_deadline = min(deadline, clock() + 10)
        state = None
        while clock() < edge_deadline:
            if executed():
                return None
            state = observe()
            consumed = (state['buttons'] & 1) == button and state['previous'] == button
            routed = state['capture'] == (FILES_WINDOW + 1 if button else 0)
            if consumed and routed and state['active'] == FILES_WINDOW:
                if button:
                    assert not (state['shift'] & 7), f'Modifier held during Files click: {state}'
                events.append(dict(edge=description, **state))
                return state
            sleep(.01)
        raise AssertionError(f'Files did not acknowledge {description}: {state}; events={events}')

    def fresh_after(tick):
        while clock() < deadline:
            if executed():
                return
            state = observe()
            if ((state['ticks'] - tick) & 0xffff) >= 9:
                return
            sleep(.01)
        raise AssertionError(f'Files double-click retry did not become fresh: {events}')

    try:
        send_button(0)
        released = edge(0, 'initial release')
        if released is None:
            return dict(attempts=0, events=events, executed_during_edge=True)
        for attempt in range(1, attempts + 1):
            # This sample precedes the first real down. Its tick is an earlier
            # bound than Files' last_click_tick, avoiding a false short pair.
            anchor = observe()['ticks']
            send_button(1)
            pressed = edge(1, f'press1 attempt{attempt}')
            if pressed is None:
                return dict(attempts=attempt, events=events, executed_during_edge=True)
            send_button(0)
            released = edge(0, f'release1 attempt{attempt}')
            if released is None:
                return dict(attempts=attempt, events=events, executed_during_edge=True)
            elapsed = (released['ticks'] - anchor) & 0xffff
            if elapsed >= 9:
                events.append(dict(expired=True, attempt=attempt, ticks=elapsed))
                fresh_after(pressed['ticks'])
                continue
            send_button(1)
            pressed = edge(1, f'press2 attempt{attempt}')
            if pressed is None:
                return dict(attempts=attempt, events=events, executed_during_edge=True)
            elapsed = (pressed['ticks'] - anchor) & 0xffff
            send_button(0)
            released = edge(0, f'release2 attempt{attempt}')
            if released is None or executed():
                return dict(attempts=attempt, events=events, executed_during_edge=True)
            if elapsed < 9:
                return dict(attempts=attempt, events=events, first_to_second_ticks=elapsed)
            events.append(dict(expired=True, attempt=attempt, ticks=elapsed))
            fresh_after(pressed['ticks'])
        raise AssertionError(f'Files double-click expired after {attempts} attempts: {events}')
    finally:
        # EXEC may switch active windows before a down can be sampled. Always
        # release the physical button, including failures and that transition.
        send_button(0)


def launch_testgames_from_files(vm, ui, disk, name):
    """Return the pre-launch serial offset and evidence for DOOM/WOLF3D.COM."""
    name = name.upper()
    assert name in ('DOOM.COM', 'WOLF3D.COM'), name
    disk = Path(disk)
    fat = FAT16(disk)
    volume = f'{disk}@@{fat.start}'
    entries = subprocess.check_output([
        'mdir', '-b', '-i', volume, '::DESKTOP/TestGames'
    ]).decode('utf-8', 'replace').splitlines()
    # The installed TestGames directory contains only visible COM launchers.
    # Read it independently so a changed catalog cannot silently click a
    # different application at a hard-coded row index.
    names = [entry.rsplit('/', 1)[-1] for entry in entries if entry.strip()]
    assert names and all(value.upper().endswith('.COM') for value in names), entries
    names.sort(key=str.upper)
    index = next((i for i, value in enumerate(names) if value.upper() == name), None)
    assert index is not None, (name, names)
    gate = Gate(vm)
    ui.refresh()
    if not (ui.b('ui_window_flags', FILES_WINDOW) & 1):
        opened = vm.offset(); vm.key('meta_l-e')
        vm.wait('WINDOW 08 OPEN', opened, 30)
        ui.until(lambda: ui.b('ui_active_window') == FILES_WINDOW, 'Files did not become active', 20)
    elif ui.b('ui_active_window') != FILES_WINDOW:
        # Use the exposed title, independently of taskbar routing.
        ui.click(62, FILES_WINDOW)
        ui.until(lambda: ui.b('ui_active_window') == FILES_WINDOW, 'Files title raise failed', 20)

    address_binding = {}

    def address():
        ui.refresh()
        segment = ui.w('app_segs', 0)  # Files is the source-defined module slot0.
        assert segment, 'Files module is not loaded'
        dump = vm.output / 'files-observed.bin'
        vm.hmp(f'pmemsave {segment << 4} 65536 "{dump}"')
        if address_binding:
            assert segment == address_binding['segment'], 'Files module changed during address editing'
        fields = address_fields(dump.read_bytes(), address_binding.get('offset'))
        assert len(fields) <= 1, f'Files address field is ambiguous: {fields}'
        if not fields:
            return None  # Bounded observer waits for initialization/update.
        if address_binding:
            assert fields[0]['pointer'] == address_binding['pointer'], 'Files address buffer moved'
        else:
            address_binding.update(segment=segment, offset=fields[0]['offset'], pointer=fields[0]['pointer'])
        return fields[0]

    def key(value):
        vm.key(value)

    def menu_key(value):
        # Menu mnemonics inspect HOST.shift; hold Alt through a completed
        # app poll. Plain edit characters instead use their BIOS ASCII code.
        vm.hmp(f'sendkey {value} 150'); time.sleep(.3)

    def observed(predicate, description, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            state = address()
            if state is not None and predicate(state):
                return state
            time.sleep(.1)
        raise AssertionError(f'{description}: {state}')

    # Click the actual address field instead of depending on an Alt+D whose
    # modifier may have been released before the app receives its queued key.
    ui.refresh()
    fx, fy = (ui.w(label, FILES_WINDOW) for label in ('ui_window_x', 'ui_window_y'))
    vm.completed_control_click(fx + 90, fy + 95)
    key('ctrl-a')
    observed(lambda state: state['selected'] == 1, 'Address was not selected')
    navigate = vm.offset()
    prefix = ''
    for ch in TESTGAMES.lower():
        prefix += ch
        value = {'\\': 'backslash', ':': 'shift-semicolon'}.get(ch, ch)
        key(value)
        # Wait for each actual field update before adding another key. An
        # expensive wallpaper paint must not fill the BIOS keyboard queue.
        state = observed(lambda current: current['text'] == prefix,
                         f'Address did not accept prefix {prefix!r}')
    assert state['text'].upper() == TESTGAMES.upper(), f'Address lost input: {state}'
    key('ret')
    vm.wait('[FILES] list', navigate, 30)
    log = gate.serial(navigate)
    assert re.search(r'\[FILES\] list C:\\DESKTOP\\TESTGAMES\s+\d+', log, re.IGNORECASE), log[-1000:]
    # Details plus ascending Name sort gives a stable row layout. Files'
    # menu command explicitly resets sort_desc=0; only header clicks toggle
    # the direction. Do not substitute clicking the Name header here.
    menu_key('alt-v'); menu_key('d')
    menu_key('alt-v'); menu_key('n')
    ui.refresh()
    fx, fy, fw = (ui.w(label, FILES_WINDOW) for label in
                  ('ui_window_x', 'ui_window_y', 'ui_window_width'))
    # Production files.c layout: client=(x+3,y+30), menu20, toolbar34,
    # address28, details header22, row18; Places occupies160 at width>=566.
    places = 160 if fw - 6 >= 560 else 0
    px = fx + 65 + places
    py = fy + 143 + index * 18
    vm.position(px, py)
    launch = vm.offset()
    labels = ('ui_mouse_buttons', 'ui_previous_button', 'app_capture',
              'ui_active_window', 'app_segs')
    offsets = {label: ui.base + ui.offset(label) for label in labels}
    module_base = address_binding['segment'] << 4
    # One bounded read captures the BIOS tick, module HOST and listing-bound
    # shell fields together. It avoids three monitor round trips per edge.
    observed_bytes = max(max(offsets.values()) + 2, module_base + 0x134)
    assert observed_bytes <= 1048576, f'Files/shell state outside low memory: {observed_bytes}'
    mouse_dump = vm.output / 'files-mouse-observed.bin'

    def mouse_state():
        vm.hmp(f'pmemsave 0 {observed_bytes} "{mouse_dump}"')
        memory = mouse_dump.read_bytes()
        assert len(memory) == observed_bytes, 'Incomplete Files mouse observation'
        word = lambda offset: struct.unpack_from('<H', memory, offset)[0]
        assert word(offsets['app_segs']) == address_binding['segment'], 'Files module changed during click'
        return dict(buttons=word(offsets['ui_mouse_buttons']),
                    previous=memory[offsets['ui_previous_button']],
                    capture=memory[offsets['app_capture']],
                    active=memory[offsets['ui_active_window']], ticks=word(0x46c),
                    host_ticks=word(module_base + 0x128),
                    shift=word(module_base + 0x124))

    mouse_evidence = observed_double_click(
        lambda button: vm.hmp(f'mouse_button {button}'), mouse_state,
        lambda: '[FILES] execute ' in gate.serial(launch))
    vm.wait('[FILES] execute', launch, 30)
    vm.wait('[DOSVM] open', launch, 30)
    log = gate.serial(launch)
    executed = re.search(r'\[FILES\] execute ([^\r\n]+)', log)
    opened = re.search(r'\[DOSVM\] open ([^\r\n]+)', log)
    wanted = TESTGAMES + '\\' + name
    assert executed and executed.group(1).upper() == wanted.upper(), log[-1800:]
    assert opened and opened.group(1).upper() == wanted.upper(), log[-1800:]
    return launch, dict(route='Files double-click', path=wanted, arguments='',
                        files_directory=TESTGAMES, catalog=names, row=index,
                        click_point=[px, py], address_verified=state,
                        address_characters_verified=len(prefix), address_binding=address_binding,
                        mouse_edges_verified=mouse_evidence)
