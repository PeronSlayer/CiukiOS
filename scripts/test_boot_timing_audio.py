#!/usr/bin/env python3
"""Run shipping machine code with BIOS ticks and notebook codec power states.

uv run --with unicorn==2.1.4 python scripts/test_boot_timing_audio.py
This models the ADC-off/EAPD states absent from QEMU's AC97 device; it does
not claim to measure the analogue output of a physical ThinkPad.
"""
from pathlib import Path
import struct
import subprocess
from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_INTR, UC_HOOK_INSN
from unicorn.x86_const import *
from test_ps2_boot import symbol


def machine(binary, segment, routine, listing):
    uc = Uc(UC_ARCH_X86, UC_MODE_16)
    uc.mem_map(0, 0x100000)
    base = segment * 16
    origin = 0 if segment == 0x900 else 0x100
    uc.mem_write(base + origin, binary)
    for reg in (UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES):
        uc.reg_write(reg, segment)
    uc.reg_write(UC_X86_REG_SS, 0x7000)
    uc.reg_write(UC_X86_REG_SP, 0xFFF0)
    uc.reg_write(UC_X86_REG_EFLAGS, 0x202)
    uc.mem_write(0x7FFF0, struct.pack('<H', 0xFF00))
    return uc, base, base + origin + symbol(listing, routine)


def ticks_test(kernel, listing, start, delay):
    uc, base, entry = machine(kernel, 0x900, 'stage1_boot_wait_ticks', listing)
    calls = 0
    def interrupt(uc, number, _):
        nonlocal calls
        assert number == 0x1A and uc.reg_read(UC_X86_REG_AX) >> 8 == 0
        tick = start + calls
        uc.reg_write(UC_X86_REG_CX, tick >> 16)
        uc.reg_write(UC_X86_REG_DX, tick & 0xFFFF)
        uc.reg_write(UC_X86_REG_AX, 0)  # BIOS return: midnight flag in AL
        calls += 1
    uc.hook_add(UC_HOOK_INTR, interrupt)
    uc.reg_write(UC_X86_REG_CX, delay)
    uc.emu_start(entry, base + 0xFF00, count=10000)
    assert uc.reg_read(UC_X86_REG_IP) == 0xFF00, 'splash wait failed to return'
    assert calls == delay + 1, f'tick {start:04x}: waited {calls-1}, expected {delay}'
    assert uc.reg_read(UC_X86_REG_CX) == delay, 'BIOS damaged caller CX'
    print(f'[boot-timing] PASS start={start:08x} delay={delay}')


def codec_test(binary, listing, power, *, ready=True, absent=False):
    uc, base, entry = machine(binary, 0x2000, 'ac97_prepare_codec', listing)
    nam = 0xD000
    uc.mem_write(base + 0x100 + symbol(listing, 'nam_base'), struct.pack('<H', nam))
    registers = {0x26: power, 0x2A: 0xABC8, 0x7C: 0x4352, 0x7E: 0x5933}
    writes = []
    ticks = 0
    def interrupt(uc, number, _):
        nonlocal ticks
        assert number == 0x1A
        uc.reg_write(UC_X86_REG_AX, 0)
        uc.reg_write(UC_X86_REG_CX, 0)
        uc.reg_write(UC_X86_REG_DX, 0x2345 + ticks)
        ticks += 1
    def read(uc, port, size, _):
        if port == 0x80:
            return 0
        assert size == 2
        if absent:
            return 0xFFFF
        value = registers.get(port - nam, 0)
        if port == nam + 0x26:
            value = (value & 0xFF00) | (0x000E if ready else 0)
        return value
    def write(uc, port, size, value, _):
        assert size == 2 and nam <= port < nam + 0x80
        writes.append((port - nam, value))
        registers[port - nam] = value
    uc.hook_add(UC_HOOK_INTR, interrupt)
    uc.hook_add(UC_HOOK_INSN, read, None, 1, 0, UC_X86_INS_IN)
    uc.hook_add(UC_HOOK_INSN, write, None, 1, 0, UC_X86_INS_OUT)
    uc.emu_start(entry, base + 0xFF00, count=100000)
    assert uc.reg_read(UC_X86_REG_IP) == 0xFF00, 'codec wait failed to terminate'
    failed = bool(uc.reg_read(UC_X86_REG_EFLAGS) & 1)
    assert failed == (not ready or absent), 'wrong playback readiness result'
    assert not any(reg == 0 for reg, _ in writes), 'boot reset erased firmware power policy'
    if not absent:
        assert registers[0x26] & 0xFF00 == power & 0x8100, 'EAPD/ADC policy changed'
    if not failed:
        assert registers[0x5E] == 0x80, 'CS4299 analogue slots were not selected'
        assert registers[0x2A] == 0xABC8, 'unrelated extended status bits lost'
        assert all(registers[reg] & 0x8000 == 0 for reg in (2, 4, 0x18)), 'output muted'
    print(f'[boot-codec] PASS power={power:04x} DAC-ready={ready} absent={absent}')


def main():
    obj = Path('build/full/obj')
    kernel, listing = (obj/'ciukidos.sys').read_bytes(), (obj/'ciukidos.lst').read_text()
    for start in (0, 0x00FE, 0x1234, 0xFFF0, 0x10000):
        for delay in (1, 36):
            ticks_test(kernel, listing, start, delay)
    out = Path('build/full/t23-runtime-repair-2026-09-06')
    out.mkdir(parents=True, exist_ok=True)
    subprocess.run(['nasm', '-f', 'bin', 'src/com/ac97init.asm', '-D', 'BOOT_SOUND=1',
                    '-l', str(out/'bootsnd.lst'), '-o', str(out/'bootsnd.com')], check=True)
    binary = (out/'bootsnd.com').read_bytes()
    assert binary == (obj/'bootsnd.com').read_bytes(), 'test binary differs from release'
    listing = (out/'bootsnd.lst').read_text()
    for power in (0x0000, 0x0100, 0x8000, 0x8100, 0xFF00):
        codec_test(binary, listing, power)
    codec_test(binary, listing, 0x8100, ready=False)
    codec_test(binary, listing, 0xFFFF, absent=True)


if __name__ == '__main__':
    main()
