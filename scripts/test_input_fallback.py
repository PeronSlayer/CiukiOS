#!/usr/bin/env python3
"""Execute the kernel handoff and INPUTINI COM against modeled BIOS/DOS.

The actual production instructions handle controller initialization, IVT
handoff and driver launch. The fixture supplies a bounded 8042, a recognizable
firmware C2 endpoint and DOS EXEC results; it does not emulate CuteMouse or
claim physical firmware/hardware compatibility.
"""
import argparse
import json
import struct
from pathlib import Path

from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_INTR, UC_HOOK_INSN
from unicorn.x86_const import *
from test_ps2_boot import Controller, symbol
from test_ps2_rollback import FaultController


SEG, BASE, STOP = 0x900, 0x9000, 0xFF00


class LateFault(FaultController):
    def read(self, u, port, size, data):
        value = super().read(u, port, size, data)
        if port == 0x60 and self.mouse_reporting and self.fault == 'final-enable-write' and not self.fired:
            self.fired = True
            self.busy = 65535
        return value

    def write(self, u, port, size, value, data):
        super().write(u, port, size, value, data)
        if port == 0x64 and value == 0xAE and self.fault == 'final-enable-wait' and not self.fired:
            self.fired = True
            self.busy = 65535


def cpu():
    u = Uc(UC_ARCH_X86, UC_MODE_16)
    u.mem_map(0, 0x100000)
    for r in (UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES):
        u.reg_write(r, SEG)
    u.reg_write(UC_X86_REG_SS, 0x7000)
    return u


def enter_interrupt(u, n, _):
    sp = (u.reg_read(UC_X86_REG_SP) - 6) & 0xFFFF
    flags = u.reg_read(UC_X86_REG_EFLAGS)
    u.mem_write((u.reg_read(UC_X86_REG_SS) << 4) + sp,
                struct.pack('<HHH', u.reg_read(UC_X86_REG_IP),
                            u.reg_read(UC_X86_REG_CS), flags & 0xFFFF))
    u.reg_write(UC_X86_REG_SP, sp)
    off, seg = struct.unpack('<HH', u.mem_read(n * 4, 4))
    u.reg_write(UC_X86_REG_EFLAGS, flags & ~0x300)
    u.reg_write(UC_X86_REG_CS, seg)
    u.reg_write(UC_X86_REG_IP, off)


def call(u, address, flags=0x202, interrupt=False):
    u.reg_write(UC_X86_REG_CS, SEG)
    u.reg_write(UC_X86_REG_SP, 0xFFF0)
    frame = struct.pack('<HHH', STOP, SEG, flags) if interrupt else struct.pack('<H', STOP)
    u.mem_write(0x7FFF0, frame)
    u.reg_write(UC_X86_REG_EFLAGS, flags)
    u.emu_start(BASE + address, BASE + STOP, count=2500000)
    assert u.reg_read(UC_X86_REG_IP) == STOP, 'instruction budget exhausted'


def kernel_case(kernel, listing, fault, translation, flags):
    u = cpu()
    u.mem_write(BASE, kernel)
    s = lambda name: symbol(listing, name)
    controller = Controller(translation=translation) if fault is None else LateFault(fault, translation, 0)
    bios74 = struct.pack('<HH', 0x0300, 0xF000)
    u.mem_write(0x74 * 4, bios74)
    u.mem_write(0xF0300, b'\xcf')
    bios15 = struct.pack('<HH', 0x0100, 0xF000)
    u.mem_write(BASE+s('old_int15_off'), bios15)
    u.mem_write(0xF0100, b'\xb8\xef\xbe\xcf')  # firmware endpoint AX=BEEF; IRET
    u.mem_write(0x15 * 4, struct.pack('<HH', s('int15_handler'), SEG))
    u.hook_add(UC_HOOK_INTR, enter_interrupt)
    u.hook_add(UC_HOOK_INSN, controller.read, None, 1, 0, UC_X86_INS_IN)
    u.hook_add(UC_HOOK_INSN, controller.write, None, 1, 0, UC_X86_INS_OUT)
    call(u, s('init_stage2_services'), flags)
    failed = fault is not None
    assert u.reg_read(UC_X86_REG_EFLAGS) & 0x200 == flags & 0x200, 'caller IF changed'
    assert u.mem_read(BASE+s('mouse_hw_ready'), 1)[0] == int(not failed)
    assert u.mem_read(BASE+s('shell_exec_external_mouse_disabled'), 1)[0] == int(failed)
    int33 = struct.unpack('<HH', u.mem_read(0x33*4, 4))
    expected = s('int_default_iret') if failed else s('int33_handler')
    assert int33 == (expected, SEG), 'INT33 ownership not handed over correctly'
    assert bytes(u.mem_read(0x74*4, 4)) == (bios74 if failed else struct.pack('<HH', s('irq12_mouse_handler'), SEG))
    assert controller.config & 1 and not controller.config & 0x10, 'keyboard disabled'
    assert not controller.pic[0x21] & 2, 'IRQ1 masked'
    assert controller.config & 0x40 == translation, 'translation changed'
    assert bool(controller.pic[0xA1] & 0x10) == failed, 'IRQ12 ownership mismatch'
    assert bool(controller.config & 0x20) == failed, 'AUX clock state mismatch'
    u.reg_write(UC_X86_REG_AX, 0)
    call(u, int33[0], flags, interrupt=True)
    assert u.reg_read(UC_X86_REG_AX) == (0 if failed else 0xFFFF), 'reset lies about hardware'
    u.reg_write(UC_X86_REG_AX, 0xC204)
    call(u, s('int15_handler'), flags, interrupt=True)
    assert (u.reg_read(UC_X86_REG_AX) == 0xBEEF) == failed, 'C2 did not reach its owning handler'


def helper_case(binary, scenario):
    u = cpu()
    u.mem_write(BASE+0x100, binary)
    state = {'ready': scenario == 'already-ready', 'execs': 0, 'status_reads': 0, 'exit': None, 'resizes': 0}
    def cf(value):
        f = u.reg_read(UC_X86_REG_EFLAGS)
        u.reg_write(UC_X86_REG_EFLAGS, (f | 1) if value else (f & ~1))
    def cstring(ptr):
        return bytes(u.mem_read(ptr, 100)).split(b'\0', 1)[0]
    def interrupt(u, n, _):
        ax = u.reg_read(UC_X86_REG_AX)
        if n == 0x33:
            assert ax == 0
            u.reg_write(UC_X86_REG_AX, 0xFFFF if state['ready'] else 0)
            return
        assert n == 0x21, hex(n)
        ah = ax >> 8
        if ah == 0x4A:
            state['resizes'] += 1
            assert u.reg_read(UC_X86_REG_ES) == SEG
            assert u.reg_read(UC_X86_REG_BX) == (len(binary)+0x100+15)//16
            cf(scenario == 'no-memory')
        elif ah == 9:
            assert b'$' in u.mem_read((u.reg_read(UC_X86_REG_DS)<<4)+u.reg_read(UC_X86_REG_DX), 100)
        elif ah == 0x4B:
            assert ax == 0x4B00
            state['execs'] += 1
            path = (u.reg_read(UC_X86_REG_DS)<<4)+u.reg_read(UC_X86_REG_DX)
            assert cstring(path) == b'\\DRIVERS\\MOUSE\\CTMOUSE.EXE'
            pb = (u.reg_read(UC_X86_REG_ES)<<4)+u.reg_read(UC_X86_REG_BX)
            env, tail, ts, f1, fs1, f2, fs2 = struct.unpack('<7H', u.mem_read(pb, 14))
            assert env == 0 and ts == fs1 == fs2 == SEG
            assert bytes(u.mem_read((ts<<4)+tail, 11)) == b'\x09 /P /W /B\r'
            assert bytes(u.mem_read((fs1<<4)+f1, 16)) == bytes(u.mem_read((fs2<<4)+f2, 16)) == b'\0           \0\0\0\0'
            cf(scenario == 'exec-failed')
            state['ready'] = scenario == 'installed'
        elif ah == 0x4D:
            state['status_reads'] += 1
            u.reg_write(UC_X86_REG_AX, 0x0300)  # successful TSR termination
        elif ah == 0x4C:
            state['exit'] = ax & 255
            u.emu_stop()
        else:
            raise AssertionError(hex(ax))
    u.hook_add(UC_HOOK_INTR, interrupt)
    u.emu_start(BASE+0x100, BASE+0xF000, count=10000)
    expected = {'already-ready': 0, 'installed': 0, 'unavailable': 1, 'exec-failed': 2, 'no-memory': 3}[scenario]
    assert state['exit'] == expected, state
    assert state['execs'] == (0 if scenario in ('already-ready', 'no-memory') else 1), state
    assert state['status_reads'] == (1 if scenario in ('installed', 'unavailable') else 0), state
    assert state['resizes'] == (0 if scenario == 'already-ready' else 1), state


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--kernel', type=Path, required=True)
    p.add_argument('--listing', type=Path, required=True)
    p.add_argument('--helper', type=Path, required=True)
    p.add_argument('--old-kernel', type=Path)
    p.add_argument('--old-listing', type=Path)
    p.add_argument('--report', type=Path, required=True)
    a = p.parse_args()
    kernel, listing = a.kernel.read_bytes(), a.listing.read_text()
    cases = []
    for fault in (None, 'defaults-rejected', 'enable-rejected', 'first-read-timeout', 'initial-busy',
                  'final-enable-write', 'final-enable-wait'):
        for translation in (0, 0x40):
            for flags in (2, 0x202):
                kernel_case(kernel, listing, fault, translation, flags)
                cases.append([fault, translation, flags])
    for case in ('already-ready', 'installed', 'unavailable', 'exec-failed', 'no-memory'):
        helper_case(a.helper.read_bytes(), case)
    old_error = None
    if a.old_kernel:
        try:
            kernel_case(a.old_kernel.read_bytes(), a.old_listing.read_text(), 'defaults-rejected', 0x40, 0x202)
        except AssertionError as e:
            old_error = str(e) or 'failed native mouse was retained'
        assert old_error, 'negative control unexpectedly passed'
    a.report.write_text(json.dumps({'kernel_cases': cases, 'helper_cases': 5, 'negative_control': old_error,
                                   'scope': 'production instructions; modeled 8042, firmware C2 endpoint and DOS EXEC'}, indent=2)+'\n')
    print(f'PASS {len(cases)} kernel handoffs and 5 helper paths; old failure: {old_error}')


if __name__ == '__main__':
    main()
