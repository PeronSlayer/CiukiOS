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


class Video:
    def __init__(self, binary, info=None, version=0x300, open_ok=True, set_ok=True,
                 reported_linear=None, reported_mode=None, mode_status=0x004F,
                 msw_pe=None, session_present=False):
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
        self.msw_pe = msw_pe
        self.session_present = session_present
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
            pitch = 3456 if self.linear else 3216
            cpu.reg_write(UC_X86_REG_BX, pitch)
            cpu.reg_write(UC_X86_REG_CX, pitch//4)
            cpu.reg_write(UC_X86_REG_DX, 2048)
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

    # A V86 monitor may expose the CVSESSION far entry, but it cannot grant
    # this real-mode caller permission to clear PE in CR0. Model both signals
    # explicitly: INT 2F/1684 succeeds, while SMSW reports CR0.PE=1.
    video = Video(code, msw_pe=1, session_present=True)
    assert not video.call('vc_session_find')
    assert video.session_queries == 1
    assert not video.call('vc_validate')
    assert video.smsw_calls >= 2
    assert (video.get('vc_lfb_ok', 1), video.get('vc_bank_ok', 1)) == (0, 1)
    passed('V86 PE=1 rejects LFB even when INT2F/1684 reports CVSESSION present')

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

    result = {'status': 'passed', 'checks': checks,
              'fixture_sha256': hashlib.sha256(code).hexdigest(),
              'source_sha256': {path: hashlib.sha256((ROOT/path).read_bytes()).hexdigest()
                                for path in ('src/com/vbe_modes.inc', 'src/com/vbe_console.inc',
                                             'src/com/vbe_session_fb.inc')},
              'scope': 'Production CPU instructions; BIOS mode data and LFB eligibility opening are supplied by the test'}
    (out/'results.json').write_text(json.dumps(result, indent=2)+'\n')


if __name__ == '__main__':
    main()
