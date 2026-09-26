#!/usr/bin/env python3
"""Instruction-level AC97 startup ordering tests, not a hardware audio test.

Run with: uv run --with unicorn python scripts/test_ac97_init_cpu.py
The complete production BOOTSND.COM executes in Unicorn. DOS file/allocation
services and PCI BIOS are supplied by this harness. Port hooks model disabled
PCI I/O decoding and posted AC-link accesses (Intel ICH3 datasheet 5.18.1.23).
Real PCM output/BIOS compatibility remain separate QEMU/hardware checks.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_INSN, UC_HOOK_INTR
from unicorn.x86_const import *

ROOT = Path(__file__).resolve().parents[1]
BASE, NAM, NABM, PCM = 0x10000, 0x1000, 0x1100, 0x40000


class AudioMachine:
    def __init__(self, binary, command=0, busy=False, cas_stuck=False,
                 fail_alloc=False, fail_pci_write=0, codec_ready=True,
                 tail=b' /Q', ec_busy=False, muted=False, uart_ready=True,
                 pcm_in_run=False, mic_in_run=False):
        self.cpu = Uc(UC_ARCH_X86, UC_MODE_16)
        self.cpu.mem_map(0, 0x100000)
        self.cpu.mem_write(BASE + 0x100, binary)
        self.cpu.mem_write(BASE + 0x80, bytes([len(tail)]) + tail + b'\r')
        for reg in (UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS):
            self.cpu.reg_write(reg, BASE // 16)
        self.cpu.reg_write(UC_X86_REG_SP, 0xFFFE)
        self.cpu.reg_write(UC_X86_REG_EFLAGS, 0x202)
        self.original_command = self.command = command
        self.pcm_in_run, self.mic_in_run = pcm_in_run, mic_in_run
        self.unsafe_capture_enable = False
        self.busy, self.cas_stuck = busy, cas_stuck
        self.fail_alloc, self.fail_pci_write = fail_alloc, fail_pci_write
        self.codec_ready = codec_ready
        self.tail, self.ec_busy, self.muted = tail, ec_busy, muted
        self.uart_ready = uart_ready
        self.console, self.ec_io = [], []
        self.diagnostic_video, self.diagnostic_serial = bytearray(), bytearray()
        self.ec_command, self.ec_phase, self.ec_value = 0, 0, 0x43
        self.file_handle = None
        self.pci_writes, self.io_before_enable, self.cas_reads = [], [], 0
        self.violations, self.codec_accesses, self.events = [], [], []
        self.pending = self.ticks = self.bdbar = 0
        self.cr = int(busy)
        self.started = self.allocated = self.freed = False
        self.exit_status = None
        self.codec = {0x26: 0xE, 0x2A: 0, 0x7C: 0x4352, 0x7E: 0x5931}
        self.pic = {0x21: 0xB8, 0xA1: 0x8E}
        self.cpu.hook_add(UC_HOOK_INTR, self.interrupt)
        self.cpu.hook_add(UC_HOOK_INSN, self.port_in, None, 1, 0, UC_X86_INS_IN)
        self.cpu.hook_add(UC_HOOK_INSN, self.port_out, None, 1, 0, UC_X86_INS_OUT)

    def carry(self, value):
        flags = self.cpu.reg_read(UC_X86_REG_EFLAGS)
        self.cpu.reg_write(UC_X86_REG_EFLAGS, (flags & ~1) | int(value))

    def cstring(self, segment, offset):
        return bytes(self.cpu.mem_read(segment * 16 + offset, 128)).split(b'\0')[0]

    def interrupt(self, cpu, number, _):
        ax = cpu.reg_read(UC_X86_REG_AX)
        self.carry(False)
        if number == 0x10:
            assert ax >> 8 == 0x0E
            assert not (self.started and self.cr & 1), 'BIOS output while our DMA is running'
            self.diagnostic_video.append(ax & 0xFF)
            return
        if number == 0x16:
            cpu.reg_write(UC_X86_REG_AX, 0x011B)
            return
        if number == 0x1A:
            if ax >> 8 == 0:
                self.ticks += 1
                cpu.reg_write(UC_X86_REG_CX, 0)
                cpu.reg_write(UC_X86_REG_DX, self.ticks)
            elif ax == 0xB101:
                cpu.reg_write(UC_X86_REG_EDX, 0x20494350)
            elif ax == 0xB102:
                self.carry(cpu.reg_read(UC_X86_REG_CX) != 0x2485)
                cpu.reg_write(UC_X86_REG_BX, 0x00FD)
            elif ax == 0xB10A:
                cpu.reg_write(UC_X86_REG_ECX,
                              (NAM if cpu.reg_read(UC_X86_REG_DI) == 0x10 else NABM) | 1)
            elif ax == 0xB109:
                cpu.reg_write(UC_X86_REG_CX,
                              self.command if cpu.reg_read(UC_X86_REG_DI) == 4 else 0x1014)
            elif ax == 0xB10C:
                assert cpu.reg_read(UC_X86_REG_BX) == 0x00FD
                assert cpu.reg_read(UC_X86_REG_DI) == 4
                value = cpu.reg_read(UC_X86_REG_CX)
                self.pci_writes.append(value)
                self.events.append(['pci-command', value])
                if len(self.pci_writes) == self.fail_pci_write:
                    self.carry(True)
                else:
                    # BME controls all three ICH audio DMA engines, including
                    # capture engines whose RUN state can predate this player.
                    if value & 4 and not self.command & 4 and (self.pcm_in_run or self.mic_in_run):
                        self.unsafe_capture_enable = True
                    self.command = value
            else:
                raise AssertionError(f'unexpected BIOS service {ax:04X}')
            return
        assert number == 0x21, f'unexpected interrupt {number:02X}'
        ah = ax >> 8
        if ah == 0x4A:
            if b'/D' in self.tail.upper():
                assert b'Entered player; DOS resize' in self.diagnostic_video, 'entry marker followed DOS resize'
        elif ah == 0x48:
            assert cpu.reg_read(UC_X86_REG_BX) == 0x3C00
            self.carry(self.fail_alloc)
            self.allocated = not self.fail_alloc
            cpu.reg_write(UC_X86_REG_AX, 8 if self.fail_alloc else PCM // 16)
        elif ah == 0x3D:
            path = self.cstring(cpu.reg_read(UC_X86_REG_DS), cpu.reg_read(UC_X86_REG_DX))
            if path == b'\\SYSTEM\\BOOT.SND':
                self.carry(True)
                if self.muted:
                    self.carry(False)
                    self.file_handle = 4
                    cpu.reg_write(UC_X86_REG_AX, 4)
                else:
                    cpu.reg_write(UC_X86_REG_AX, 2)
            else:
                assert path == b'\\SYSTEM\\BOOT.PCM', path
                self.file_handle = 5
                cpu.reg_write(UC_X86_REG_AX, 5)
        elif ah == 0x3F:
            count = cpu.reg_read(UC_X86_REG_CX)
            address = cpu.reg_read(UC_X86_REG_DS) * 16 + cpu.reg_read(UC_X86_REG_DX)
            if self.file_handle == 4:
                assert count == 1
                cpu.mem_write(address, b'0')
            else:
                assert PCM <= address < PCM + 245760
                cpu.mem_write(address, b'\x55' * count)
            cpu.reg_write(UC_X86_REG_AX, count)
        elif ah == 0x3E:
            pass
        elif ah == 0x49:
            assert cpu.reg_read(UC_X86_REG_ES) == PCM // 16
            assert self.cr == 0, 'released PCM while DMA still active'
            self.freed = True
        elif ah == 0x4C:
            self.exit_status = ax & 0xFF
            cpu.emu_stop()
        elif ah == 9:
            assert not (self.started and self.cr & 1), 'DOS output while our DMA is running'
            address = cpu.reg_read(UC_X86_REG_DS) * 16 + cpu.reg_read(UC_X86_REG_DX)
            self.console.append(bytes(cpu.mem_read(address, 256)).split(b'$')[0].decode('ascii'))
        elif ah == 2:
            assert not (self.started and self.cr & 1), 'DOS output while our DMA is running'
        else:
            raise AssertionError(f'unexpected DOS service {ax:04X}')

    def decoded(self, port):
        if NAM <= port < NABM + 0x40 and not self.command & 1:
            self.io_before_enable.append(port)
            return False
        return True

    def codec_access(self, direction, port):
        self.codec_accesses.append([direction, port - NAM])
        if self.pending:
            self.violations.append([direction, port - NAM])
        self.pending = 2

    def port_in(self, cpu, port, size, _):
        if port in self.pic:
            return self.pic[port]
        if port == 0x22C:
            return 0xFF  # no native ISA Sound Blaster
        if port == 0x80:
            return 0
        if port == 0x3FD:
            return 0x20 if self.uart_ready else 0
        if port in (0x62, 0x66):
            self.ec_io.append(['in', port])
            if port == 0x62:
                self.ec_phase = 0
                return self.ec_value
            return 2 if self.ec_busy else int(self.ec_command == 0x80 and self.ec_phase == 2)
        if not self.decoded(port):
            return (1 << (size * 8)) - 1
        if port == NABM + 0x34:
            self.cas_reads += 1
            if self.cas_stuck:
                return 1
            if self.pending:
                self.pending -= 1
                return 1
            return 0
        if NAM <= port < NAM + 0x100:
            self.codec_access('read', port)
            return self.codec.get(port - NAM, 0)
        if port == NABM + 0x1B:
            self.events.append(['read-output-control', self.command])
            return self.cr
        if port == NABM + 0x0B:
            return int(self.pcm_in_run)
        if port == NABM + 0x2B:
            return int(self.mic_in_run)
        if port == NABM + 0x2C:
            return 2
        if port == NABM + 0x30:
            return 0x100 if self.codec_ready else 0
        if port == NABM + 0x16:
            return 5 if self.started else 1
        raise AssertionError(f'unexpected port read {port:04X}/{size}')

    def port_out(self, cpu, port, size, value, _):
        if port == 0x3F8:
            self.diagnostic_serial.append(value)
            return
        if port in self.pic:
            self.pic[port] = value
            return
        assert port not in (0x42, 0x43, 0x61), 'unexpected PC speaker fallback'
        if port in (0x62, 0x66):
            self.ec_io.append(['out', port, value])
            if port == 0x66:
                self.ec_command, self.ec_phase = value, 1
            elif self.ec_phase == 1:
                assert value == 0x30, 'unexpected EC register'
                self.ec_phase = 2
            else:
                assert self.ec_command == 0x81 and self.ec_phase == 2
                self.ec_value, self.ec_phase = value, 0
            return
        assert self.decoded(port), f'write before PCI I/O decoding: {port:04X}'
        if NAM <= port < NAM + 0x100:
            self.codec_access('write', port)
            self.codec[port - NAM] = value
        elif port == NABM + 0x1B:
            self.cr = 0 if value == 2 else value
            if value & 1:
                assert self.command & 5 == 5, 'DMA started without PCI bus master enabled'
                assert self.bdbar % 8 == 0
                for index in range(15):
                    address, words, flags = struct.unpack('<IHH', cpu.mem_read(self.bdbar + index * 8, 8))
                    assert (address, words, flags) == (PCM + index * 16384, 8192, 0)
                self.started = True
        elif port == NABM + 0x10:
            self.bdbar = value
        else:
            assert port in (NABM + 0x15, NABM + 0x16, NABM + 0x2C), hex(port)

    def run(self):
        self.cpu.emu_start(BASE + 0x100, BASE + 0x10000, count=8000000)
        assert self.exit_status is not None, 'startup did not terminate within instruction bound'
        assert self.command == self.original_command, 'original PCI command was not restored'
        assert self.pic == {0x21: 0xB8, 0xA1: 0x8E}, 'PIC masks changed'
        assert self.allocated == self.freed, 'PCM allocation leaked'
        assert not self.unsafe_capture_enable, 'enabled global bus mastering with stale capture DMA RUN'
        return self


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('build/full/t23-audio-cpu'))
    parser.add_argument('--binary', type=Path, help='Execute a previously built negative-control binary')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    binary = args.binary or args.output / 'BOOTSND.COM'
    if not args.binary:
        subprocess.run(['nasm', '-DBOOT_SOUND=1', '-f', 'bin', 'src/com/ac97init.asm',
                        '-o', str(binary)], cwd=ROOT, check=True)
    tests = [
        ('initial-io-disabled', {}, True),
        ('posted-codec-writes', {'command': 1}, True),
        ('preserve-command-bits', {'command': 0x0101}, True),
        ('busy-output-io-disabled', {'busy': True}, False),
        ('busy-output-enabled', {'command': 5, 'busy': True}, False),
        ('allocation-failure', {'fail_alloc': True}, False),
        ('io-enable-failure', {'fail_pci_write': 1}, False),
        ('bus-master-enable-failure', {'fail_pci_write': 2}, False),
        ('codec-ready-timeout', {'codec_ready': False}, False),
        ('codec-semaphore-timeout', {'command': 1, 'cas_stuck': True}, False),
        ('manual-test-no-raw-ec', {'tail': b' TEST'}, True),
        ('diagnostic-forces-muted-test', {'tail': b' /D', 'muted': True}, True),
        ('diagnostic-explicit-ec', {'tail': b' /D /E'}, True),
        ('diagnostic-ec-busy-bounded', {'tail': b' /D /E', 'ec_busy': True}, True),
        ('quiet-never-accesses-ec', {'tail': b' /Q /E'}, True),
        ('lowercase-diagnostic', {'tail': b' /d'}, True),
        ('diagnostic-pci-failure', {'tail': b' /D', 'fail_pci_write': 1}, False),
        ('diagnostic-uart-stuck-bounded', {'tail': b' /D', 'uart_ready': False}, True),
        ('stale-pcm-input-dma', {'pcm_in_run': True}, False),
        ('stale-mic-input-dma', {'mic_in_run': True}, False),
    ]
    results = []
    for name, options, success in tests:
        machine = AudioMachine(binary.read_bytes(), **options)
        error = None
        try:
            machine.run()
            assert (machine.exit_status == 0) == success, 'wrong startup exit status'
            assert machine.started == success, 'wrong DMA launch outcome'
            assert not machine.io_before_enable, 'I/O accessed before PCI I/O was enabled'
            assert not machine.violations, 'codec access overlapped pending posted access'
            if options.get('busy'):
                assert not machine.codec_accesses, 'busy output codec was touched'
                assert all(value & 4 == machine.original_command & 4
                           for value in machine.pci_writes), 'enabled DMA merely to inspect busy status'
            if options.get('cas_stuck'):
                assert 0 < machine.cas_reads <= 65536, 'CAS timeout is not bounded'
                assert not machine.codec_accesses, 'codec accessed while semaphore was stuck'
            tail = options.get('tail', b' /Q').upper()
            if b'/Q' in tail or b'/E' not in tail:
                assert not machine.ec_io, 'EC accessed without explicit non-quiet /E'
            elif options.get('ec_busy'):
                assert len(machine.ec_io) == 65535, 'EC wait did not use its bounded budget'
                assert all(item[0] == 'in' for item in machine.ec_io), 'wrote to busy EC'
            else:
                assert machine.ec_value == 3, 'explicit EC diagnostic did not preserve volume/unmute'
            if options.get('uart_ready', True):
                assert machine.diagnostic_video == machine.diagnostic_serial, 'screen/serial diagnostics differ'
            else:
                assert not machine.diagnostic_serial, 'wrote to busy UART'
            diagnostic = machine.diagnostic_video.decode('ascii').splitlines()
            if b'/D' in tail:
                assert diagnostic and 'Entered player' in diagnostic[0], 'missing first visible stage'
                if success:
                    for stage in ('Probe native', 'PCI BIOS', 'Check output busy',
                                  'read PCM', 'Configure codec', 'amplifier timer',
                                  'DMA start', 'DMA stopped', 'Restore PCI'):
                        assert any(stage in line for line in diagnostic), f'missing diagnostic: {stage}'
            else:
                assert not diagnostic, 'diagnostic stages printed without /D'
            if b'/Q' in tail and b'/D' not in tail:
                assert not machine.console, 'quiet startup emitted console text'
        except Exception as exc:
            error = str(exc)
        results.append({'case': name, 'passed': error is None, 'error': error,
                        'exit_status': machine.exit_status, 'started': machine.started,
                        'pci_writes': machine.pci_writes, 'cas_reads': machine.cas_reads,
                        'unserialized_accesses': machine.violations,
                        'io_before_enable': machine.io_before_enable,
                        'ec_port_accesses': len(machine.ec_io), 'console': machine.console,
                        'diagnostic': machine.diagnostic_video.decode('ascii')})
        print(f'{"PASS" if error is None else "FAIL"} {name}: {error or "checked"}', flush=True)
    report = {'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
              'test_type': 'instruction-level modeled PCI/DOS/AC97; not physical audio',
              'passed': all(result['passed'] for result in results), 'cases': results}
    (args.output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    assert report['passed'], 'AC97 ordering regression detected'


if __name__ == '__main__':
    main()
