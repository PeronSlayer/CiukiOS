#!/usr/bin/env python3
"""Exercise LOADDRV's actual VIDEO INT10-hook evidence helper in Unicorn.

The entire production COM source is assembled with NASM. The helper and its
surrounding PUSHF/POPF wrapper are entered by listing-derived offsets; BIOS
IVT contents and the pre-EXEC IVT snapshot are supplied by the fixture.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_16
from unicorn.x86_const import *


ROOT = Path(__file__).resolve().parents[1]
BASE = 0x10000
CS = BASE >> 4
STACK = 0xF000
RETURN_IP = 0x7000
TEST_CLASS = 0x8000
ORG = 0x100


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def listing_entries(listing):
    entries = []
    for index, line in enumerate(listing.splitlines()):
        match = re.match(r'^\s*\d+\s+([0-9A-F]{8})\s+', line)
        if match:
            entries.append((index, int(match.group(1), 16), line))
    return entries


def source_label_address(listing_lines, entries, label):
    for index, line in enumerate(listing_lines):
        if re.search(rf'\b{re.escape(label)}\s+(?:dw|dd|db|resb)\b', line):
            match = re.match(r'^\s*\d+\s+([0-9A-F]{8})\s+', line)
            if match:
                return int(match.group(1), 16)
    for index, line in enumerate(listing_lines):
        if re.search(rf'^\s*\d+\s+{re.escape(label)}:\s*$', line):
            for entry_index, address, _ in entries:
                if entry_index > index:
                    return address
            raise AssertionError(f'no assembled instruction follows {label}')
    raise AssertionError(f'listing has no address for {label}')


def wrapper_offsets(entries):
    call_index = next(i for i, (_, _, line) in enumerate(entries)
                      if 'call video_hook_evidence' in line)
    push_index = call_index - 1
    pop_index = call_index + 1
    assert 'pushf' in entries[push_index][2]
    assert 'popf' in entries[pop_index][2]
    return entries[push_index][1], entries[pop_index][1] + len(
        bytes.fromhex(re.search(r'\s([0-9A-F]{2}(?:[0-9A-F]{2})*)\s+popf',
                                entries[pop_index][2]).group(1)))


class DriverImage:
    def __init__(self, code, listing):
        self.cpu = Uc(UC_ARCH_X86, UC_MODE_16)
        self.cpu.mem_map(0, 0x100000)
        self.cpu.mem_write(BASE+ORG, code)
        self.entries = listing_entries(listing)
        lines = listing.splitlines()
        self.helper = ORG + source_label_address(lines, self.entries, 'video_hook_evidence')
        self.ivt_copy = ORG + source_label_address(lines, self.entries, 'ivt_copy')
        self.class_start = ORG + source_label_address(lines, self.entries, 'class_start')
        self.class_end = ORG + source_label_address(lines, self.entries, 'class_end')
        self.detail_end = ORG + source_label_address(lines, self.entries, 'detail_end')
        self.detail = ORG + source_label_address(lines, self.entries, 'detail')
        wrapper_start, wrapper_end = wrapper_offsets(self.entries)
        self.wrapper_start = ORG + wrapper_start
        self.wrapper_end = ORG + wrapper_end

    def p16(self, address, value):
        self.cpu.mem_write(address, struct.pack('<H', value & 0xFFFF))

    def setup(self, class_name, before, after, detail_seed=b''):
        cpu = self.cpu
        cpu.mem_write(BASE+RETURN_IP, b'\xF4')
        cpu.reg_write(UC_X86_REG_CS, CS)
        cpu.reg_write(UC_X86_REG_DS, CS)
        cpu.reg_write(UC_X86_REG_ES, 0x7777)
        cpu.reg_write(UC_X86_REG_SS, CS)
        cpu.reg_write(UC_X86_REG_SP, STACK)
        cpu.reg_write(UC_X86_REG_EFLAGS, 0x202)
        cpu.mem_write(CS*16+TEST_CLASS, class_name)
        self.p16(CS*16+self.class_start, TEST_CLASS)
        self.p16(CS*16+self.class_end, TEST_CLASS+len(class_name))
        cpu.mem_write(CS*16+self.detail, detail_seed+b'\0'+bytes(128))
        self.p16(CS*16+self.detail_end, self.detail)
        self.write_ivt(before, after)

    def write_ivt(self, before, after):
        # Vectors are offset:segment words in memory; the helper formats the
        # dword as segment:offset.
        old_off, old_seg = before
        new_off, new_seg = after
        cpu = self.cpu
        cpu.mem_write(CS*16+self.ivt_copy+0x10*4,
                      struct.pack('<HH', old_off, old_seg))
        cpu.mem_write(0x10*4, struct.pack('<HH', new_off, new_seg))

    def invoke(self, address, until=None, stack_return=True):
        cpu = self.cpu
        if stack_return:
            cpu.reg_write(UC_X86_REG_SP, STACK-2)
            self.p16(CS*16+STACK-2, RETURN_IP)
            until = BASE+RETURN_IP
        else:
            cpu.reg_write(UC_X86_REG_SP, STACK)
        try:
            cpu.emu_start(BASE+address, BASE+until if until is not None else 0,
                          count=10000)
        except UcError as error:
            raise AssertionError((str(error), {
                'cs': hex(cpu.reg_read(UC_X86_REG_CS)),
                'ip': hex(cpu.reg_read(UC_X86_REG_IP)),
                'ds': hex(cpu.reg_read(UC_X86_REG_DS)),
                'es': hex(cpu.reg_read(UC_X86_REG_ES)),
                'ss': hex(cpu.reg_read(UC_X86_REG_SS)),
                'sp': hex(cpu.reg_read(UC_X86_REG_SP)),
                'eflags': hex(cpu.reg_read(UC_X86_REG_EFLAGS)),
            })) from error

    def detail_text(self):
        data = self.cpu.mem_read(CS*16+self.detail, 128)
        return bytes(data).split(b'\0', 1)[0].decode('ascii')

    def register_snapshot(self):
        cpu = self.cpu
        regs = (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX,
                UC_X86_REG_EDX, UC_X86_REG_ESI, UC_X86_REG_EDI,
                UC_X86_REG_EBP, UC_X86_REG_ESP, UC_X86_REG_DS,
                UC_X86_REG_ES, UC_X86_REG_FS, UC_X86_REG_GS,
                UC_X86_REG_SS)
        return {reg: cpu.reg_read(reg) for reg in regs}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path,
                        default=ROOT/'build/tests/video-driver-hook')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    binary = out/'loaddrv.com'
    listing_path = out/'loaddrv.lst'
    subprocess.run(['nasm', '-f', 'bin', '-l', str(listing_path),
                    '-o', str(binary), 'src/com/loaddrv.asm'],
                   cwd=ROOT, check=True)
    code = binary.read_bytes()
    listing = listing_path.read_text()
    driver = DriverImage(code, listing)
    checks = []

    old = (0x4567, 0xF000)
    hooked = (0xA456, 0xD123)

    driver.setup(b'VIDEO', old, old)
    before = driver.register_snapshot()
    driver.invoke(driver.helper)
    after = driver.register_snapshot()
    assert before == after, ('helper changed general/segment registers or ES',
                             {key: (hex(before[key]), hex(after[key]))
                              for key in before if before[key] != after[key]})
    assert driver.detail_text() == ' INT10 F000:4567->F000:4567 unchanged'
    checks.append('VIDEO unchanged vector is annotated with pre-EXEC and current INT10')

    driver.setup(b'VIDEO', old, hooked)
    driver.invoke(driver.helper)
    assert driver.detail_text() == ' INT10 F000:4567->D123:A456 hooked'
    checks.append('VIDEO changed vector is annotated as hooked with both pointers')

    sentinel = b'KEEP THIS DETAIL'
    driver.setup(b'AUDIO', old, hooked, sentinel)
    unchanged_detail_end = driver.cpu.mem_read(CS*16+driver.detail_end, 2)
    before = driver.register_snapshot()
    driver.invoke(driver.helper)
    after = driver.register_snapshot()
    assert before == after, 'non-VIDEO helper changed registers or ES'
    assert driver.cpu.mem_read(CS*16+driver.detail, len(sentinel)+1) == sentinel+b'\0'
    assert driver.cpu.mem_read(CS*16+driver.detail_end, 2) == unchanged_detail_end
    checks.append('Non-VIDEO class receives no INT10 annotation or detail mutation')

    driver.setup(b'VIDEO', old, hooked)
    cpu = driver.cpu
    cpu.reg_write(UC_X86_REG_EAX, 0x11223344)
    cpu.reg_write(UC_X86_REG_EBX, 0x55667788)
    cpu.reg_write(UC_X86_REG_ECX, 0x99AABBCC)
    cpu.reg_write(UC_X86_REG_EDX, 0xDDEEFF00)
    cpu.reg_write(UC_X86_REG_ESI, 0x10203040)
    cpu.reg_write(UC_X86_REG_EDI, 0x50607080)
    cpu.reg_write(UC_X86_REG_EBP, 0x90A0B0C0)
    cpu.reg_write(UC_X86_REG_EFLAGS, 0x203)  # caller's carry is significant
    before = driver.register_snapshot()
    entry_flags = cpu.reg_read(UC_X86_REG_EFLAGS)
    driver.invoke(driver.wrapper_start, until=driver.wrapper_end,
                  stack_return=False)
    after = driver.register_snapshot()
    final_flags = cpu.reg_read(UC_X86_REG_EFLAGS)
    assert before == after, 'actual PUSHF/helper/POPF wrapper changed caller registers/ES'
    assert (entry_flags & 0xCD5) == (final_flags & 0xCD5), (
        hex(entry_flags), hex(final_flags))
    assert driver.detail_text() == ' INT10 F000:4567->D123:A456 hooked'
    checks.append('Actual caller PUSHF/POPF wrapper preserves original flags around helper')

    manifest = {
        'passed': True,
        'checks': checks,
        'scope': 'Whole production LOADDRV.COM assembled by NASM; helper and wrapper executed at listing-derived offsets in Unicorn; IVT state supplied by fixture',
        'source': 'src/com/loaddrv.asm',
        'source_sha256': sha256((ROOT/'src/com/loaddrv.asm').read_bytes()),
        'binary_artifact': binary.name,
        'loaddrv_bytes': len(code),
        'loaddrv_sha256': sha256(code),
        'listing_artifact': listing_path.name,
        'listing_sha256': sha256(listing_path.read_bytes()),
        'offsets': {
            'video_hook_evidence': hex(driver.helper),
            'caller_pushf': hex(driver.wrapper_start),
            'caller_after_popf': hex(driver.wrapper_end),
            'class_start': hex(driver.class_start),
            'class_end': hex(driver.class_end),
            'ivt_copy': hex(driver.ivt_copy),
            'detail_end': hex(driver.detail_end),
            'detail': hex(driver.detail),
        },
    }
    (out/'result.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
