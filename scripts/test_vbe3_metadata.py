#!/usr/bin/env python3
"""Run production VBE descriptor and mode selection instructions in Unicorn.

BIOS responses deliberately give linear and banked access different pitch,
page count and RGB channel positions. LFB eligibility/A20 opening is a test
boundary: its actual machine-state implementation has independent tests.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_INTR, UC_HOOK_CODE
from unicorn.x86_const import *

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x10000
MODE = 0x140


def descriptor():
    info = bytearray(256)
    for offset, value in ((0, 0x99), (4, 64), (6, 64), (8, 0xA000),
                          (16, 3200), (18, 800), (20, 600), (50, 3328)):
        struct.pack_into('<H', info, offset, value)
    info[2] = 7
    info[24:28] = bytes((1, 32, 1, 6))
    info[29] = 4
    info[31:39] = bytes((8, 16, 8, 8, 8, 0, 8, 24))
    struct.pack_into('<I', info, 40, 0xE0000000)
    info[52:54] = bytes((0, 1))
    info[54:62] = bytes((8, 0, 8, 8, 8, 16, 8, 24))
    return info


def xga_descriptor(depth=32):
    """VBE2 geometry from the failing T23 profile, with its checked stride."""
    info = descriptor()
    struct.pack_into('<HHH', info, 16, 1024*((depth+7)//8), 1024, 768)
    info[25] = depth
    if depth == 16:
        info[31:39] = bytes((5, 11, 6, 5, 5, 0, 0, 0))
    info[50:62] = bytes(12)
    return info


class Video:
    def __init__(self, binary, info=None, version=0x300, open_ok=True, set_ok=True,
                 reported_linear=None, reported_mode=None, mode_status=0x004F,
                 msw_pe=None, session_present=False, session_caps=0x90000,
                 session_active=0, session_magic=0x534D5643,
                 session_fail_bind_modes=(), session_retain=False,
                 session_unbind_ok=True, session_io_ok=True,
                 session_display_info=None, session_display_ok=True,
                 scanline_result=None):
        self.cpu = Uc(UC_ARCH_X86, UC_MODE_16)
        self.cpu.mem_map(0, 0x100000)
        self.cpu.mem_write(BASE+0x100, binary)
        pos = binary.index(b'VBM1')+4
        self.symbols = {}
        while True:
            address = struct.unpack_from('<H', binary, pos)[0]
            pos += 2
            if not address:
                break
            end = binary.index(0, pos)
            self.symbols[binary[pos:end].decode()] = BASE+address
            pos = end+1
        self.info = bytes(descriptor() if info is None else info)
        self.version = version
        self.open_ok, self.set_ok = open_ok, set_ok
        self.reported_linear = reported_linear
        self.reported_mode = reported_mode
        self.mode_status = mode_status
        self.scanline_result = scanline_result
        self.msw_pe = msw_pe
        self.session_present = session_present
        self.session_caps = session_caps
        self.session_active = session_active
        self.session_magic = session_magic
        self.session_fail_bind_modes = set(session_fail_bind_modes)
        self.session_retain = session_retain
        self.session_unbind_ok = session_unbind_ok
        self.session_io_ok = session_io_ok
        self.session_display_info = session_display_info
        self.session_display_ok = session_display_ok
        self.session_binding = None
        self.session_calls = []
        self.bind_packets = []
        self.row_packets = []
        self.fill_packets = []
        self.local_limit_calls = self.bank_map_calls = 0
        self.session_queries = 0
        self.smsw_calls = 0
        self.bios_calls = []
        self.set_calls = []
        self.open_calls = self.close_calls = self.allocations = self.frees = 0
        self.linear = False
        self.controller = bytearray(512)
        self.controller[:4] = b'VESA'
        struct.pack_into('<H', self.controller, 4, version)
        struct.pack_into('<H', self.controller, 18, 512)
        self.cpu.mem_write(self.symbols['vc_info'], self.info)
        self.cpu.mem_write(self.symbols['vc_controller'], bytes(self.controller))
        self.set('vc_mode', MODE)
        self.cpu.hook_add(UC_HOOK_INTR, self.interrupt)
        self.cpu.hook_add(UC_HOOK_CODE, self.boundary)

    def set(self, name, value, size=2):
        self.cpu.mem_write(self.symbols[name], value.to_bytes(size, 'little'))

    def get(self, name, size=2):
        return int.from_bytes(self.cpu.mem_read(self.symbols[name], size), 'little')

    def carry(self, enabled):
        flags = self.cpu.reg_read(UC_X86_REG_EFLAGS)
        self.cpu.reg_write(UC_X86_REG_EFLAGS, flags | 1 if enabled else flags & ~1)

    def boundary(self, cpu, address, _size, _data):
        if address == 0xF0100:
            self.monitor_call(cpu)
            return
        if address == self.symbols['vc_fb_limits']:
            self.local_limit_calls += 1
            assert not self.msw_pe, 'A V86 client attempted a local CR0 switch'
        if address == self.symbols['vc_map']:
            self.bank_map_calls += 1
        if self.msw_pe is not None and cpu.mem_read(address, 3) == b'\x0f\x01\xe0':
            # Exercise the PE branch without making Unicorn enter protected
            # mode or requiring the fixture to install a GDT.
            self.smsw_calls += 1
            cpu.reg_write(UC_X86_REG_AX, self.msw_pe)
            cpu.reg_write(UC_X86_REG_IP, (cpu.reg_read(UC_X86_REG_IP)+3) & 0xffff)
            return
        if address not in (self.symbols['vc_lfb_open'], self.symbols['vc_lfb_close']):
            return
        if address == self.symbols['vc_lfb_open']:
            self.open_calls += 1
            self.carry(not self.open_ok)
        else:
            self.close_calls += 1
        stack = cpu.reg_read(UC_X86_REG_SS)*16+cpu.reg_read(UC_X86_REG_SP)
        target = int.from_bytes(cpu.mem_read(stack, 2), 'little')
        cpu.reg_write(UC_X86_REG_SP, cpu.reg_read(UC_X86_REG_SP)+2)
        cpu.reg_write(UC_X86_REG_IP, target)

    def monitor_call(self, cpu):
        """Supply the protected service boundary; execute every client instruction."""
        operation = cpu.reg_read(UC_X86_REG_AX)
        assert operation & 0x100, 'Desktop calls must preserve the system VM'
        operation &= ~0x100
        self.session_calls.append(operation)
        pointer = cpu.reg_read(UC_X86_REG_ES)*16+cpu.reg_read(UC_X86_REG_DI)
        failed = False
        if operation == 0:  # CVMS
            assert cpu.reg_read(UC_X86_REG_CX) == 64
            packet = bytearray(64)
            struct.pack_into('<IHHII', packet, 0, self.session_magic,
                             0x100, 64, self.session_caps, self.session_active)
            if self.session_binding is not None:
                physical, extent = self.session_binding
                struct.pack_into('<III', packet, 40, 1, extent, physical)
            cpu.mem_write(pointer, bytes(packet))
        elif operation == 4:  # CVFB
            assert self.get('vc_session_owned', 1) == 1, 'BIND needs provisional ownership'
            packet = bytes(cpu.mem_read(pointer, 32))
            assert cpu.reg_read(UC_X86_REG_CX) == 32
            assert struct.unpack_from('<IHH', packet) == (0x42465643, 0x100, 32)
            self.bind_packets.append(packet)
            failed = self.get('vc_mode') in self.session_fail_bind_modes
            if not failed or self.session_retain:
                self.session_binding = struct.unpack_from('<II', packet, 8)
            cpu.reg_write(UC_X86_REG_BX, 0)  # software protected transport only
        elif operation == 5:  # Idempotent UNBIND, or retained engine quarantine.
            failed = not self.session_unbind_ok
            if not failed:
                self.session_binding = None
        elif operation == 0x14:  # CVGD read-only diagnostics / panel contract.
            assert cpu.reg_read(UC_X86_REG_CX) == 192
            failed = not self.session_display_ok or self.session_active != 0
            if not failed:
                if self.session_display_info is None:
                    packet = bytearray(192)
                    struct.pack_into('<IHH', packet, 0, 0x44475643, 0x100, 192)
                else:
                    packet = self.session_display_info
                assert len(packet) == 192
                cpu.mem_write(pointer, bytes(packet))
        elif operation in (0x11, 0x15):
            packet = bytes(cpu.mem_read(pointer, 32))
            assert cpu.reg_read(UC_X86_REG_CX) == 32
            (self.row_packets if operation == 0x11 else self.fill_packets).append(packet)
            failed = not self.session_io_ok or self.session_binding is None
        else:
            raise AssertionError(('CVSESSION operation', hex(operation)))
        cpu.reg_write(UC_X86_REG_AX, 8 if failed else 0)
        self.carry(failed)
        stack = cpu.reg_read(UC_X86_REG_SS)*16+cpu.reg_read(UC_X86_REG_SP)
        ip, cs = struct.unpack('<HH', cpu.mem_read(stack, 4))
        cpu.reg_write(UC_X86_REG_SP, cpu.reg_read(UC_X86_REG_SP)+4)
        cpu.reg_write(UC_X86_REG_CS, cs)
        cpu.reg_write(UC_X86_REG_IP, ip)

    def interrupt(self, cpu, number, _):
        ax = cpu.reg_read(UC_X86_REG_AX)
        if number == 0x2F:
            if ax == 0x1684:
                self.session_queries += 1
                assert cpu.reg_read(UC_X86_REG_ES) == 0
                assert cpu.reg_read(UC_X86_REG_DI) == 0
                if self.session_present:
                    cpu.reg_write(UC_X86_REG_ES, 0xF000)
                    cpu.reg_write(UC_X86_REG_DI, 0x0100)
                return
            return
        if number == 0x33:
            assert ax == 2
            return
        if number == 0x21:
            ah = cpu.reg_read(UC_X86_REG_AH)
            if ah == 0x48:
                self.allocations += 1
                cpu.reg_write(UC_X86_REG_AX, 0x3000)
            elif ah == 0x49:
                self.frees += 1
            else:
                raise AssertionError(('DOS function', ah))
            self.carry(False)
            return
        assert number == 0x10, number
        self.bios_calls.append(ax)
        buffer = cpu.reg_read(UC_X86_REG_ES)*16+cpu.reg_read(UC_X86_REG_DI)
        if ax == 0x4F00:
            cpu.mem_write(buffer, bytes(self.controller))
        elif ax == 0x4F01:
            cpu.mem_write(buffer, self.info)
        elif ax == 0x4F02:
            bx = cpu.reg_read(UC_X86_REG_BX)
            self.set_calls.append(bx)
            assert bx & 0x3FFF == MODE
            if bx & 0x4000 and not self.set_ok:
                cpu.reg_write(UC_X86_REG_AX, 0x014F)
                return
            self.linear = bool(bx & 0x4000)
        elif ax == 0x4F03:
            linear = self.linear if self.reported_linear is None else self.reported_linear
            mode = MODE if self.reported_mode is None else self.reported_mode
            cpu.reg_write(UC_X86_REG_BX, mode | (0x4000 if linear else 0))
            cpu.reg_write(UC_X86_REG_AX, self.mode_status)
            return
        elif ax == 0x4F06:
            # Active pitch is allowed to differ from the mode-info default.
            if self.scanline_result is None:
                pitch = 3456 if self.linear else 3216
                status, pixels, rows = 0x004F, pitch//4, 2048
            else:
                status, pitch, pixels, rows = self.scanline_result
            cpu.reg_write(UC_X86_REG_BX, pitch)
            cpu.reg_write(UC_X86_REG_CX, pixels)
            cpu.reg_write(UC_X86_REG_DX, rows)
            cpu.reg_write(UC_X86_REG_AX, status)
            return
        elif ax == 0x4F07:
            pass
        elif ax == 0x1130:
            cpu.reg_write(UC_X86_REG_ES, 0xF000)
            cpu.reg_write(UC_X86_REG_BP, 0)
            return
        elif ax == 3:
            return
        else:
            raise AssertionError(('BIOS function', hex(ax)))
        cpu.reg_write(UC_X86_REG_AX, 0x004F)

    def call(self, name, **registers):
        cpu = self.cpu
        for register in (UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS):
            cpu.reg_write(register, BASE//16)
        cpu.reg_write(UC_X86_REG_SP, 0xEFFC)
        cpu.reg_write(UC_X86_REG_EFLAGS, 0x202)
        cpu.mem_write(BASE+0xEFFC, struct.pack('<H', self.symbols['stop']-BASE))
        for register, value in registers.items():
            cpu.reg_write(globals()['UC_X86_REG_'+register.upper()], value)
        cpu.emu_start(self.symbols[name], self.symbols['stop'], count=100000)
        assert cpu.reg_read(UC_X86_REG_IP) == self.symbols['stop']-BASE
        return bool(cpu.reg_read(UC_X86_REG_EFLAGS) & 1)

    def red(self):
        self.call('vc_set_palette', dx=self.symbols['vc_palette']-BASE, cx=16, bx=0)
        return int.from_bytes(self.cpu.mem_read(self.symbols['vc_colors']+4*4, 4), 'little')

    def pages(self):
        return self.cpu.mem_read(self.symbols['vc_info']+29, 1)[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT/'build/full/ui-smooth-2026-09-25/vbe3-metadata')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    binary = out/'fixture.com'
    subprocess.run(['nasm', '-f', 'bin', 'scripts/fixtures/vbe3_metadata_cpu.asm',
                    '-o', str(binary)], cwd=ROOT, check=True)
    code = binary.read_bytes()
    checks = []
    def passed(name):
        checks.append(name)
        print('PASS '+name, flush=True)

    video = Video(code)
    assert not video.call('vc_validate')
    assert (video.get('vc_bank_ok', 1), video.get('vc_lfb_ok', 1)) == (1, 1)
    assert video.get('vc_pitch') == 3328 and video.pages() == 1 and video.red() == 0xAA
    passed('VBE3 linear pitch, page count and swapped RGB masks used together')

    for bad_linear in (False, True):
        info = descriptor()
        position = 54 if bad_linear else 31
        info[position:position+6] = bytes((8, 0, 8, 0, 8, 0))
        struct.pack_into('<H', info, 50 if bad_linear else 16, 0)
        video = Video(code, info)
        assert not video.call('vc_validate')
        expected = (1, 0) if bad_linear else (0, 1)
        assert (video.get('vc_bank_ok', 1), video.get('vc_lfb_ok', 1)) == expected
        assert video.red() == (0xAA0000 if bad_linear else 0xAA)
    passed('Invalid bank layout cannot reject valid LFB; invalid LFB cannot poison bank fallback')

    for bad_bank, bad_linear in ((True, False), (False, True), (True, True)):
        info = descriptor()
        if bad_bank:
            info[32] = 248  # an 8-bit red field cannot start at bit 248
        if bad_linear:
            info[55] = 248  # 8+248 used to wrap the 8-bit endpoint to zero
        video = Video(code, info)
        rejected = video.call('vc_validate')
        assert rejected == (bad_bank and bad_linear)
        if not rejected:
            assert (video.get('vc_bank_ok', 1), video.get('vc_lfb_ok', 1)) == (
                int(not bad_bank), int(not bad_linear))
            assert video.red() == (0xAA0000 if bad_linear else 0xAA)
    passed('RGB endpoint overflow rejected independently for banked and linear layouts')

    info = descriptor()
    info[50:62] = bytes(12)
    video = Video(code, info, version=0x200)
    assert not video.call('vc_validate')
    assert video.get('vc_pitch') == 3200 and video.pages() == 4 and video.red() == 0xAA0000
    passed('VBE2 ignores absent VBE3 extension and retains original shared layout')

    for open_ok, set_ok in ((True, True), (False, True), (True, False)):
        video = Video(code, open_ok=open_ok, set_ok=set_ok)
        assert not video.call('vc_begin')
        linear = open_ok and set_ok
        assert video.get('vc_lfb', 1) == int(linear)
        assert video.get('vc_pitch') == (3456 if linear else 3216)
        assert video.pages() == (1 if linear else 0)
        assert video.red() == (0xAA if linear else 0xAA0000)
        expected = [0x4140] if linear else ([0x140] if not open_ok else [0x4140, 0x140])
        assert video.set_calls == expected, video.set_calls
        assert not video.call('vc_end')
        assert video.allocations == video.frees == 1
    passed('Actual vc_begin handles LFB success, opener refusal and 4F02 refusal with correct bank format')

    for open_ok, reported_linear in ((True, False), (False, True)):
        video = Video(code, open_ok=open_ok, reported_linear=reported_linear)
        assert video.call('vc_begin'), 'BIOS reported the wrong framebuffer access model'
        assert video.get('vc_active', 1) == 0 and video.get('vc_lfb', 1) == 0
        assert video.allocations == video.frees == 1
    passed('4F03 access-model mismatch rejects both directions and cleans up before any rendering')

    # INT 2F/1684 requires ES:DI=0. Without a provider, the multiplex chain
    # leaves those input registers unchanged; nonzero caller scratch values
    # must not be mistaken for a returned CVSESSION entry or cached.
    absent = Video(code)
    saved = {'ax': 0x1111, 'bx': 0x2222, 'cx': 0x3333,
             'dx': 0x4444, 'si': 0x5555, 'bp': 0x6666}
    for caller_es, caller_di in ((0x2345, 0x6789), (0x3456, 0x789A)):
        assert absent.call('vc_session_find', **saved, es=caller_es, di=caller_di)
        assert absent.cpu.reg_read(UC_X86_REG_ES) == caller_es
        assert absent.cpu.reg_read(UC_X86_REG_DI) == caller_di
        for name, value in saved.items():
            assert absent.cpu.reg_read(globals()['UC_X86_REG_'+name.upper()]) == value
    assert absent.session_queries == 2
    assert absent.get('vc_session_entry', 4) == 0
    passed('Absent INT 2F/1684 provider cannot cache caller ES:DI and preserves both registers')

    # A present provider returns a valid far entry, which should be cached;
    # the second lookup must use that cache and still preserve caller ES:DI.
    present = Video(code, session_present=True)
    for caller_es, caller_di in ((0x2345, 0x6789), (0x3456, 0x789A)):
        assert not present.call('vc_session_find', **saved, es=caller_es, di=caller_di)
        assert present.cpu.reg_read(UC_X86_REG_ES) == caller_es
        assert present.cpu.reg_read(UC_X86_REG_DI) == caller_di
        for name, value in saved.items():
            assert present.cpu.reg_read(globals()['UC_X86_REG_'+name.upper()]) == value
    assert present.session_queries == 1
    assert present.get('vc_session_entry', 4) == 0xF0000100
    passed('Present INT 2F/1684 far entry is cached and caller ES:DI remains intact')

    # Pointer discovery alone is insufficient. V86 clients need both complete
    # pixel transports, a well-formed contract, and the system VM context.
    for options in ({'session_caps': 0x10000}, {'session_caps': 0x80000},
                    {'session_active': 1}, {'session_magic': 0}):
        video = Video(code, msw_pe=1, session_present=True, **options)
        assert not video.call('vc_validate')
        assert (video.get('vc_lfb_ok', 1), video.get('vc_bank_ok', 1)) == (0, 1)
        assert video.open_calls == video.local_limit_calls == 0
    passed('V86 LFB eligibility rejects incomplete, malformed and non-system CVSESSION contracts')

    video = Video(code, msw_pe=1, session_present=True)
    assert not video.call('vc_validate')
    assert (video.get('vc_lfb_ok', 1), video.get('vc_bank_ok', 1)) == (1, 1)
    assert not video.call('vc_begin')
    assert video.get('vc_lfb', 1) == 2 and video.get('vc_active', 1) == 1
    assert video.set_calls == [0x4140]
    assert video.open_calls == video.local_limit_calls == 0
    packet = video.bind_packets[-1]
    assert struct.unpack_from('<IIHHHHII', packet, 8) == (
        0xE0000000, video.get('vc_access_bytes', 4), 800, 600, 3456, 32,
        32*1024*1024, 0)
    assert video.get('vc_session_owned', 1) == video.get('vc_session_bound', 1) == 1
    passed('Complete CVSESSION contract enables BIOS-verified true LFB in V86 without local CR0 or fake bank alias')

    video.call('vc_fb_begin')
    video.call('vc_fb_put', fs=0x4000, si=0x100, edi=64, cx=32)
    video.call('vc_fb_get', fs=0x4000, si=0x100, edi=64, cx=32)
    video.call('vc_fb_fill', eax=0x12345678, ecx=8, edi=128)
    video.call('vc_fb_end')
    assert len(video.row_packets) == 2 and len(video.fill_packets) == 1
    for direction, packet in zip((1, 0), video.row_packets):
        assert struct.unpack_from('<IHH', packet) == (0x52465643, 0x100, 32)
        assert struct.unpack_from('<HHHHHHII', packet, 8) == (
            0x4000, 0x100, 32, 1, 32, direction, 3456, 64)
    assert struct.unpack('<IHHIIIIII', video.fill_packets[0]) == (
        0x46465643, 0x100, 32, 128, 8, 0x12345678, 4, 0, 0)
    assert video.local_limit_calls == video.bank_map_calls == 0
    before = list(video.bios_calls)
    assert video.call('vc_map', edi=0)
    assert video.bios_calls == before, 'Protected LFB must reject even direct bank-map calls'
    passed('Protected pointer read/write, row and exact-colour fill packets avoid BIOS banks and local CR0 switches')

    # Cached bindings can be lost. Rebind on the next transfer must remain a
    # true linear binding, with failure quarantined instead of banked writes.
    video.session_binding = None
    video.set('vc_session_bound', 0, 1)
    video.call('vc_fb_put', fs=0x4000, si=0x100, edi=64, cx=32)
    assert len(video.bind_packets) == 2 and len(video.row_packets) == 3
    assert video.get('vc_active', 1) == 1
    video.session_io_ok = False
    video.call('vc_fb_get', fs=0x4000, si=0x100, edi=64, cx=32)
    assert video.get('vc_active', 1) == 0
    assert video.bank_map_calls == 1 and video.local_limit_calls == 0
    assert not video.call('vc_end') and video.get('vc_session_owned', 1) == 0
    passed('Lost protected binding rebinds safely; failed protected I/O disables painting without local or bank fallback')

    for fail_rebind in (False, True):
        fill = Video(code, msw_pe=1, session_present=True)
        assert not fill.call('vc_begin')
        if fail_rebind:
            fill.set('vc_session_bound', 0, 1)
            fill.session_binding = None
            fill.session_fail_bind_modes = {MODE}
        else:
            fill.session_io_ok = False
        assert fill.call('vc_fb_fill', eax=0x12345678, ecx=8, edi=128)
        assert fill.get('vc_active', 1) == 0
        calls = list(fill.session_calls)
        fill.session_io_ok = True
        assert fill.call('vc_fb_fill', eax=0x12345678, ecx=8, edi=128)
        assert fill.call('vc_fb_put_rows', fs=0x4000, si=0x100,
                         edi=128, cx=32, bx=32, dx=2)
        assert fill.call('vc_fb_get_rows', fs=0x4000, si=0x100,
                         edi=128, cx=32, bx=32, dx=2)
        assert fill.session_calls == calls, 'Inactive protected fill must not paint or rebind'
        assert fill.bank_map_calls == fill.local_limit_calls == 0
        assert not fill.call('vc_end')
    passed('Protected FILL and rebind failures block all fills and batched rows until a complete mode restart')

    for retained in (False, True):
        failed = Video(code, msw_pe=1, session_present=True,
                       session_fail_bind_modes={MODE}, session_retain=retained,
                       session_unbind_ok=not retained)
        assert failed.call('vc_begin')
        assert failed.get('vc_last_failure', 1) == 7
        assert failed.set_calls == [0x4140] and failed.open_calls == 0
        assert failed.get('vc_session_owned', 1) == int(retained)
        if retained:
            assert failed.get('vc_lfb', 1) == 2 and failed.frees == 0
            before = list(failed.bios_calls)
            assert failed.call('vc_end')
            assert failed.bios_calls == before and failed.frees == 0
            # Direct retry must not issue even descriptor/font BIOS calls.
            assert failed.call('vc_begin')
            assert failed.bios_calls == before and failed.set_calls == [0x4140]
            failed.session_unbind_ok = True
            assert not failed.call('vc_end')
            assert failed.get('vc_session_owned', 1) == failed.get('vc_lfb', 1) == 0
        assert failed.allocations == failed.frees == 1
    passed('Failed BIND releases provisional ownership or retains quarantine until UNBIND succeeds, forbidding BIOS retries')

    # VBE reports image pages as an additional-page count, so zero is one
    # usable image. The banked and linear VBE 3.0 fields are independent.
    info = descriptor()
    info[0] &= ~0x80             # force the checked banked layout
    info[29], info[52], info[53] = 6, 0, 1
    video = Video(code, info)
    assert not video.call('vc_validate')
    assert video.get('vc_bank_ok', 1) == 1 and video.pages() == 0

    info = descriptor()
    info[52], info[53] = 3, 0
    video = Video(code, info)
    assert not video.call('vc_validate')
    assert video.get('vc_lfb_ok', 1) == 1 and video.pages() == 0
    passed('Zero VBE3 banked and linear page counts each retain one usable page')

    # A missing linear pitch means the linear-specific geometry is absent;
    # keep the checked standard stride and banked page count instead.
    info = descriptor()
    info[29], info[52], info[53] = 2, 4, 7
    struct.pack_into('<H', info, 50, 0)
    video = Video(code, info)
    assert not video.call('vc_validate')
    assert video.get('vc_lfb_ok', 1) == 1
    assert video.get('vc_pitch') == 3200 and video.pages() == 4
    passed('Zero VBE3 linear pitch retains checked standard pitch and banked page count')

    for status, mode in ((0x014F, MODE), (0x004F, MODE+1)):
        video = Video(code, mode_status=status, reported_mode=mode)
        assert video.call('vc_begin'), ('4F03 response accepted', hex(status), hex(mode))
        assert video.get('vc_active', 1) == 0
        assert video.allocations == video.frees == 1
        assert 0x4F03 in video.bios_calls
        assert 0x4F06 not in video.bios_calls and 0x4F07 not in video.bios_calls
    passed('Failed 4F03 status and wrong active mode are rejected before page setup or rendering')

    info = descriptor()
    info[0] |= 0x40  # no bank window is available
    info[16:18] = bytes(2)
    info[31:39] = bytes(8)
    video = Video(code, info)
    assert not video.call('vc_validate') and video.get('vc_lfb_ok', 1) == 1
    video = Video(code, info, set_ok=False)
    assert video.call('vc_begin') and video.get('vc_active', 1) == 0
    assert video.set_calls == [0x4140] and video.allocations == video.frees == 1
    passed('Valid linear-only descriptor accepted; failed linear-only set frees memory and cannot fake bank fallback')

    # The diskseq58 T23 firmware answered 4F06 with AX=004F and plausible
    # pixels/rows but a stride equal to the checked byte pitch divided by 8.
    # Such output is not byte-addressed as VBE specifies. Reject the entire
    # query; neither scale a vendor-specific unit nor discard the valid mode.
    for depth in (32, 16):
        expected_pitch = 1024*((depth+7)//8)
        for transport, options, marker in (
            ('local', {}, 1), ('banked', {'msw_pe': 1}, 0),
            ('protected', {'msw_pe': 1, 'session_present': True}, 2),
        ):
            response = (0x004F, expected_pitch//8, 1024, 1536)
            video = Video(code, xga_descriptor(depth), version=0x200,
                          scanline_result=response, **options)
            assert not video.call('vc_begin'), (depth, transport, 'valid mode poisoned by malformed 4F06')
            assert video.get('vc_lfb', 1) == marker
            assert video.get('vc_pitch') == expected_pitch
            assert video.get('vc_scan_lines') == 768
            assert video.get('vc_frame_bytes', 4) == video.get('vc_access_bytes', 4) == expected_pitch*768
            if marker == 2:
                assert struct.unpack_from('<H', video.bind_packets[-1], 20)[0] == expected_pitch
            assert not video.call('vc_end')
            passed(f'T23 {depth}bpp factor-of-eight scanline response cannot poison checked {transport} stride or enable extra pages')

    for transport, options in (
        ('local', {}), ('banked', {'msw_pe': 1}),
        ('protected', {'msw_pe': 1, 'session_present': True}),
    ):
        for label, response, expected_pitch, expected_rows in (
            ('valid larger byte stride', (0x004F, 4352, 1088, 1024), 4352, 1024),
            ('valid nonaligned byte stride', (0x004F, 4097, 1024, 768), 4097, 768),
            ('query failure', (0x014F, 512, 1024, 1536), 4096, 768),
            ('too few logical pixels', (0x004F, 4096, 1023, 1536), 4096, 768),
            ('too few logical rows', (0x004F, 4096, 1024, 767), 4096, 768),
            ('pixel count disagrees with byte stride', (0x004F, 4352, 1024, 1024), 4096, 768),
            ('logical extent exceeds usable VRAM', (0x004F, 4096, 1024, 0xffff), 4096, 768),
        ):
            video = Video(code, xga_descriptor(), version=0x200,
                          scanline_result=response, **options)
            assert not video.call('vc_begin'), (label, transport)
            assert (video.get('vc_pitch'), video.get('vc_scan_lines')) == (
                expected_pitch, expected_rows), (label, transport)
            assert video.get('vc_frame_bytes', 4) == expected_pitch*768
            assert not video.call('vc_end')
            passed(f'4F06 {label} handled atomically for {transport} framebuffer')

    for label, memory_blocks, reported_pitch, reported_rows, expected_rows in (
        ('zero byte pitch rejected without division trap', 512, 0, 768, 768),
        ('unknown zero TotalMemory retains the checked visible frame', 0, 4352, 1024, 768),
        ('capacity quotient 65520 rejects reported 65535 rows', 4095, 4096, 65535, 768),
        ('capacity quotient 65536 accepts 65535 rows without overflow', 4096, 4096, 65535, 65535),
    ):
        pixels = reported_pitch//4
        video = Video(code, xga_descriptor(), version=0x200, msw_pe=1,
                      session_present=True,
                      scanline_result=(0x004F, reported_pitch, pixels, reported_rows))
        # vc_begin already has a cached controller, and the BIOS mock also
        # retains the same TotalMemory if a later controller query is needed.
        struct.pack_into('<H', video.controller, 18, memory_blocks)
        video.cpu.mem_write(video.symbols['vc_controller'], bytes(video.controller))
        assert not video.call('vc_begin'), label
        assert video.get('vc_pitch') == 4096 and video.get('vc_scan_lines') == expected_rows, label
        assert video.get('vc_access_bytes', 4) == 4096*768
        assert not video.call('vc_end')
        passed(f'4F06 integer-capacity boundary: {label}')

    result = {'status': 'passed', 'checks': checks,
              'fixture_sha256': hashlib.sha256(code).hexdigest(),
              'source_sha256': {path: hashlib.sha256((ROOT/path).read_bytes()).hexdigest()
                                for path in ('src/com/vbe_modes.inc', 'src/com/vbe_console.inc', 'src/com/vbe_fb.inc',
                                             'src/com/vbe_session_fb.inc')},
              'scope': 'Production CPU instructions; BIOS mode data, native LFB opening and the CVSESSION service boundary are supplied by the test'}
    (out/'results.json').write_text(json.dumps(result, indent=2)+'\n')


if __name__ == '__main__':
    main()
