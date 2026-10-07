#!/usr/bin/env python3
"""Run the production STI-shadow release instructions in a tiny CPU fixture.

The cleanup block is extracted from the shipped adaptation patch, assembled by
JWasm, then executed unchanged in Unicorn. This checks the flag ownership rule;
it does not emulate the complete Jemm V86 exit or profile-withdrawal path.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_EBP, UC_X86_REG_EFLAGS, UC_X86_REG_ESP

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / 'patches/jemm-ciukios-sti-shadow-release.patch'
BASE, STACK, STOP = 0x100000, 0x180000, 0x700000


def digest(data):
    return hashlib.sha256(data).hexdigest()


def production_cleanup():
    lines = PATCH.read_text().splitlines()
    start = next(i for i, line in enumerate(lines)
                 if line.startswith('+ ; Releasing the profile may happen'))
    block = []
    for line in lines[start:]:
        if line.strip().lower() == 'mov cvactive,0':
            block.append(line.strip())
            return '\n'.join(block)
        if not line.startswith('+'):
            break
        instruction = line[1:]
        block.append(instruction)
    raise AssertionError('release block does not end at CvActive=0')


def assemble(block, output, jwasm):
    assembly = ('.386p\n.model flat\noption casemap:none\n'
                'Client_Reg_Struc STRUCT\n Client_EFlags DD ?\n'
                'Client_Reg_Struc ENDS\n'
                '.data\npublic CvShadow,CvShadowTF,CvActive\n'
                'CvShadow db 0\nCvShadowTF db 0\nCvActive dd 0\n'
                '.code\npublic release_cleanup\n'
                'release_cleanup proc\n' + block + '\n ret\n'
                'release_cleanup endp\nend\n')
    output.mkdir(parents=True, exist_ok=True)
    asm, obj, elf, binary = [output / ('sti-shadow' + suffix)
                              for suffix in ('.asm', '.o', '.elf', '.bin')]
    asm.write_text(assembly)
    link = output / 'link.ld'
    link.write_text('SECTIONS { . = 0x100000; .text : { *(.text*) } '
                    '.data : { *(.data*) } .bss : { *(.bss*) } }\n')
    log = subprocess.check_output([str(jwasm), '-nologo', '-elf', '-Cp',
                                   '-Fo' + str(obj), str(asm)], text=True)
    (output / 'assemble.log').write_text(log)
    subprocess.run(['ld', '-m', 'elf_i386', '-T', str(link), '-o', str(elf), str(obj)],
                   check=True)
    subprocess.run(['objcopy', '-O', 'binary', str(elf), str(binary)], check=True)
    symbols = {fields[2]: int(fields[0], 16)
               for line in subprocess.check_output(['nm', str(elf)], text=True).splitlines()
               if len(fields := line.split()) == 3}
    return binary.read_bytes(), symbols


def run_case(binary, symbols, *, shadow, original_tf, expect_tf):
    cpu = Uc(UC_ARCH_X86, UC_MODE_32)
    cpu.mem_map(BASE, 0x100000)
    cpu.mem_write(BASE, binary)
    cpu.mem_write(STACK, struct.pack('<I', STOP))
    cpu.reg_write(UC_X86_REG_EBP, STACK + 0x100)
    cpu.reg_write(UC_X86_REG_ESP, STACK)
    flags_before = 0x00020202 | (0x100 if original_tf or shadow else 0)
    cpu.mem_write(STACK + 0x100, struct.pack('<I', flags_before))
    cpu.mem_write(symbols['CvShadow'], bytes((shadow, original_tf)))
    cpu.mem_write(symbols['CvActive'], struct.pack('<I', 1))
    cpu.emu_start(symbols['release_cleanup'], STOP, count=100)
    flags_after, = struct.unpack('<I', cpu.mem_read(STACK + 0x100, 4))
    assert bool(flags_after & 0x100) == expect_tf, (hex(flags_before), hex(flags_after))
    assert flags_after == (flags_before if expect_tf else flags_before & ~0x100)
    assert cpu.mem_read(symbols['CvShadow'], 2) == b'\0\0'
    assert cpu.mem_read(symbols['CvActive'], 4) == b'\0\0\0\0'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path,
                        default=ROOT / 'build/tests/jemm-sti-shadow-release')
    parser.add_argument('--jwasm', type=Path,
                        default=ROOT / 'build/external/JWasm/build/GccUnixR/jwasm')
    args = parser.parse_args()
    patch_bytes = PATCH.read_bytes()
    block = production_cleanup()
    binary, symbols = assemble(block, args.output, args.jwasm)
    cases = [
        ('synthetic_TF_removed_when_profile_withdrawn', 1, 0, False),
        ('genuine_guest_TF_preserved_when_profile_withdrawn', 1, 1, True),
        ('guest_TF_preserved_without_pending_shadow', 0, 1, True),
    ]
    for _, shadow, original_tf, expect_tf in cases:
        run_case(binary, symbols, shadow=shadow, original_tf=original_tf,
                 expect_tf=expect_tf)
    report = {'result': 'PASS', 'scope': 'exact production patch cleanup block; no full Jemm/V86 runtime claim',
              'patch_sha256': digest(patch_bytes), 'assembled_sha256': digest(binary),
              'cases': [name for name, *_ in cases]}
    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
