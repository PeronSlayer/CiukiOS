#!/usr/bin/env python3
"""Run the assembled disk routines against BIOS recovery and clobber cases.

The BIOS model supplies sector bytes and documented error outputs, not DOS
results. Assertions inspect transferred bytes, destinations, registers and
subsequent requests. This complements HDD-only whole-system acceptance.
"""
import argparse
import struct
from pathlib import Path

from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_INTR
from unicorn.x86_const import *
from test_ps2_boot import symbol


class Disk:
    def __init__(self, kernel, listing, *, reject=0, geometry_vector=False,
                 clobber=False, permanent=False):
        self.uc = Uc(UC_ARCH_X86, UC_MODE_16)
        self.uc.mem_map(0, 0x100000)
        self.base = 0x3000
        self.uc.mem_write(self.base, kernel)
        self.listing = listing
        self.reject = reject
        self.permanent = permanent
        self.geometry_vector = geometry_vector
        self.clobber = clobber
        self.requests = []
        self.writes = {}
        self.uc.mem_write(self.addr('boot_drive'), b'\x80')
        self.uc.hook_add(UC_HOOK_INTR, self.interrupt)

    def addr(self, name):
        return self.base + symbol(self.listing, name)

    @staticmethod
    def payload(lba):
        return bytes(((lba * 11 + i * 7 + (i >> 5)) & 255) for i in range(512))

    def interrupt(self, uc, number, _):
        assert number == 0x13, f'unexpected interrupt {number:x}'
        ax = uc.reg_read(UC_X86_REG_AX)
        fn = ax >> 8
        flags = uc.reg_read(UC_X86_REG_EFLAGS)
        error = False
        if fn in (0x42, 0x43):
            pointer = uc.reg_read(UC_X86_REG_DS) * 16 + uc.reg_read(UC_X86_REG_SI)
            size, count, off, seg, lba = struct.unpack('<HHHHQ', uc.mem_read(pointer, 16))
            self.requests.append((fn, count, lba, seg * 16 + off))
            assert size == 0x10 and count == 1, f'invalid reused DAP: size={size} count={count}'
            if self.reject or self.permanent:
                self.reject = max(0, self.reject - 1)
                # EDD updates the count to the number actually transferred.
                uc.mem_write(pointer + 2, b'\x00\x00')
                error = True
            elif fn == 0x42:
                uc.mem_write(seg * 16 + off, self.payload(lba))
            else:
                self.writes[lba] = bytes(uc.mem_read(seg * 16 + off, 512))
        elif fn == 0x08:
            uc.reg_write(UC_X86_REG_CX, 0xFF3F)
            uc.reg_write(UC_X86_REG_DX, 0x0F01)
            if self.geometry_vector:
                uc.reg_write(UC_X86_REG_ES, 0xF000)
                uc.reg_write(UC_X86_REG_DI, 0x1234)
        elif fn in (0x02, 0x03):
            cx, dx = uc.reg_read(UC_X86_REG_CX), uc.reg_read(UC_X86_REG_DX)
            cyl = (cx >> 8) | ((cx & 0xC0) << 2)
            lba = (cyl * 16 + (dx >> 8)) * 63 + (cx & 63) - 1
            dest = uc.reg_read(UC_X86_REG_ES) * 16 + uc.reg_read(UC_X86_REG_BX)
            self.requests.append((fn, ax & 255, lba, dest))
            if self.permanent:
                error = True
            elif fn == 2:
                uc.mem_write(dest, self.payload(lba))
            else:
                self.writes[lba] = bytes(uc.mem_read(dest, 512))
        elif fn != 0:
            raise AssertionError(f'unexpected disk function {fn:x}')
        if self.clobber and fn != 0x08:
            # Historical firmware / resident hooks do not all preserve the
            # upper halves; segment preservation is our wrapper contract.
            for reg in (UC_X86_REG_EBX, UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP):
                uc.reg_write(reg, 0xDEAD7777)
            uc.reg_write(UC_X86_REG_DS, 0xEEEE)
            uc.reg_write(UC_X86_REG_ES, 0xDDDD)
        uc.reg_write(UC_X86_REG_AX, 0x2000 if error else 0)
        uc.reg_write(UC_X86_REG_EFLAGS, (flags | 1) if error else (flags & ~1))

    def call(self, name, lba, data=None):
        uc = self.uc
        initial = {
            UC_X86_REG_EBX: 0x12340200,
            UC_X86_REG_ECX: 0xA1B24567,
            UC_X86_REG_EDX: 0xB1C20000 | (lba >> 16),
            UC_X86_REG_ESI: 0xC1D23456,
            UC_X86_REG_EDI: 0xD1E25678,
            UC_X86_REG_EBP: 0xE1F27890,
            UC_X86_REG_DS: 0x2468,
            UC_X86_REG_ES: 0x4000,
        }
        for reg, value in initial.items():
            uc.reg_write(reg, value)
        uc.reg_write(UC_X86_REG_CS, 0x300)
        uc.reg_write(UC_X86_REG_SS, 0x7000)
        uc.reg_write(UC_X86_REG_SP, 0xFFF0)
        uc.reg_write(UC_X86_REG_AX, lba & 0xFFFF)
        uc.reg_write(UC_X86_REG_EFLAGS, 0x202)
        uc.mem_write(0x7FFF0, struct.pack('<H', 0xF000))
        uc.mem_write(0x40200, data or bytes([0xA5]) * 512)
        uc.emu_start(self.addr(name), self.base + 0xF000, count=50000)
        assert uc.reg_read(UC_X86_REG_IP) == 0xF000, 'disk routine did not return'
        if not self.permanent:
            assert not uc.reg_read(UC_X86_REG_EFLAGS) & 1, 'successful recovery returned CF'
            if name.startswith('write'):
                assert self.writes.get(lba + 63) == data, 'write used wrong source or sector'
            else:
                assert bytes(uc.mem_read(0x40200, 512)) == self.payload(lba + 63), 'read returned stale/wrong bytes'
        else:
            assert uc.reg_read(UC_X86_REG_EFLAGS) & 1, 'permanent disk failure was concealed'
        for reg, value in initial.items():
            assert uc.reg_read(reg) == value, f'caller register {reg} corrupted: {uc.reg_read(reg):x} != {value:x}'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--kernel', default='build/full/obj/ciukidos.sys')
    ap.add_argument('--listing', default='build/full/obj/ciukidos.lst')
    args = ap.parse_args()
    kernel, listing = Path(args.kernel).read_bytes(), Path(args.listing).read_text()
    failures = []
    for name in ('read_sector_lba', 'read_sector_lba32', 'write_sector_lba', 'write_sector_lba32'):
        for case, options in (
            ('edd', {}), ('geometry-buffer', {'reject': 1, 'geometry_vector': True}),
            ('dap-reuse', {'reject': 1}), ('registers', {'clobber': True}),
            ('failure', {'permanent': True}),
        ):
            try:
                disk = Disk(kernel, listing, **options)
                for lba in ((70001, 70009) if name.endswith('32') else (361, 369)):
                    data = disk.payload(lba + 777) if name.startswith('write') else None
                    disk.call(name, lba, data)
                print(f'PASS {name} {case}')
            except Exception as exc:
                failures.append(f'{name} {case}: {exc}')
                print(f'FAIL {failures[-1]}')
    if failures:
        raise SystemExit(f'{len(failures)} disk recovery checks failed')
    print('PASS 20 assembled disk transfer checks')


if __name__ == '__main__':
    main()
