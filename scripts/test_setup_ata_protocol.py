#!/usr/bin/env python3
"""Run the assembled Setup ATA routines against a timed PIO device model.

This is an instruction-level protocol test, not an emulated/physical HDD test.
Each I/O cycle advances 100 ns. After the last data word, status may remain
stale for 400 ns (ATA-3 section 5.2.15), then BSY precedes command completion.
The model rejects task-file commands while BSY or DRQ is set and records any
premature return. Real QEMU installation tests separately check disk contents.
Requires unicorn (e.g. uv run --with unicorn python this_file.py ...).
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import zlib

from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_INSN
from unicorn.x86_const import (UC_X86_INS_IN, UC_X86_INS_OUT, UC_X86_REG_CS,
    UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS, UC_X86_REG_SP,
    UC_X86_REG_BX, UC_X86_REG_CX, UC_X86_REG_EFLAGS)
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_SI

SYMBOLS = ('raw_ata_write_n', 'raw_ata_read_n', 'raw_ata_flush_cache',
           'ata_cmd_base', 'ata_ctrl_base', 'ata_dev_select',
           'raw_clone_lba_lo', 'io_buffer', 'setup_disk_sectors',
           'setup_crc_buffer')


class Device:
    def __init__(self, data, delayed_ready=False):
        self.data = data
        self.words = 0
        self.time = 0
        self.ready_at = 1200 if delayed_ready else 0
        self.completed_at = 0
        self.command = 0
        self.violations = []
        self.written = bytearray()
        self.task_file = {}
        self.lbas = []

    def status(self):
        if self.time < self.ready_at:
            return 0x58  # selected device has not finished its preceding data phase
        if self.completed_at:
            if self.time < self.completed_at + 400:
                return 0x58  # previous data-phase status is still observable
            if self.time < self.completed_at + 800:
                return 0x80
            return 0x50
        return 0x58 if self.command in (0x20, 0x30) else 0x50

    def read(self, uc, port, size, user):
        self.time += 100
        if port in (0x1f7, 0x3f6):
            return self.status()
        if port == 0x1f0:
            assert size == 2 and self.command == 0x20
            value = struct.unpack_from('<H', self.data, self.words * 2)[0]
            self.word_done()
            return value
        raise AssertionError(('unexpected port read', port, size))

    def write(self, uc, port, size, value, user):
        self.time += 100
        if port == 0x3f6:
            return
        if port == 0x1f0:
            assert size == 2 and self.command == 0x30
            self.written += struct.pack('<H', value)
            self.word_done()
        elif 0x1f2 <= port <= 0x1f7:
            if self.status() & 0x88:
                self.violations.append(f'task-file write {port:04x} while BSY/DRQ at {self.time} ns')
            if port == 0x1f7:
                r = self.task_file
                self.lbas.append(r.get(0x1f3,0) | r.get(0x1f4,0)<<8 |
                                 r.get(0x1f5,0)<<16 | (r.get(0x1f6,0)&15)<<24)
                self.command = value
                self.words = 0
                self.completed_at = 0
            self.task_file[port] = value
        else:
            raise AssertionError(('unexpected port write', port, size))

    def word_done(self):
        self.words += 1
        if self.words == 256:
            self.completed_at = self.time


def run(binary, symbols, operation, delayed_ready=False, lba=424, count=1):
    uc = Uc(UC_ARCH_X86, UC_MODE_16)
    uc.mem_map(0, 1024*1024)
    base = 0x20000
    uc.mem_write(base+0x100, binary)
    for reg in (UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS):
        uc.reg_write(reg, base >> 4)
    uc.reg_write(UC_X86_REG_SP, 0xf000)
    uc.mem_write(base+0xf000, struct.pack('<H', 0x80))
    uc.mem_write(base+symbols['ata_cmd_base'], struct.pack('<H', 0x1f0))
    uc.mem_write(base+symbols['ata_ctrl_base'], struct.pack('<H', 0x3f6))
    uc.mem_write(base+symbols['ata_dev_select'], b'\xe0')
    uc.mem_write(base+symbols['raw_clone_lba_lo'], struct.pack('<I', lba))
    uc.mem_write(base+symbols['setup_disk_sectors'], struct.pack('<I', 1048576))
    data = bytes((i*37+11) & 255 for i in range(512))
    if operation == 'write':
        uc.mem_write(base+symbols['io_buffer'], data)
    device = Device(data, delayed_ready)
    uc.hook_add(UC_HOOK_INSN, device.read, None, 1, 0, UC_X86_INS_IN)
    uc.hook_add(UC_HOOK_INSN, device.write, None, 1, 0, UC_X86_INS_OUT)
    uc.reg_write(UC_X86_REG_BX, symbols['io_buffer'])
    uc.reg_write(UC_X86_REG_CX, count)
    name = 'raw_ata_' + operation + '_n'
    uc.emu_start(base+symbols[name], base+0x80, count=30000000)
    carry = bool(uc.reg_read(UC_X86_REG_EFLAGS) & 1)
    if device.completed_at and device.time < device.completed_at+800:
        device.violations.append('returned before device completed its data phase')
    actual = bytes(device.written) if operation == 'write' else bytes(uc.mem_read(base+symbols['io_buffer'],512))
    return {'operation': operation, 'delayed_ready': delayed_ready,
            'violations': device.violations, 'carry': carry,
            'data_matches': actual == data, 'data_words': device.words,
            'requested_lba': lba, 'task_file_lbas': device.lbas}


def check_crc(binary, symbols):
    uc = Uc(UC_ARCH_X86, UC_MODE_16)
    uc.mem_map(0, 1024*1024)
    base = 0x20000
    uc.mem_write(base+0x100,binary)
    for reg in (UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS):
        uc.reg_write(reg,base>>4)
    data = bytes((i*113 + i//256) & 255 for i in range(4096))
    crc = 0xffffffff
    offset = 0
    for sectors in (1,3,4):
        chunk = data[offset:offset+sectors*512]
        uc.mem_write(base+symbols['io_buffer'],chunk)
        uc.reg_write(UC_X86_REG_EAX,crc)
        uc.reg_write(UC_X86_REG_CX,sectors)
        uc.reg_write(UC_X86_REG_SI,symbols['io_buffer'])
        uc.reg_write(UC_X86_REG_SP,0xf000)
        uc.mem_write(base+0xf000,struct.pack('<H',0x80))
        uc.emu_start(base+symbols['setup_crc_buffer'],base+0x80,count=1000000)
        crc = uc.reg_read(UC_X86_REG_EAX)
        offset += len(chunk)
    assert crc ^ 0xffffffff == zlib.crc32(data)
    return {'bytes':len(data),'chunks_in_sectors':[1,3,4],
            'matches_independent_zlib':True,'crc32':f'{crc ^ 0xffffffff:08x}'}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--setup', type=Path, default=Path('src/com/setup.asm'))
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--expect-vulnerable', action='store_true')
    args = ap.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    wrapper = out/'fixture.asm'
    wrapper.write_text('%include "'+str(args.setup.resolve())+'"\n' +
                       'dw '+', '.join(SYMBOLS)+'\n')
    binary_path = out/'fixture.com'
    subprocess.run(['nasm','-f','bin',str(wrapper),'-o',str(binary_path)],check=True)
    binary = binary_path.read_bytes()
    symbols = dict(zip(SYMBOLS,struct.unpack('<'+'H'*len(SYMBOLS),binary[-2*len(SYMBOLS):])))
    rows = [run(binary,symbols,op,delayed) for op in ('write','read') for delayed in (False,True)]
    report = {'test_kind':'instruction-level ATA protocol model', 'cases': rows}
    (out/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    for row in rows:
        assert row['data_matches'] and not row['carry'], row
        assert bool(row['violations']) == args.expect_vulnerable, row
        assert row['task_file_lbas'] == [row['requested_lba']], row
    if not args.expect_vulnerable:
        report['crc32'] = check_crc(binary,symbols)
        report['bounds'] = []
        for lba,count in ((262207,1),(262205,4),(424,0),(424,9)):
            row=run(binary,symbols,'write',lba=lba,count=count)
            assert row['carry'] and row['data_words']==0 and not row['task_file_lbas'], row
            report['bounds'].append({'lba':lba,'sectors':count,'rejected_before_io':True})
        for lba in (65535,65536,196670):
            row=run(binary,symbols,'write',lba=lba)
            assert row['task_file_lbas']==[lba] and not row['carry'] and not row['violations'], row
            rows.append(row)
    (out/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__ == '__main__':
    main()
