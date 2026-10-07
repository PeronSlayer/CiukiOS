#!/usr/bin/env python3
"""Execute production GUI ownership quarantine and teardown with a fake monitor."""
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


def assemble(out):
    definitions = []
    for filename, names in (
        ('shell_gui.inc', ('ui_video_wait_release', 'ui_mode_switch')),
        ('shell_gui_compositor.inc', ('ui_comp_begin', 'ui_comp_end', 'ui_pages_failure')),
    ):
        source = (ROOT / 'src/com' / filename).read_text()
        for name in names:
            match = re.search(r'^' + name + r':\n.*?(?=^[A-Za-z_]\w*:|\Z)', source,
                              re.S | re.M)
            assert match, name
            definitions.append(match.group(0))
    (out / 'gui_video_quarantine.inc').write_text('\n'.join(definitions))
    binary = out / 'gui-video-quarantine.com'
    subprocess.run(['nasm', '-f', 'bin', '-I' + str(out) + '/',
                    '-o', str(binary), 'scripts/fixtures/gui_video_quarantine_cpu.asm'],
                   cwd=ROOT, check=True)
    return binary


class GUI:
    def __init__(self, code):
        self.cpu = Uc(UC_ARCH_X86, UC_MODE_16)
        self.cpu.mem_map(0, 0x100000)
        self.cpu.mem_write(BASE + 0x100, code)
        self.symbols = {}
        pos = code.index(b'CVQ1') + 4
        while True:
            offset = struct.unpack_from('<H', code, pos)[0]
            pos += 2
            if not offset:
                break
            end = code.index(0, pos)
            self.symbols[code[pos:end].decode()] = BASE + offset
            pos = end + 1
        self.halts = self.frees = self.allocations = 0
        self.cpu.hook_add(UC_HOOK_CODE, self.instruction)
        self.cpu.hook_add(UC_HOOK_INTR, self.interrupt)

    def get(self, name, size=2):
        return int.from_bytes(self.cpu.mem_read(self.symbols[name], size), 'little')

    def set(self, name, value, size=2):
        self.cpu.mem_write(self.symbols[name], value.to_bytes(size, 'little'))

    def instruction(self, cpu, address, size, _):
        if bytes(cpu.mem_read(address, size)) == b'\xf4':
            assert cpu.reg_read(UC_X86_REG_EFLAGS) & 0x200, 'HLT without IRQs'
            assert self.get('ui_active', 1) == 0, 'active UI during quarantine'
            assert self.get('vc_session_owned', 1) == 1
            assert self.get('bios_calls') == self.get('init_calls') == 0
            assert self.frees == 0, 'compositor freed before engine drain'
            assert self.get('ui_page_pending', 1) == 1
            assert self.get('ui_front_base', 4) == 0x11223344
            self.halts += 1

    def interrupt(self, cpu, number, _):
        assert number == 0x21, f'unexpected interrupt {number:#x}'
        ax = cpu.reg_read(UC_X86_REG_AX)
        if ax >> 8 == 0x48:
            self.allocations += 1
            cpu.reg_write(UC_X86_REG_AX, 0x8000)
        else:
            assert ax >> 8 == 0x49
            assert self.get('vc_session_owned', 1) == 0, 'free before safe release'
            self.frees += 1
        cpu.reg_write(UC_X86_REG_EFLAGS, cpu.reg_read(UC_X86_REG_EFLAGS) & ~1)

    def call(self, name, *, flags=0x202, max_halts=20):
        cpu = self.cpu
        cpu.reg_write(UC_X86_REG_CS, BASE >> 4)
        cpu.reg_write(UC_X86_REG_DS, BASE >> 4)
        cpu.reg_write(UC_X86_REG_ES, BASE >> 4)
        cpu.reg_write(UC_X86_REG_SS, BASE >> 4)
        cpu.reg_write(UC_X86_REG_SP, 0xfffc)
        cpu.reg_write(UC_X86_REG_EFLAGS, flags)
        cpu.mem_write(BASE + 0xfffc, struct.pack('<H', self.symbols['stop'] - BASE))
        initial = {UC_X86_REG_EAX: 0x11223344, UC_X86_REG_EBX: 0x55667788,
                   UC_X86_REG_ECX: 0x12345678, UC_X86_REG_EDX: 0xabcdef01,
                   UC_X86_REG_ESI: 0x11112222, UC_X86_REG_EDI: 0x33334444,
                   UC_X86_REG_EBP: 0x77778888}
        for register, value in initial.items():
            cpu.reg_write(register, value)
        entry = self.symbols[name]
        stop = self.symbols['stop']
        for _ in range(max_halts + 1):
            cpu.emu_start(entry, stop, count=100000)
            entry = BASE + cpu.reg_read(UC_X86_REG_IP)
            if entry == stop:
                assert cpu.reg_read(UC_X86_REG_SP) == 0xfffe
                if name in ('ui_video_wait_release', 'ui_comp_begin', 'ui_comp_end'):
                    for register, value in initial.items():
                        assert cpu.reg_read(register) == value
                return True
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path,
                        default=ROOT / 'build/tests/gui-video-quarantine-cpu')
    args = parser.parse_args()
    out = args.output.resolve()
    assert ROOT / 'build' in out.parents
    out.mkdir(parents=True, exist_ok=True)
    binary = assemble(out)
    code = binary.read_bytes()
    checks = []
    for flags in (0x002, 0x202):
        gui = GUI(code)
        gui.set('release_remaining', 2)
        assert gui.call('ui_video_wait_release', flags=flags)
        assert gui.halts == 2 and gui.get('release_calls') == 3
        assert gui.get('vc_session_owned', 1) == 0
        assert (gui.cpu.reg_read(UC_X86_REG_EFLAGS) & 0x200) == flags & 0x200
        assert not gui.cpu.reg_read(UC_X86_REG_EFLAGS) & 1
        checks.append(f'quarantine resumes after drain and preserves IF={bool(flags & 0x200)}')
    gui = GUI(code)
    gui.set('release_remaining', 0xffff)
    assert not gui.call('ui_video_wait_release', max_halts=3)
    assert gui.halts == 4 and gui.get('vc_session_owned', 1) == 1
    checks.append('permanent busy engine remains halted without BIOS/free/init')
    gui = GUI(code)
    gui.set('ui_comp_seg', 0x8000)
    gui.set('ui_comp_capacity', 61440)
    gui.set('release_remaining', 3)
    assert gui.call('ui_comp_end')
    assert gui.halts == 3 and gui.frees == 1
    assert gui.get('ui_comp_seg') == gui.get('ui_comp_capacity') == 0
    assert gui.get('ui_page_enabled', 1) == gui.get('ui_page_pending', 1) == 0
    assert gui.get('ui_front_base', 4) == 0 and gui.get('vc_access_bytes', 4) == gui.get('vc_frame_bytes', 4)
    checks.append('compositor storage/page bookkeeping retire only after safe cleanup')
    gui = GUI(code)
    gui.set('ui_comp_seg', 0x8000)
    gui.set('release_remaining', 2)
    assert gui.call('ui_mode_switch')
    assert gui.halts == 2 and gui.frees == 1 and gui.get('init_calls') == 1
    assert gui.get('ui_mode_request') == 0 and gui.get('vc_graphics_transition', 1) == 0
    assert gui.get('ui_pointer_visible', 1) == 0 and gui.get('ui_active', 1) == 1
    checks.append('mode switch waits before freeing/reinitializing and drops old cursor')
    gui = GUI(code)
    assert gui.call('ui_pages_failure')
    assert gui.get('ui_active', 1) == 0 and gui.get('ui_comp_recover', 1) == 1
    assert gui.get('bios_calls') == 0 and gui.get('ui_front_base', 4) == 0x11223344
    checks.append('protected page failure requests full recovery without BIOS/bookkeeping reset')
    gui = GUI(code)
    gui.set('vc_lfb', 0, 1)
    assert gui.call('ui_pages_failure')
    assert gui.get('bios_calls') == 1 and gui.get('ui_front_base', 4) == 0
    checks.append('ordinary banked page failure retains established fallback behavior')
    gui = GUI(code)
    gui.set('bind_fail', 1, 1)
    gui.set('release_remaining', 2)
    assert gui.call('ui_comp_begin')
    assert gui.allocations == 1 and gui.halts == 2
    assert gui.get('ui_active', 1) == 0 and gui.get('ui_comp_recover', 1) == 1
    assert gui.get('vc_session_owned', 1) == 0 and gui.frees == 0
    assert gui.call('ui_comp_end') and gui.frees == 1
    checks.append('failed expanded protected binding drains before recovery frees compositor')
    record = {'status': 'PASS', 'checks': checks, 'count': len(checks),
              'scope': 'Actual NASM GUI instructions; fake monitor, IRQ wake and DOS allocation only',
              'fixture_sha256': hashlib.sha256(code).hexdigest(),
              'production_snapshot_sha256': hashlib.sha256((out / 'gui_video_quarantine.inc').read_bytes()).hexdigest()}
    (out / 'report.json').write_text(json.dumps(record, indent=2) + '\n')
    print(f'PASS: {len(checks)} GUI quarantine CPU scenarios')


if __name__ == '__main__':
    main()
