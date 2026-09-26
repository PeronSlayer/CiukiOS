#!/usr/bin/env python3
"""Exercise late UI fixes in complete production COM binaries with Unicorn.

Run: uv run --with unicorn python scripts/claude_followup_cpu.py
This is an instruction-level test: BIOS keyboard responses and boundary calls
to repaint/video teardown are supplied by the harness. It is not a display,
wall-clock, hardware or full-operating-system test.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_CODE, UC_HOOK_INTR
from unicorn.x86_const import *

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x10000
ORG = 0x100
RETURN = 0xF000
DAY = 0x1800B0


def symbol(lines, name):
    for index, line in enumerate(lines):
        if re.search(r'\b' + re.escape(name) + r'(?=:|\s+d[bwd]\b|\s+times\b)', line):
            for following in lines[index:]:
                found = re.match(r'\s*\d+\s+([0-9A-F]{8})\s', following)
                if found:
                    return ORG + int(found[1], 16)
    raise AssertionError(f'Production listing has no {name}')


class Program:
    def __init__(self, binary, listing):
        self.binary = binary
        self.lines = listing.read_text().splitlines()
        self.uc = Uc(UC_ARCH_X86, UC_MODE_16)
        self.uc.mem_map(0, 0x100000)
        self.uc.mem_write(BASE + ORG, binary.read_bytes())
        self.queue = []
        self.shift = 0
        self.reads = []
        self.stops = {}
        self.stopped = None
        self.uc.hook_add(UC_HOOK_INTR, self.interrupt)
        self.uc.hook_add(UC_HOOK_CODE, self.code)

    def address(self, name):
        return symbol(self.lines, name)

    def put(self, name, value, length=2):
        self.uc.mem_write(BASE + self.address(name), value.to_bytes(length, 'little'))

    def get(self, name, length=2):
        return int.from_bytes(self.uc.mem_read(BASE + self.address(name), length), 'little')

    def interrupt(self, cpu, number, _):
        assert number == 0x16, ('unexpected interrupt', number)
        function = cpu.reg_read(UC_X86_REG_AH)
        if function == 0x11:
            flags = cpu.reg_read(UC_X86_REG_EFLAGS)
            cpu.reg_write(UC_X86_REG_EFLAGS, flags & ~0x40 if self.queue else flags | 0x40)
            if self.queue:
                cpu.reg_write(UC_X86_REG_AX, self.queue[0])
        elif function == 0x10:
            assert self.queue, 'blocking keyboard read on empty queue'
            value = self.queue.pop(0)
            self.reads.append(value)
            cpu.reg_write(UC_X86_REG_AX, value)
        elif function == 2:
            cpu.reg_write(UC_X86_REG_AL, self.shift)
        else:
            raise AssertionError(('unexpected keyboard function', function))

    def code(self, cpu, address, _size, _):
        if address == BASE + RETURN:
            self.stopped = 'return'
            cpu.emu_stop()
        elif address in self.stops:
            self.stopped = self.stops[address]
            cpu.emu_stop()

    def run(self, entry, stops=()):
        cpu = self.uc
        for register in (UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_SS):
            cpu.reg_write(register, BASE // 16)
        cpu.reg_write(UC_X86_REG_ES, 0x3333)
        cpu.reg_write(UC_X86_REG_EAX, 0xAABBCCDD)
        cpu.reg_write(UC_X86_REG_EDX, 0xA55A5AA5)
        cpu.reg_write(UC_X86_REG_EFLAGS, 0x202)
        cpu.reg_write(UC_X86_REG_SP, RETURN-2)
        cpu.mem_write(BASE + RETURN-2, struct.pack('<H', RETURN))
        self.stops = {BASE + self.address(name): name for name in stops}
        self.stopped = None
        offset = self.address(entry) if isinstance(entry, str) else entry
        cpu.emu_start(BASE + offset, BASE + 0xFFFF, count=20000)
        return self.stopped


def timers(program):
    starts = [0, 1, 0xFFFF-218, 0xFFFF, 0x10000]
    starts += list(range(0x17FFD0, DAY))
    elapsed_values = [0, 1, 42, 175, 176, 217, 218, 219, 500]
    cpu = program.uc
    samples = 0
    for start in starts:
        for elapsed in elapsed_values:
            program.put('preview_tick', start, 4)
            cpu.mem_write(0x46C, struct.pack('<I', (start + elapsed) % DAY))
            cpu.mem_write(0x470, b'\x03')
            assert program.run('preview_elapsed') == 'return'
            observed = cpu.reg_read(UC_X86_REG_EDX)
            assert observed == elapsed, (hex(start), elapsed, observed)
            assert cpu.reg_read(UC_X86_REG_EAX) == 0xAABBCCDD
            assert cpu.reg_read(UC_X86_REG_ES) == 0x3333
            assert bytes(cpu.mem_read(0x470, 1)) == b'\x03', 'DOS date rollover was consumed'
            samples += 1
    # Execute both actual preview call sites through their deadline branches.
    calls = []
    for line in program.lines:
        found = re.match(r'\s*\d+\s+([0-9A-F]{8})\s.*\bcall preview_elapsed\b', line)
        if found:
            calls.append(ORG + int(found[1], 16))
    assert len(calls) == 2, calls
    for caller in calls:
        for elapsed in (0, 217, 218, 219, 0x10003):
            program.put('preview_tick', 0, 4)
            cpu.mem_write(0x46C, struct.pack('<I', elapsed))
            result = program.run(caller, ('vc_end',))
            assert (result == 'vc_end') == (elapsed >= 218), (caller, elapsed, result)
    return {'elapsed_cases': samples, 'actual_preview_callers': 2,
            'deadline_cases': 10, 'midnight_flag_preserved': True}


def prepare_run(program):
    for name, value, length in (
            ('ui_dialog', 1, 1), ('ui_active_window', 1, 1),
            ('ui_focus', 40, 2), ('ui_hit_count', 4, 2),
            ('ui_run_len', 0, 1), ('ui_run_cursor', 0, 1),
            ('ui_dialog_x', 100, 2), ('ui_dialog_y', 100, 2),
            ('ui_width', 800, 2), ('ui_height', 600, 2)):
        program.put(name, value, length)
    program.uc.mem_write(BASE + program.address('ui_run_text'), bytes(97))
    # The native Run window records its title controls, then its footer.
    hits = b''.join(bytes(8) + bytes([action, 2]) for action in (17, 18, 40, 41))
    program.uc.mem_write(BASE + program.address('ui_hits'), hits)


def batched_tab(program, reverse, released, plain_shift_held=False):
    prepare_run(program)
    tab = 0x0F00 if reverse else 0x0F09
    program.queue = [0x1E61, 0x3062, tab, 0x2E63]
    program.reads = []
    program.shift = 1 if plain_shift_held else (0 if released or not reverse else 1)
    assert program.run('ui_redraw_input', ('ui_redraw', 'ui_action')) == 'ui_redraw'
    assert program.reads == [0x1E61, 0x3062], program.reads
    assert program.queue == [tab, 0x2E63], 'Tab was consumed by the editing batch'
    assert program.get('ui_run_len', 1) == 2
    assert bytes(program.uc.mem_read(BASE + program.address('ui_run_text'), 3)) == b'ab\0'
    assert program.run('ui_event') == 'return'
    assert program.queue == [0x2E63]
    expected = 18 if reverse or plain_shift_held else 41
    actual = program.get('ui_focus')
    assert actual == expected, f'Expected focus {expected}, observed {actual}; shift state {program.shift}'
    return {'queued_tab_preserved': True, 'expected_focus': expected,
            'actual_focus': actual, 'shift_released_before_event': released,
            'plain_tab_with_live_shift': plain_shift_held}


def pointer_guard(program):
    samples = 0
    for packed in (0, 1):
        for ready, visible in ((0, 0), (0, 1), (1, 1)):
            program.put('ui_vbe', packed, 1)
            program.put('ui_mouse_ready', ready, 1)
            program.put('ui_pointer_visible', visible, 1)
            stopped = program.run('ui_pointer_show', ('ui_vga_pointer_show', 'vc_pointer_save'))
            assert stopped == 'return', (packed, ready, visible, stopped)
            samples += 1
    return {'no_duplicate_backend_entries': samples,
            'scope': 'Generic guard only; this does not emulate VGA planes'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path,
                        default=ROOT/'build/full/ui-smooth-2026-09-25/claude-followup-cpu')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    programs = {}
    hashes = {}
    for name in ('shell', 'vgasetup'):
        binary, listing = output/f'{name}.com', output/f'{name}.lst'
        subprocess.run(['nasm', '-f', 'bin', f'src/com/{name}.asm', '-o', str(binary),
                        '-l', str(listing)], cwd=ROOT, check=True)
        programs[name] = Program(binary, listing)
        hashes[name] = hashlib.sha256(binary.read_bytes()).hexdigest()
    tests = {
        'preview_midnight_and_deadline': lambda: timers(programs['vgasetup']),
        'queued_tab': lambda: batched_tab(programs['shell'], False, False),
        'queued_shift_tab_held': lambda: batched_tab(programs['shell'], True, False),
        'queued_shift_tab_released': lambda: batched_tab(programs['shell'], True, True),
        'plain_tab_with_live_shift': lambda: batched_tab(programs['shell'], False, False, True),
        'generic_pointer_guard': lambda: pointer_guard(programs['shell']),
    }
    results = {'kind': 'production CPU instructions with simulated BIOS responses',
               'binary_sha256': hashes, 'checks': {}}
    failed = False
    for name, test in tests.items():
        try:
            value = test()
            results['checks'][name] = {'status': 'passed', **value}
            print(f'PASS {name}', flush=True)
        except Exception as error:
            failed = True
            results['checks'][name] = {'status': 'failed', 'error': str(error)}
            print(f'FAIL {name}: {error}', flush=True)
    results['status'] = 'failed' if failed else 'passed'
    (output/'results.json').write_text(json.dumps(results, indent=2) + '\n')
    return int(failed)


if __name__ == '__main__':
    raise SystemExit(main())
