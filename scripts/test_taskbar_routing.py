#!/usr/bin/env python3
"""Execute actual shell task dispatch and keyboard focus with colliding IDs."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

from unicorn import Uc, UC_ARCH_X86, UC_MODE_16
from unicorn.x86_const import UC_X86_REG_AX, UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS, UC_X86_REG_SP

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x10000


def assemble(out, *, remove_owner_route=False):
    source = (ROOT / 'src/com/shell_gui.inc').read_text()
    action = re.search(r'^ui_action:\n.*?^\.non_files:\n', source, re.S | re.M)
    assert action, 'Actual task/module dispatch prefix unavailable'
    dispatch = action.group()
    if remove_owner_route:
        dispatch, count = re.subn(r'(?<=\.menu_closed:\n).*?(?=^\.module_controls:\n)',
                                 '', dispatch, flags=re.S | re.M)
        assert count == 1, 'Owner dispatch mutation unavailable'
    (out / 'taskbar_routing_action.inc').write_text(dispatch)
    inputs = (ROOT / 'src/com/shell_gui_input.inc').read_text()
    focus = re.search(r'^ui_focus_previous:\n.*?(?=^ui_damage_pressed:)', inputs, re.S | re.M)
    assert focus, 'Actual keyboard focus walk unavailable'
    (out / 'taskbar_routing_focus.inc').write_text(focus.group())
    binary = out / 'taskbar-routing.com'
    subprocess.run(['nasm', '-f', 'bin', '-I' + str(out) + '/', '-o', str(binary),
                    'scripts/fixtures/taskbar_routing_cpu.asm'], cwd=ROOT, check=True)
    return binary, dispatch, focus.group()


class Shell:
    def __init__(self, code):
        self.cpu = Uc(UC_ARCH_X86, UC_MODE_16)
        self.cpu.mem_map(0, 0x100000)
        self.cpu.mem_write(BASE + 0x100, code)
        self.symbols = {}
        pos = code.index(b'CTR1') + 4
        while True:
            offset = struct.unpack_from('<H', code, pos)[0]
            pos += 2
            if not offset:
                break
            end = code.index(0, pos)
            self.symbols[code[pos:end].decode()] = BASE + offset
            pos = end + 1

    def set(self, name, value, size=2):
        self.cpu.mem_write(self.symbols[name], value.to_bytes(size, 'little'))

    def get(self, name, size=2):
        return int.from_bytes(self.cpu.mem_read(self.symbols[name], size), 'little')

    def call(self, name, action=0):
        cpu = self.cpu
        for register in (UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS):
            cpu.reg_write(register, BASE >> 4)
        cpu.reg_write(UC_X86_REG_SP, 0xfffc)
        cpu.reg_write(UC_X86_REG_AX, action)
        cpu.mem_write(BASE + 0xfffc, struct.pack('<H', self.symbols['stop'] - BASE))
        cpu.emu_start(self.symbols[name], self.symbols['stop'], count=10000)
        assert cpu.reg_read(UC_X86_REG_SP) == 0xfffe, 'Dispatch did not complete'
        return cpu.reg_read(UC_X86_REG_AX)


def checks(code):
    results = []
    # The paint owner is deliberately unrelated to the click owner. This
    # detects accidentally using the post-paint owner instead of the hit tag.
    for action in (200, 208, 211, 231):
        for active in (8, 11, 31):
            for paint_owner in (0, active + 1):
                for present in (0, 1):
                    shell = Shell(code)
                    shell.set('ui_active_window', active, 1)
                    shell.set('ui_hit_owner', paint_owner, 1)
                    shell.set('module_present', present, 1)
                    shell.call('ui_action', action)
                    assert shell.get('route', 1) == 1
                    assert shell.get('opened_window', 1) == action - 200
                    assert shell.get('slot_calls') == shell.get('action_calls') == 0
                    assert shell.get('damage_calls') == 1
                    results.append(dict(case='taskbar', action=action, active=active,
                                        paint_owner=paint_owner, module_present=present))
    for action in (100, 199, 200, 208, 211, 231, 232, 248):
        for active in (8, 11, 31):
            for paint_owner in (0, active + 1):
                for answer, route in ((0, 3), (250, 2), (249, 4)):
                    shell = Shell(code)
                    shell.set('ui_active_window', active, 1)
                    shell.set('ui_hit_target', active + 1, 1)
                    shell.set('ui_hit_owner', paint_owner, 1)
                    shell.set('module_result', answer)
                    shell.call('ui_action', action)
                    assert shell.get('route', 1) == route
                    assert shell.get('opened_window', 1) == 0xff
                    assert shell.get('slot_calls') == shell.get('action_calls') == 1
                    assert shell.get('action_input') == action
                    results.append(dict(case='module-control', action=action, active=active,
                                        paint_owner=paint_owner, answer=answer))
    for action in (99, 100, 199, 232, 248):
        shell = Shell(code)
        shell.set('module_present', 0, 1)
        shell.call('ui_action', action)
        assert shell.get('route', 1) == 5 and shell.get('action_calls') == 0
        results.append(dict(case='native-control-boundary', action=action))
    shell = Shell(code)
    shell.call('ui_action', 249)
    assert shell.get('route', 1) == 4 and shell.get('action_calls') == 0
    results.append(dict(case='command-request'))
    # Real Tab traversal excludes taskbar-owner0 hits and module-owned IDs
    # above199, even when they share211. Test next and previous traversal.
    for name in ('ui_focus_next', 'ui_focus_previous'):
        shell = Shell(code)
        records = [(211, 9), (110, 9), (211, 0), (120, 12)]
        data = b''.join(bytes(8) + bytes(pair) for pair in records)
        shell.cpu.mem_write(shell.symbols['ui_hits'], data)
        shell.set('ui_hit_count', len(records))
        assert shell.call(name) == 250 and shell.get('ui_focus') == 110
        results.append(dict(case='keyboard-focus', direction=name))
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/tests/taskbar-routing-cpu')
    args = parser.parse_args()
    out = args.output.resolve()
    assert ROOT / 'build' in out.parents
    out.mkdir(parents=True, exist_ok=True)
    binary, action, focus = assemble(out)
    report = dict(passed=False, fixture_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                  action_sha256=hashlib.sha256(action.encode()).hexdigest(),
                  focus_sha256=hashlib.sha256(focus.encode()).hexdigest())
    try:
        report['checks'] = checks(binary.read_bytes())
        negative_dir = out / 'without-owner-routing'
        negative_dir.mkdir(exist_ok=True)
        negative, _, _ = assemble(negative_dir, remove_owner_route=True)
        shell = Shell(negative.read_bytes())
        shell.call('ui_action', 211)
        # Removing the new route reproduces the original collision: Files
        # receives the DOS task button as its own control and remains active.
        assert shell.get('route', 1) == 2 and shell.get('action_input') == 211
        assert shell.get('opened_window', 1) == 0xff
        report['negative_control'] = dict(detected=True, route='module-redraw',
                                          action=211, native_window_opened=False)
        report['passed'] = True
    finally:
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Taskbar routing: {len(report["checks"])} CPU scenarios PASS; {out / "report.json"}')


if __name__ == '__main__':
    main()
