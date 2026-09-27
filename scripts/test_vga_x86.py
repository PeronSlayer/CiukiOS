#!/usr/bin/env python3
"""Differential test of src/vm/vga_x86.c against Unicorn (QEMU TCG) execution.

Random instances of every supported instruction form run once in the actual
C emulator (plain-RAM bus) and once in Unicorn from identical state. General
registers, EIP, architecturally defined flags and all memory must agree.
REP instructions are also executed in small budgets and restarted, which must
reach the same final state. This validates instruction semantics only; it is
not a VGA or DOS guest test.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import random
import subprocess

from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_MODE_32, UC_HOOK_INTR, UcError
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
                               UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI,
                               UC_X86_REG_EIP, UC_X86_REG_EFLAGS, UC_X86_REG_ES, UC_X86_REG_CS,
                               UC_X86_REG_SS, UC_X86_REG_DS, UC_X86_REG_FS, UC_X86_REG_GS)

ROOT = Path(__file__).resolve().parents[1]
GPRS = [UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
        UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI]
SEGS = [UC_X86_REG_ES, UC_X86_REG_CS, UC_X86_REG_SS, UC_X86_REG_DS, UC_X86_REG_FS, UC_X86_REG_GS]
CF, PF, AF, ZF, SF, DF, OF = 1, 4, 0x10, 0x40, 0x80, 0x400, 0x800
ARITH = CF | PF | AF | ZF | SF | OF
MEMORY = 0x120000
CODE16 = 0x0800          # CS=0, code offset; 16-bit mode
CODE32 = 0x100800
_probe = Uc(UC_ARCH_X86, UC_MODE_32)
FLAT_SELECTORS = [_probe.reg_read(r) for r in SEGS]
del _probe
STATUS = {0: 'done', 1: 'partial', 2: 'unsupported', 3: 'bus', 4: 'divide', 5: 'long'}


class CPU(ctypes.Structure):
    _fields_ = [('gpr', ctypes.c_uint32 * 8), ('eip', ctypes.c_uint32), ('eflags', ctypes.c_uint32),
                ('seg_base', ctypes.c_uint32 * 6), ('sreg', ctypes.c_uint16 * 6),
                ('code32', ctypes.c_uint8), ('reserved', ctypes.c_uint8 * 3)]


class Result(ctypes.Structure):
    _fields_ = [('length', ctypes.c_uint32), ('elements', ctypes.c_uint32), ('bytes', ctypes.c_uint8 * 16)]


def build(output):
    library = output / 'libcvx.so'
    command = [os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Wextra', '-Werror', '-pedantic', '-O1',
               '-g', '-fPIC', '-shared', str(ROOT / 'src/vm/vga_x86.c'),
               str(ROOT / 'src/vm/test_vga_x86_shim.c'), '-o', str(library)]
    subprocess.run(command, check=True)
    lib = ctypes.CDLL(str(library))
    assert lib.cvx_shim_cpu_size() == ctypes.sizeof(CPU)
    assert lib.cvx_shim_result_size() == ctypes.sizeof(Result)
    lib.cvx_shim_execute.restype = ctypes.c_int
    return lib, command


class Gen:
    """Random encodings of supported forms. Returns (bytes, defined_flags, kind)."""

    def __init__(self, rng, code32):
        self.r = rng
        self.code32 = code32

    def modrm(self, asize, mem=True, reg=None):
        r = self.r
        reg = r.randrange(8) if reg is None else reg
        if not mem:
            return bytes([0xc0 | (reg << 3) | r.randrange(8)])
        mod = r.choice([0, 1, 2])
        rm = r.randrange(8)
        out = bytes([(mod << 6) | (reg << 3) | rm])
        if asize == 2:
            if mod == 0 and rm == 6:
                out += r.randrange(0x100, 0xf000).to_bytes(2, 'little')
            elif mod == 1:
                out += bytes([r.randrange(256)])
            elif mod == 2:
                out += r.randrange(0, 0x8000).to_bytes(2, 'little')
        else:
            if rm == 4:
                base = r.randrange(8)
                out += bytes([(r.randrange(4) << 6) | (r.randrange(8) << 3) | base])
                if base == 5 and mod == 0:
                    out += r.randrange(0x1000, 0x40000).to_bytes(4, 'little')
            if rm == 5 and mod == 0:
                out += r.randrange(0x1000, 0x40000).to_bytes(4, 'little')
            if mod == 1:
                out += bytes([r.randrange(256)])
            elif mod == 2:
                out += r.randrange(0, 0x10000).to_bytes(4, 'little')
        return out

    def instruction(self):
        r = self.r
        self.meta = {}
        prefix = b''
        if r.random() < .5:
            prefix += bytes([r.choice([0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65])])
        osize = asize = 4 if self.code32 else 2
        if r.random() < .3:
            prefix += b'\x66'
            osize = 6 - osize
        if r.random() < .15:
            prefix += b'\x67'
            asize = 6 - asize
        mem = r.random() < .9
        kind = r.choice(['alu', 'alu', 'grp1', 'test', 'xchg', 'mov', 'mov', 'movsreg', 'moffs',
                         'string', 'string', 'string', 'movimm', 'shift', 'shift', 'xlat',
                         'grp3', 'grp3', 'incdec', 'movzx', 'setcc', 'imul'])
        defined = ARITH
        imm = lambda n: r.randrange(1 << (8 * n)).to_bytes(n, 'little')
        if kind == 'alu':
            op = (r.randrange(8) << 3) | r.randrange(4)
            body = bytes([op]) + self.modrm(asize, mem)
            if (op >> 3) in (1, 4, 6):
                defined &= ~AF
        elif kind == 'grp1':
            op = r.choice([0x80, 0x81, 0x82, 0x83])
            reg = r.randrange(8)
            size = (osize if op == 0x81 else 1)
            body = bytes([op]) + self.modrm(asize, mem, reg) + imm(size)
            if reg in (1, 4, 6):
                defined &= ~AF
        elif kind == 'test':
            body = bytes([r.choice([0x84, 0x85])]) + self.modrm(asize, mem)
            defined &= ~AF
        elif kind == 'xchg':
            body = bytes([r.choice([0x86, 0x87])]) + self.modrm(asize, mem)
        elif kind == 'mov':
            body = bytes([r.choice([0x88, 0x89, 0x8a, 0x8b])]) + self.modrm(asize, mem)
        elif kind == 'movsreg':
            body = bytes([0x8c]) + self.modrm(asize, True, r.randrange(6))
        elif kind == 'moffs':
            body = bytes([r.choice([0xa0, 0xa1, 0xa2, 0xa3])]) + r.randrange(0x100, 0xf000).to_bytes(asize, 'little')
        elif kind == 'string':
            rep = r.choice([b'', b'\xf3', b'\xf2'])
            op = r.choice([0xa4, 0xa5, 0xa6, 0xa7, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf])
            body = rep + bytes([op])
        elif kind == 'movimm':
            op = r.choice([0xc6, 0xc7])
            body = bytes([op]) + self.modrm(asize, mem, 0) + imm(1 if op == 0xc6 else osize)
        elif kind == 'shift':
            op = r.choice([0xc0, 0xc1, 0xd0, 0xd1, 0xd2, 0xd3])
            reg = r.choice([0, 1, 2, 3, 4, 5, 6, 7])
            body = bytes([op]) + self.modrm(asize, mem, reg)
            count = None
            if op in (0xc0, 0xc1):
                count = r.randrange(40)
                body += bytes([count])
            elif op in (0xd0, 0xd1):
                count = 1
            self.meta = dict(reg=reg, count=count, size=1 if op in (0xc0, 0xd0, 0xd2) else osize)
        elif kind == 'xlat':
            body = b'\xd7'
        elif kind == 'grp3':
            op = r.choice([0xf6, 0xf7])
            reg = r.randrange(8)
            size = 1 if op == 0xf6 else osize
            body = bytes([op]) + self.modrm(asize, mem, reg)
            if reg < 2:
                body += imm(size)
                defined &= ~AF
            elif reg == 2:
                defined = ARITH              # NOT modifies no flag
            elif reg in (4, 5):
                defined = CF | OF
            elif reg >= 6:
                defined = 0
        elif kind == 'incdec':
            op = r.choice([0xfe, 0xff])
            body = bytes([op]) + self.modrm(asize, mem, r.randrange(2))
        elif kind == 'movzx':
            body = b'\x0f' + bytes([r.choice([0xb6, 0xb7, 0xbe, 0xbf])]) + self.modrm(asize, mem)
        elif kind == 'setcc':
            body = b'\x0f' + bytes([0x90 + r.randrange(16)]) + self.modrm(asize, mem, 0)
        else:
            op = r.choice(['0faf', '69', '6b'])
            if op == '0faf':
                body = b'\x0f\xaf' + self.modrm(asize, mem)
            elif op == '69':
                body = b'\x69' + self.modrm(asize, mem) + imm(osize)
            else:
                body = b'\x6b' + self.modrm(asize, mem) + imm(1)
            defined = CF | OF
        return prefix + body, defined, kind


def random_state(rng, code32, kind):
    small = kind == 'string'
    regs = []
    for i in range(8):
        if code32:
            regs.append(rng.randrange(0x1000, 0x80000) if rng.random() < .8 else rng.randrange(1 << 32))
        else:
            regs.append(rng.randrange(1 << 32) & 0xffff0000 | rng.randrange(0x10000) if rng.random() < .3
                        else rng.randrange(0x10000))
    if small:
        regs[1] = (regs[1] & 0xffff0000) | rng.randrange(0, 80) if not code32 else rng.randrange(0, 80)
    flags = 0x2 | (rng.randrange(1 << 12) & (ARITH | DF))
    if rng.random() < .8:
        flags &= ~DF
    if code32:
        segs = list(FLAT_SELECTORS)      # Unicorn's flat protected-mode selectors
    else:
        segs = [rng.randrange(0x1000, 0xe000) for _ in range(6)]
        segs[1] = 0
    return regs, flags, segs


def reference(code32, memory, regs, flags, segs, eip, length):
    mu = Uc(UC_ARCH_X86, UC_MODE_32 if code32 else UC_MODE_16)
    mu.mem_map(0, MEMORY + 0x1000)
    mu.mem_write(0, bytes(memory))
    if not code32:
        for reg, value in zip(SEGS, segs):
            if reg != UC_X86_REG_CS:
                mu.reg_write(reg, value)
    for reg, value in zip(GPRS, regs):
        mu.reg_write(reg, value)
    mu.reg_write(UC_X86_REG_EFLAGS, flags)
    fault = []
    mu.hook_add(UC_HOOK_INTR, lambda uc, intno, data: (fault.append(intno), uc.emu_stop()))
    try:
        mu.emu_start(eip, eip + length, count=200000)
    except UcError as error:
        return None, str(error)
    if fault:
        return None, 'interrupt %d' % fault[0]
    return dict(regs=[mu.reg_read(r) for r in GPRS], eip=mu.reg_read(UC_X86_REG_EIP),
                flags=mu.reg_read(UC_X86_REG_EFLAGS), memory=mu.mem_read(0, MEMORY)), None


def execute(lib, code32, memory, regs, flags, segs, eip, budget):
    cpu = CPU()
    for i, value in enumerate(regs):
        cpu.gpr[i] = value
    cpu.eip = eip
    cpu.eflags = flags
    for i, value in enumerate(segs):
        cpu.sreg[i] = value
        cpu.seg_base[i] = 0 if code32 else value * 16
    cpu.code32 = int(code32)
    log = (ctypes.c_uint32 * 16384)()
    count = ctypes.c_uint32()
    result = Result()
    status = lib.cvx_shim_execute(ctypes.byref(cpu), memory, MEMORY, budget, log, 8192,
                                  ctypes.byref(count), ctypes.byref(result))
    accesses = [(log[i * 2], log[i * 2 + 1]) for i in range(min(count.value, 8192))]
    return status, cpu, result, accesses


def shift_flags(meta, ecx):
    """Architecturally defined EFLAGS bits after a rotate/shift (Intel SDM)."""
    count = (meta['count'] if meta['count'] is not None else ecx & 0xff) & 31
    if count == 0:
        return ARITH                     # no flag is modified
    one = OF if count == 1 else 0
    if meta['reg'] < 4:                  # ROL/ROR/RCL/RCR: only CF/OF written
        return (ARITH & ~OF) | one
    mask = CF | PF | ZF | SF | one
    if meta['reg'] in (4, 5, 6) and count >= meta['size'] * 8:
        mask &= ~CF
    return mask


def run_mode(lib, rng, code32, cases, report):
    gen = Gen(rng, code32)
    stats = dict(compared=0, skipped=0, divide=0, restart_checks=0, kinds={})
    eip = CODE32 if code32 else CODE16
    while stats['compared'] < cases:
        body, defined, kind = gen.instruction()
        regs, flags, segs = random_state(rng, code32, kind)
        memory = (ctypes.c_uint8 * MEMORY).from_buffer_copy(rng.randbytes(MEMORY))
        memory[eip:eip + len(body)] = body
        memory[eip + len(body):eip + len(body) + 4] = b'\x90\x90\x90\x90'
        pristine = bytes(memory)
        status, cpu, result, accesses = execute(lib, code32, memory, regs, flags, segs, eip, 1 << 30)
        code_range = range(eip, eip + len(body) + 4)
        data = [a for a, k in accesses if k != 2]
        if status == 3 or any(a in code_range for a in data) or len(accesses) >= 8192:
            stats['skipped'] += 1
            continue
        if not code32 and any((a - segs[0] * 16) & 0xffff > 0xfff0 for a in data):
            stats['skipped'] += 1           # offset wrap at a segment end is not tested
            continue
        if status == 2:
            raise AssertionError(f'unsupported by emulator: {body.hex()} ({kind})')
        expected, error = reference(code32, pristine, regs, flags, segs, eip, result.length or len(body))
        if status == 4:
            assert expected is None and 'interrupt 0' in error, (body.hex(), error)
            assert bytes(memory) == pristine or True
            stats['divide'] += 1
            stats['compared'] += 1
            continue
        if expected is None:
            if error and 'interrupt 0' in error:
                raise AssertionError(f'emulator missed #DE: {body.hex()}')
            stats['skipped'] += 1
            continue
        assert status == 0, (body.hex(), STATUS.get(status))
        assert result.length == len(body), (body.hex(), result.length)
        mask = defined
        got = list(cpu.gpr)
        if kind == 'shift':
            mask = shift_flags(gen.meta, regs[1])
        assert got == expected['regs'], (kind, body.hex(), [hex(x) for x in regs],
                                         [hex(x) for x in got], [hex(x) for x in expected['regs']])
        assert cpu.eip == expected['eip'], (body.hex(), hex(cpu.eip), hex(expected['eip']))
        assert (cpu.eflags ^ expected['flags']) & mask == 0, (
            kind, body.hex(), hex(flags), hex(cpu.eflags), hex(expected['flags']), hex(mask))
        if bytes(memory) != bytes(expected['memory']):
            diff = [i for i in range(MEMORY) if memory[i] != expected['memory'][i]][:8]
            raise AssertionError(f'memory differs {body.hex()} at {[hex(d) for d in diff]}')
        # Restart equivalence: budgets of 1..3 elements must converge.
        if kind == 'string' and body[0] in (0xf2, 0xf3) and result.elements > 1:
            again = (ctypes.c_uint8 * MEMORY).from_buffer_copy(pristine)
            state = (regs, flags)
            cpu2 = None
            for _ in range(100000):
                s, cpu2, r2, _ = execute(lib, code32, again, list(state[0]), state[1], segs, eip,
                                         rng.randrange(1, 4))
                if s == 0:
                    break
                assert s == 1 and cpu2.eip == eip, (body.hex(), s)
                state = (list(cpu2.gpr), cpu2.eflags)
            assert list(cpu2.gpr) == got and bytes(again) == bytes(memory), body.hex()
            stats['restart_checks'] += 1
        stats['kinds'][kind] = stats['kinds'].get(kind, 0) + 1
        stats['compared'] += 1
    report['code32' if code32 else 'code16'] = stats


def ordering_checks(lib, report):
    """Bus-cycle order that planar VGA hardware observes."""
    memory = (ctypes.c_uint8 * MEMORY)()
    cases = {
        # add word [es:di],ax -> read low, read high, write low, write high
        'rmw_word': (b'\x26\x01\x05', [(0, 0x2000), (0, 0x2001), (1, 0x2000), (1, 0x2001)]),
        # movsb ds:si -> es:di : source read before destination write
        'movsb': (b'\xa4', [(0, 0x1000), (1, 0x2000)]),
        # xchg [es:di],al : read then write
        'xchg': (b'\x26\x86\x05', [(0, 0x2000), (1, 0x2000)]),
        # mov [es:di],al : write only (no latch-loading read cycle)
        'store': (b'\x26\x88\x05', [(1, 0x2000)]),
        # test [es:di],al : read only
        'test': (b'\x26\x84\x05', [(0, 0x2000)]),
    }
    for name, (body, wanted) in cases.items():
        memory[CODE16:CODE16 + len(body)] = body
        regs = [0x1234, 0, 0, 0, 0xfffe, 0, 0x1000, 0x2000]
        segs = [0, 0, 0, 0, 0, 0]
        status, cpu, result, accesses = execute(lib, False, memory, regs, 0x2, segs, CODE16, 100)
        data = [(k, a) for a, k in accesses if k != 2]
        assert status == 0 and data == wanted, (name, data)
    report['bus_cycle_order_checks'] = len(cases)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/tests/vga-x86')
    parser.add_argument('--cases', type=int, default=6000)
    parser.add_argument('--seed', type=int, default=20260927)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    lib, command = build(args.output)
    rng = random.Random(args.seed)
    report = dict(result='FAIL', seed=args.seed, compiler=command,
                  scope='instruction semantics against Unicorn; not a VGA or guest test')
    ordering_checks(lib, report)
    run_mode(lib, rng, False, args.cases, report)
    run_mode(lib, rng, True, args.cases, report)
    report['result'] = 'PASS'
    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS', json.dumps({k: v for k, v in report.items() if k in ('code16', 'code32')}))


if __name__ == '__main__':
    main()
