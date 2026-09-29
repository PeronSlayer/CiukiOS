#!/usr/bin/env python3
"""Execute built DOSWIN/SHELL lifecycle instructions with refused JLM cleanup.

NASM assembles the production files unchanged. Unicorn executes their entry,
uninstall, release and UI guards; only JLM, BIOS, DOS allocator and unrelated
painting/audio endpoints are modeled. This is not QEMU or hardware acceptance.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_CODE, UC_HOOK_INTR, UC_HOOK_INSN
from unicorn.x86_const import *

ROOT = Path(__file__).resolve().parents[1]
RSEG, SSEG, STUBSEG = 0x2000, 0x4000, 0x9000
RBASE, SBASE, STOP = RSEG * 16, SSEG * 16, 0xF800
FIELDS = dict(installed=34, guest_live=37, errors=44, cg_active=192,
              gfx_buffer=194, host_mode=209, cleanup_op=144, cleanup_error=146)
OPS = dict(query=0, begin=1, end=2, unbind=5, state=0x21, enter=0x26, leave=0x27)


def symbols(text, origin=0):
    result, pending = {}, []
    for line in text.splitlines():
        label = re.search(r'(?:^|\s)([A-Za-z_][\w]*)(?=:|\s+(?:d[bwdq]|times)\s)', line)
        if label:
            pending.append(label[1])
        address = re.match(r'\s*\d+\s+([0-9A-F]{8})\s', line)
        if address:
            for name in pending:
                result[name] = int(address[1], 16) + origin
            pending.clear()
    return result


def build(source, output):
    subprocess.run(['nasm', '-f', 'bin', source, '-l', str(output.with_suffix('.lst')),
                    '-o', str(output)], cwd=ROOT, check=True)


class Machine:
    def __init__(self, runtime, listing, shell, shell_listing):
        self.u = u = Uc(UC_ARCH_X86, UC_MODE_16)
        u.mem_map(0, 0x100000)
        u.mem_write(RBASE, runtime)
        u.mem_write(SBASE + 0x100, shell)
        self.r = symbols(listing)
        self.s = symbols(shell_listing, 0x100)
        self.phase = self.r.get('vga_session', 148)  # old binary counterexample
        self.events, self.freed, self.serial = [], [], bytearray()
        self.failures, self.free_failures = {}, []
        self.active, self.bound, self.host = True, True, True
        self.old_vectors, self.owned_vectors = {}, {}
        self.halted_at = None
        self.original_if = 0x200
        self.bank = 0x30000
        self.old_keyboard_reads = 0
        self.stop_on_redraw = False
        for name, value in [('installed', 1), ('guest_live', 0), ('cg_active', 1), ('host_mode', 1)]:
            self.put(FIELDS[name], value)
        self.put(self.phase, 1)
        self.put(self.r['vga_host_entered'], 1)
        if 'vga_hooks' in self.r:
            self.put(self.r['vga_hooks'], 1)
        self.put(FIELDS['gfx_buffer'], 0x6000, 2)
        self.put(self.r['vga_host_packet'] + 4, 0x10000, 4)
        self.put(self.r['vga_entry'], 0, 2)
        self.put(self.r['vga_entry'] + 2, STUBSEG, 2)
        u.mem_write(STUBSEG * 16, b'\xcb')  # real far return from modeled JLM
        self.endpoints = {STUBSEG * 16: self.monitor}
        for index, (vector, old, handler) in enumerate([
                (8, 'old_int08', 'timer_handler'), (0x16, 'old_int16', 'keyboard_handler'),
                (0x33, 'old_int33', 'mouse_handler'), (0x21, 'vga_old_int21', 'vga_int21')]):
            target = 0x100 + index * 0x20
            pointer = struct.pack('<HH', target, STUBSEG)
            self.old_vectors[vector] = pointer
            self.owned_vectors[vector] = struct.pack('<HH', self.r[handler], RSEG)
            u.mem_write(RBASE + self.r[old], pointer)
            u.mem_write(vector * 4, self.owned_vectors[vector])
            u.mem_write(STUBSEG * 16 + target, b'\xcf')
        self.endpoints[STUBSEG * 16 + 0x120] = self.keyboard
        self.put(self.r['physical_mouse'], 0)  # absent firmware mouse endpoint
        self.sput('dw_segment', RSEG)
        self.sput('dw_entry', 0)
        self.sput('dw_entry', RSEG, offset=2)
        self.sput('ui_window_flags', 1, 1, 11)
        self.sput('ui_active_window', 11, 1)
        self.sput('dos_host_active', 1, 1)
        self.sput('ui_active', 1, 1)
        self.skip = {'ui_sfx_end', 'ui_sfx_begin', 'ui_windows_open'}
        self.forbidden = {'dw_load', 'app_suspend', 'wp_end', 'ui_assets_end', 'vc_end',
                          'ui_comp_end', 'ui_icons_end', 'restore_shell_video_state'}
        self.shell_stop = None
        u.hook_add(UC_HOOK_CODE, self.code)
        u.hook_add(UC_HOOK_INTR, self.interrupt)
        u.hook_add(UC_HOOK_INSN, lambda *_: 0x20, None, 1, 0, UC_X86_INS_IN)
        u.hook_add(UC_HOOK_INSN, self.port_out, None, 1, 0, UC_X86_INS_OUT)

    def put(self, address, value, size=1):
        self.u.mem_write(RBASE + address, value.to_bytes(size, 'little'))

    def get(self, address, size=1):
        return int.from_bytes(self.u.mem_read(RBASE + address, size), 'little')

    def sput(self, name, value, size=2, offset=0):
        self.u.mem_write(SBASE + self.s[name] + offset, value.to_bytes(size, 'little'))

    def sget(self, name, size=2, offset=0):
        return int.from_bytes(self.u.mem_read(SBASE + self.s[name] + offset, size), 'little')

    def flags(self, failed):
        self.u.reg_write(UC_X86_REG_EFLAGS, (self.u.reg_read(UC_X86_REG_EFLAGS) & ~1) | int(bool(failed)))

    def vectors(self):
        return {n: bytes(self.u.mem_read(n * 4, 4)) for n in self.old_vectors}

    def monitor(self):
        u = self.u
        operation = u.reg_read(UC_X86_REG_AX)
        self.events.append(operation)
        error = self.failures.get(operation, [])
        failure = error.pop(0) if error else 0
        if not failure:
            if operation == OPS['state']:
                assert self.active
                self.put(self.r['vga_state'] + 152, self.bank, 4)
            elif operation == OPS['leave']:
                assert self.host
                self.host = False
            elif operation == OPS['enter']:
                assert self.active and not self.host
                self.host = True
            elif operation == OPS['end']:
                assert self.active and not self.host
                assert self.vectors() == self.owned_vectors, 'vectors changed before END succeeded'
                self.active = False
            elif operation == OPS['unbind']:
                assert not self.active, 'UNBIND attempted on live session'
                assert self.bound
                self.bound = False
            else:
                raise AssertionError(f'unexpected JLM operation {operation:#x}')
        u.reg_write(UC_X86_REG_AX, failure)
        u.reg_write(UC_X86_REG_BX, 0xBEEF)  # ABI caller must not assume BX survives
        self.flags(failure)

    def keyboard(self):
        self.old_keyboard_reads += 1
        assert self.get(FIELDS['installed']) == 1
        assert self.get(self.phase) != 0
        assert self.freed == []
        assert self.serial, 'unsafe recovery did not report its retained ownership'
        self.u.reg_write(UC_X86_REG_AX, 0x1C0D)

    def port_out(self, cpu, port, size, value, _):
        assert port in (0xE9, 0x3F8)
        if port == 0xE9:
            self.serial.append(value & 255)

    def near_return(self):
        u = self.u
        sp = u.reg_read(UC_X86_REG_SP)
        ip = int.from_bytes(u.mem_read(u.reg_read(UC_X86_REG_SS) * 16 + sp, 2), 'little')
        u.reg_write(UC_X86_REG_SP, sp + 2)
        u.reg_write(UC_X86_REG_IP, ip)

    def code(self, cpu, address, size, _):
        if address in self.endpoints:
            self.endpoints[address]()
        if address == SBASE + self.s['ui_redraw'] and self.stop_on_redraw:
            self.halted_at = 'ui_redraw'
            cpu.emu_stop()
        for name in self.skip:
            if address == SBASE + self.s[name]:
                self.events.append(name)
                self.near_return()
                return
        for name in self.forbidden:
            if address == SBASE + self.s[name]:
                raise AssertionError('retained owner reached ' + name)

    def interrupt(self, cpu, number, _):
        ax = cpu.reg_read(UC_X86_REG_AX)
        if number == 0x10:
            assert ax == 0x4F05, hex(ax)
            assert not self.active, 'BIOS bank resync before END'
            assert self.vectors() == self.old_vectors
            assert cpu.reg_read(UC_X86_REG_DX) == self.bank // 0x10000
            self.events.append('resync')
            cpu.reg_write(UC_X86_REG_AX, 0x004F)
            return
        if number == 0x21 and ax >> 8 == 0x49:
            segment = cpu.reg_read(UC_X86_REG_ES)
            assert not self.active and not self.bound, 'free before monitor release'
            assert self.vectors() == self.old_vectors
            failure = self.free_failures.pop(0) if self.free_failures else 0
            if not failure:
                assert segment not in self.freed, 'double DOS free'
                self.freed.append(segment)
            cpu.reg_write(UC_X86_REG_AX, failure)
            self.flags(failure)
            return
        raise AssertionError(f'unexpected interrupt {number:#x} AX={ax:#x}')

    def run(self, name='runtime_entry', operation=1, shell=False, far=True):
        u = self.u
        segment, base, syms = (SSEG, SBASE, self.s) if shell else (RSEG, RBASE, self.r)
        for reg in (UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES):
            u.reg_write(reg, segment)
        u.reg_write(UC_X86_REG_SS, 0x7000)
        u.reg_write(UC_X86_REG_SP, 0xF000)
        u.reg_write(UC_X86_REG_AX, operation)
        u.reg_write(UC_X86_REG_EFLAGS, 0x202)
        u.mem_write(0x7F000, struct.pack('<HH', STOP, segment) if far else struct.pack('<H', STOP))
        u.emu_start(base + syms[name], base + STOP, count=2000000)
        assert self.halted_at or u.reg_read(UC_X86_REG_IP) == STOP, 'CPU instruction budget exhausted'
        assert u.reg_read(UC_X86_REG_EFLAGS) & 0x200, 'caller IF lost'
        return bool(u.reg_read(UC_X86_REG_EFLAGS) & 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/tests/dos-window-lifecycle-2026-09-27/current')
    parser.add_argument('--runtime', type=Path)
    parser.add_argument('--runtime-listing', type=Path)
    parser.add_argument('--shell', type=Path)
    parser.add_argument('--shell-listing', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    runtime = args.runtime or args.output / 'doswin.drv'
    shell = args.shell or args.output / 'shell.com'
    if not args.runtime:
        build('src/com/dos_window_runtime.asm', runtime)
    if not args.shell:
        build('src/com/shell.asm', shell)
    payload = (runtime.read_bytes(), (args.runtime_listing or runtime.with_suffix('.lst')).read_text(),
               shell.read_bytes(), (args.shell_listing or shell.with_suffix('.lst')).read_text())
    report = dict(scope=__doc__, runtime_sha256=hashlib.sha256(payload[0]).hexdigest(),
                  shell_sha256=hashlib.sha256(payload[2]).hexdigest(), runtime_bytes=len(payload[0]),
                  shell_bytes=len(payload[2]), shell_limit=0xEE00, cases=[], qemu=False, physical_hardware=False)
    factory = lambda: Machine(*payload)
    def passed(name):
        report['cases'].append(name)
    try:
        m = factory(); m.failures[2] = [0x22]
        assert m.run(), 'END refusal did not propagate CF'
        assert m.get(FIELDS['installed']) == 1 and m.get(m.phase) == 1, 'END refusal discarded owner'
        assert m.active and m.bound and m.host and m.vectors() == m.owned_vectors
        assert m.get(FIELDS['errors'], 2) & 0x80
        assert m.get(144, 2) == 2 and m.get(146, 2) == 0x22
        passed('END_refusal_retains_owner_hooks_mapping_and_exact_diagnostic')
        m.bank = 0x50000
        assert not m.run()
        assert not m.active and not m.bound and m.get(FIELDS['installed']) == 0
        assert m.events.count(2) == 2 and m.events.count(5) == 1
        assert m.vectors() == m.old_vectors
        passed('END_retry_rereads_changed_host_bank_then_releases_once')
        m = factory(); m.failures[5] = [8]
        assert m.run() and not m.active and m.bound
        assert m.get(FIELDS['installed']) == 1 and m.get(m.phase) == 2
        assert m.vectors() == m.old_vectors and 'resync' in m.events
        assert m.get(144, 2) == 5 and m.get(146, 2) == 8
        assert not m.run()
        assert m.events.count(2) == 1 and m.events.count(5) == 2
        passed('UNBIND_refusal_keeps_allocation_and_retry_never_repeats_END')
        m = factory(); assert not m.run()
        assert m.events == [0x21, 0x27, 2, 'resync', 5], m.events
        assert not m.run() and m.events.count(2) == 1
        passed('normal_uninstall_order_and_idempotent_finished_entry')
        m = factory(); m.put(37, 1)
        assert m.run() and m.events == []
        m.put(37, 0)
        assert m.run(operation=0) and m.events == []
        passed('live_guest_cleanup_and_second_install_rejected')
        m = factory(); m.failures[2] = [6]; m.failures[0x26] = [0x24]
        assert not m.run()
        assert m.old_keyboard_reads == 1 and b'display paused' in m.serial
        assert m.events.count(2) == 2 and m.events.count(5) == 1 and m.freed == []
        passed('partial_cleanup_host_recovery_refusal_waits_saved_keyboard_without_rendering')
        for failed_op in (2, 5):
            m = factory(); m.failures[failed_op] = [8]
            assert m.run('dw_release', shell=True, far=False)
            assert m.sget('dw_segment') == RSEG and not m.freed
            assert m.sget('dw_status') == m.s['dw_retained']
            assert not m.run('dw_release', shell=True, far=False)
            assert m.freed == [0x6000, RSEG] and m.sget('dw_segment') == 0
            assert m.events.count(2) == (2 if failed_op == 2 else 1)
            passed(f'shell_release_retains_then_retries_operation_{failed_op}')
        m = factory(); m.free_failures = [0, 7]
        assert m.run('dw_release', shell=True, far=False)
        assert m.sget('dw_segment') == RSEG and m.get(194, 2) == 0
        assert not m.run('dw_release', shell=True, far=False)
        assert m.freed == [0x6000, RSEG]
        passed('DOS_module_free_failure_does_not_double_free_graphics_buffer')
        for name in ('dw_launch', 'ui_to_dos', 'ui_queue_command'):
            m = factory(); m.failures[2] = [0x22]; m.stop_on_redraw = True
            m.run(name, shell=True, far=False)
            assert m.halted_at == 'ui_redraw' and not m.freed and m.sget('dw_segment') == RSEG
            assert m.sget('dw_status') == m.s['dw_retained'] and m.active and m.host
            assert 'ui_sfx_begin' not in m.events
            assert m.u.reg_read(UC_X86_REG_SP) == 0xF000, 'retained UI path leaked stack'
            passed(name + '_blocks_new_allocation_or_mode_exit_with_retry_message')
        m = factory(); m.failures[2] = [0x22]
        assert m.run('ui_windows_close', shell=True, far=False)
        assert m.sget('ui_window_flags', 1, 11) == 1 and not m.freed
        passed('close_keeps_retry_window_visible_when_cleanup_refused')
        assert len(payload[2]) <= 0xEE00
        report['result'] = 'PASS'
    except Exception as exc:
        report['result'], report['error'] = 'FAIL', str(exc)
        raise
    finally:
        (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'[dos-window-lifecycle] PASS {len(report["cases"])} actual-binary cases; no QEMU/hardware claim')


if __name__ == '__main__':
    main()
