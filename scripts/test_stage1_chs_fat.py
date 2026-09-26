#!/usr/bin/env python3
"""Execute the assembled full Stage1 FAT reader with BIOS geometry variation.

Run: uv run --with unicorn python scripts/test_stage1_chs_fat.py
BIOS disk responses are modeled instruction boundaries, not physical disk
certification. The negative control removes only the new DI save/restore
from a source copy, assembles it, and must reproduce the wrong FAT lookup.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_INTR, UC_HOOK_CODE
from unicorn.x86_const import *

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT/'src/boot/full_stage1_loader.asm'
SEGMENT, BASE, RETURN = 0x800, 0x8000, 0x7000
REGISTERS = {'bx': UC_X86_REG_BX, 'cx': UC_X86_REG_CX,
             'dx': UC_X86_REG_DX, 'si': UC_X86_REG_SI,
             'di': UC_X86_REG_DI, 'ds': UC_X86_REG_DS, 'es': UC_X86_REG_ES}


def address(lines, name):
    for index, line in enumerate(lines):
        if re.search(r'\b'+re.escape(name)+r'(?=:|\s+d[bwd]\b)', line):
            for following in lines[index:]:
                match = re.match(r'\s*\d+\s+([0-9A-F]{8})\s', following)
                if match:
                    return int(match[1], 16)
    raise AssertionError(f'Missing assembled symbol: {name}')


def assemble(source, out, name, lba_offset, drive):
    binary, listing = out/(name+'.bin'), out/(name+'.lst')
    subprocess.run(['nasm', '-f', 'bin', str(source),
                    f'-DFAT_LBA_OFFSET={lba_offset}', f'-DDOS_DEFAULT_DRIVE_INDEX={drive}',
                    '-o', str(binary), '-l', str(listing)], check=True, cwd=ROOT)
    data, lines = binary.read_bytes(), listing.read_text().splitlines()
    instruction = next(line for line in lines
                       if 'mov byte [loader_default_drive], DOS_DEFAULT_DRIVE_INDEX' in line)
    patch_offset = int(instruction.split()[1], 16)+4
    assert data[patch_offset] == drive
    assert len(data) <= 4096, 'Stage1 exceeds its loader window'
    return data, lines, {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
                         'default_drive_immediate_offset': patch_offset}


def execute(binary, lines, cluster, scenario, lba_offset, direct=False):
    cpu = Uc(UC_ARCH_X86, UC_MODE_16)
    cpu.mem_map(0, 0x100000)
    cpu.mem_write(BASE, binary)
    values = dict(bx=0x100 if direct else 0x3210, cx=0x5678, dx=0,
                  si=0x789a, di=0x4321, ds=SEGMENT, es=0x500)
    for name, register in REGISTERS.items():
        cpu.reg_write(register, values[name])
    for register, value in ((UC_X86_REG_CS, SEGMENT), (UC_X86_REG_SS, 0),
                            (UC_X86_REG_SP, 0x7bfe), (UC_X86_REG_EFLAGS, 0x202)):
        cpu.reg_write(register, value)
    cpu.mem_write(0x7bfe, struct.pack('<H', RETURN))
    cpu.mem_write(BASE+address(lines, 'boot_drive'), b'\x80')
    relative_lba = 73+(cluster >> 8)
    expected_lba = relative_lba+lba_offset
    cpu.reg_write(UC_X86_REG_AX, relative_lba if direct else cluster)
    sector = bytearray(512)
    expected_cluster = cluster+1
    struct.pack_into('<H', sector, (cluster & 255)*2, expected_cluster)
    calls = []
    transferred = False
    returned = False

    def interrupt(uc, number, _):
        nonlocal transferred
        assert number == 0x13
        ah = uc.reg_read(UC_X86_REG_AH)
        calls.append(ah)
        carry = False
        destination = None
        if ah == 0x42:
            packet = uc.reg_read(UC_X86_REG_DS)*16+uc.reg_read(UC_X86_REG_SI)
            raw = bytes(uc.mem_read(packet, 16))
            assert raw[:2] == b'\x10\0' and struct.unpack_from('<H', raw, 2)[0] == 1
            assert struct.unpack_from('<Q', raw, 8)[0] == expected_lba
            carry = scenario != 'edd'
            if not carry:
                offset, segment = struct.unpack_from('<HH', raw, 4)
                destination = segment*16+offset
        elif ah == 8:
            # BIOS legitimately returns a table pointer here. It must not
            # replace the FAT reader's DI even when geometry is unavailable.
            uc.reg_write(UC_X86_REG_CX, 63)
            uc.reg_write(UC_X86_REG_DX, 15 << 8)
            uc.reg_write(UC_X86_REG_ES, 0xf000)
            uc.reg_write(UC_X86_REG_DI, 0x1234)
            carry = scenario == 'default-geometry'
        elif ah == 2:
            cx, dx = uc.reg_read(UC_X86_REG_CX), uc.reg_read(UC_X86_REG_DX)
            cylinder = (cx >> 8) | ((cx & 0xc0) << 2)
            lba = (cylinder*16+(dx >> 8))*63+(cx & 63)-1
            assert lba == expected_lba and dx & 255 == 0x80
            assert uc.reg_read(UC_X86_REG_AL) == 1
            carry = scenario == 'read-error'
            if not carry:
                destination = uc.reg_read(UC_X86_REG_ES)*16+uc.reg_read(UC_X86_REG_BX)
        else:
            raise AssertionError(f'Unexpected BIOS function {ah:#x}')
        if destination is not None:
            uc.mem_write(destination, bytes(sector))
            transferred = True
        uc.reg_write(UC_X86_REG_AH, 0x20 if carry else 0)
        flags = uc.reg_read(UC_X86_REG_EFLAGS)
        uc.reg_write(UC_X86_REG_EFLAGS, (flags & ~1) | int(carry))

    def stop(uc, location, _size, _):
        nonlocal returned
        if location == BASE+RETURN:
            returned = True
            uc.emu_stop()

    cpu.hook_add(UC_HOOK_INTR, interrupt)
    cpu.hook_add(UC_HOOK_CODE, stop)
    symbol = 'read_sector_rel32' if direct else 'fat16_next_cluster'
    cpu.emu_start(BASE+address(lines, symbol), 0xfffff, count=10000)
    assert returned and cpu.reg_read(UC_X86_REG_SP) == 0x7c00
    assert calls == ([0x42] if scenario == 'edd' else [0x42, 8, 2])
    assert bool(cpu.reg_read(UC_X86_REG_EFLAGS) & 1) == (scenario == 'read-error')
    assert transferred == (scenario != 'read-error')
    preserved = all(cpu.reg_read(register) == values[name]
                    for name, register in REGISTERS.items())
    result = {'cluster': cluster, 'scenario': scenario, 'entry': symbol,
              'returned_cluster': cpu.reg_read(UC_X86_REG_AX),
              'expected_cluster': expected_cluster, 'registers_preserved': preserved,
              'bios_functions': calls}
    result['correct'] = preserved and (direct or scenario == 'read-error'
                                      or result['returned_cluster'] == expected_cluster)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path,
                        default=ROOT/'build/check/stage1-chs-fat')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    text = SOURCE.read_text()
    begin, end = text.index('\nread_sector_rel32:\n'), text.index('\nread_sector_chs32:\n')
    wrapper = text[begin:end]
    assert wrapper.count('    push di\n') == wrapper.count('    pop di\n') == 1
    old = wrapper.replace('    push di\n', '').replace('    pop di\n', '')
    control_source = out/'before-di-preservation.asm'
    control_source.write_text(text[:begin]+old+text[end:])
    report = {'scope': 'Production Stage1 instructions, simulated BIOS responses',
              'source_sha256': hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
              'profiles': []}
    for profile, offset, drive in (('hdd', 0, 2), ('live-cd', 63, 3)):
        binary, lines, metadata = assemble(SOURCE, out, profile, offset, drive)
        before, before_lines, before_metadata = assemble(control_source, out, profile+'-before', offset, drive)
        assert metadata['bytes'] == before_metadata['bytes']+2
        assert metadata['default_drive_immediate_offset'] == before_metadata['default_drive_immediate_offset']
        cases = [execute(binary, lines, cluster, scenario, offset, direct)
                 for cluster in (2, 255, 256, 8194)
                 for scenario in ('edd', 'chs', 'default-geometry', 'read-error')
                 for direct in (False, True)]
        assert all(case['correct'] for case in cases), cases
        controls = [execute(before, before_lines, 2, scenario, offset)
                    for scenario in ('edd', 'chs')]
        assert controls[0]['correct'] and not controls[1]['correct'], controls
        assert controls[1]['returned_cluster'] == 0, controls
        report['profiles'].append(dict(profile=profile, assembled=metadata,
            previous=before_metadata, passing_cases=len(cases),
            negative_control=controls, cases=cases))
    report['passed'] = True
    (out/'result.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({key: value for key, value in report.items() if key != 'profiles'}, indent=2))
    for profile in report['profiles']:
        print(profile['profile'], profile['passing_cases'], 'cases passed;',
              'negative control reproduced;', profile['assembled'])


if __name__ == '__main__':
    main()
