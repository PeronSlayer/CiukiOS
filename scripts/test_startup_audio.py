#!/usr/bin/env python3
"""Execute the production startup ring in Unicorn with DOS/ICH register mocks.

Run: uv run --with unicorn python scripts/test_startup_audio.py --output DIR
This tests actual NASM routines, descriptor/file ownership and failure paths;
it does not establish physical AC-link, codec or loudspeaker behavior.
"""
import argparse
import hashlib
import json
import re
import struct
import subprocess
from pathlib import Path

from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_INSN, UC_HOOK_INTR, UC_HOOK_MEM_WRITE
from unicorn.x86_const import *

ROOT = Path(__file__).resolve().parents[1]
RING = 0x40000
RING_BYTES = 65536
NABM = 0x1000
NAM = 0x2000


def extract(text, first, last):
    start = text.index(first + ':')
    end = text.index(last, start)
    return text[start:end]


def fixture_source():
    driver = (ROOT / 'src/com/ui_sound_driver.inc').read_text()
    code = extract(driver, 'sfx_init', '%include "src/com/ui_sound_startup.inc"')
    code += '\n%include "src/com/ui_sound_startup.inc"\n'
    code += extract(driver, 'sfx_release', '%include "src/com/ui_sound_sb.inc"')
    symbols = [
        'stop', 'sfx_init', 'sfx_boot_pcm', 'sfx_play', 'sfx_poll', 'sfx_stop', 'sfx_release',
        'sfx_result', 'sfx_op', 'sfx_buffer', 'sfx_available', 'sfx_playing', 'sfx_boot_active',
        'sfx_boot_file', 'sfx_boot_count', 'sfx_boot_next', 'sfx_boot_civ', 'sfx_boot_left',
        'sfx_boot_total', 'sfx_boot_loaded', 'sfx_boot_played', 'sfx_boot_restarts',
        'sfx_audio_stage', 'sfx_audio_largest', 'ac97_bdl', 'mock_fail_stage', 'mock_cleanups',
    ]
    asm = ['bits 16', 'cpu 386', 'org 0x100', 'jmp stop', "db 'SAU1'",
           '%macro export 1', 'dw %1', 'db %str(%1),0', '%endmacro']
    asm += [f'export {name}' for name in symbols]
    asm += ['dw 0', 'stop: hlt', '%define ICH_PO_CR 0x1B', '%define ICH_PO_SR 0x16',
            '%define ICH_PO_LVI 0x15', '%define ICH_PO_BDBAR 0x10',
            '%define ICH_CR_RESET 2', '%define ICH_CR_RUN 1', '%define ICH_SR_ERROR 0x10',
            '%define ICH_SR_DCH 1', '%define AC97_MASTER_VOL 2',
            '%define AC97_HEADPHONE_VOL 4', '%define AC97_PCM_OUT_VOL 0x18',
            '%define SFX_SB_MAX 65532', code]
    for label, stage in [('pci_find_intel_ac97', 1), ('ac97_enable_io', 2),
                         ('ac97_all_streams_idle', 3), ('ac97_wait_codec', 4),
                         ('ac97_prepare_codec', 5), ('ac97_enable_pci', 14)]:
        asm += [f'{label}:', f'cmp byte [mock_fail_stage],{stage}', 'je mock_failed']
        if stage == 1:
            asm += ['mov word [pci_device_id],0x2485', 'mov word [pci_subsystem_vendor],0x1014']
        if stage == 5:
            asm += ['mov word [codec_vendor_1],0x4352', 'mov word [codec_vendor_2],0x5930',
                    'mov word [codec_power_status],0x000F']
        asm += ['clc', 'ret']
    asm += [
        'mock_failed: stc', 'ret', 'boot_restore_pci: clc', 'ret',
        'ac97_codec_read: in ax,dx', 'clc', 'ret',
        'ac97_cleanup_output: inc word [mock_cleanups]',
        'mov dx,[nabm_base]', 'add dx,ICH_PO_CR', 'xor al,al', 'out dx,al',
        'mov dx,[nabm_base]', 'add dx,ICH_PO_BDBAR', 'xor eax,eax', 'out dx,eax',
        'clc', 'ret',
        'sfx_sb_init: mov word [sfx_result],3', 'ret',
        'sfx_sb_poll: ret', 'sfx_sb_stop: ret', 'sfx_sb_start: ret', 'sfx_boot_program: ret',
        'mock_fail_stage: db 0', 'mock_cleanups: dw 0', 'quiet_mode: db 0',
        'audio_diagnostic: db 0', 'sfx_inited: db 0', 'sfx_available: db 0',
        'sfx_music_owned: db 0', 'sfx_playing: db 0', 'sfx_op: dw 7',
        'sfx_result: dw 0', 'sfx_buffer: dw 0', 'sfx_file: dw 0', 'sfx_bytes: dw 0',
        'sfx_tick: dw 0', 'sfx_timeout: dw 90', 'sfx_setting: db "1"', 'sfx_sb_offset: dw 0',
        'pci_device_id: dw 0', 'pci_subsystem_vendor: dw 0', 'codec_vendor_1: dw 0',
        'codec_vendor_2: dw 0', 'codec_power_status: dw 0',
        'nam_base: dw 0x2000', 'nabm_base: dw 0x1000',
        'sfx_preference: db "\\SYSTEM\\BOOT.SND",0',
        'sfx_boot_path: db "\\SYSTEM\\BOOT.PCM",0',
        'sfx_cue_path: db "\\SYSTEM\\SOUNDS\\INFO.PCM",0',
        'sfx_paths: times 4 dw sfx_cue_path', 'sfx_sb_paths: times 4 dw sfx_cue_path',
        'align 16', 'ac97_bdl: times 32*8 db 0',
    ]
    return '\n'.join(asm) + '\n'


class AudioCPU:
    def __init__(self, binary, payload, fail_stage=0, allocation_failure=False):
        self.uc = Uc(UC_ARCH_X86, UC_MODE_16)
        self.uc.mem_map(0, 0x100000)
        self.uc.mem_write(0x10100, binary)
        self.uc.mem_write(RING - 32, b'\xA5' * (RING_BYTES + 64))
        self.symbols = {}
        cursor = binary.index(b'SAU1') + 4
        while address := struct.unpack_from('<H', binary, cursor)[0]:
            end = binary.index(b'\0', cursor + 2)
            self.symbols[binary[cursor + 2:end].decode()] = 0x10000 + address
            cursor = end + 1
        self.files = {'\\SYSTEM\\BOOT.PCM': bytearray(payload),
                      '\\SYSTEM\\SOUNDS\\INFO.PCM': bytearray(b'\x03\x04\x05\x06' * 3072)}
        self.handles = {}
        self.next_handle = 5
        self.allocations = []
        self.frees = []
        self.allocation_failure = allocation_failure
        self.read_error_at = None
        self.advance_during_read = 0
        self.dos_reads = []
        self.reset_failure = False
        self.bdl_base = 0
        self.queue = []
        self.lvi = None
        self.civ = 0
        self.run_requested = False
        self.running = False
        self.sr_flags = 0
        self.output = bytearray()
        self.published = 0
        self.resets = 0
        self.tick = 10
        self.set('mock_fail_stage', fail_stage, 1)
        self.uc.mem_write(0x046C, struct.pack('<H', self.tick))
        self.uc.hook_add(UC_HOOK_INTR, self.interrupt)
        self.uc.hook_add(UC_HOOK_INSN, self.port_in, None, 1, 0, UC_X86_INS_IN)
        self.uc.hook_add(UC_HOOK_INSN, self.port_out, None, 1, 0, UC_X86_INS_OUT)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.memory_write)

    def set(self, name, value, size=2):
        self.uc.mem_write(self.symbols[name], int(value).to_bytes(size, 'little'))

    def get(self, name, size=2):
        return int.from_bytes(self.uc.mem_read(self.symbols[name], size), 'little')

    def call(self, label, result=0):
        for reg in (UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS):
            self.uc.reg_write(reg, 0x1000)
        self.uc.reg_write(UC_X86_REG_SP, 0xEFFC)
        self.uc.reg_write(UC_X86_REG_EFLAGS, 2)
        self.uc.mem_write(0x1EFFC, struct.pack('<H', self.symbols['stop'] - 0x10000))
        self.set('sfx_result', result)
        self.uc.emu_start(self.symbols[label], self.symbols['stop'], count=4000000)
        assert self.uc.reg_read(UC_X86_REG_IP) == self.symbols['stop'] - 0x10000, label + ' did not return'
        return self.get('sfx_result')

    def cf(self, failed=False):
        flags = self.uc.reg_read(UC_X86_REG_EFLAGS)
        self.uc.reg_write(UC_X86_REG_EFLAGS, (flags & ~1) | bool(failed))

    def cstring(self, address):
        value = bytearray()
        while byte := self.uc.mem_read(address + len(value), 1)[0]:
            value.append(byte)
        return value.decode('ascii')

    def interrupt(self, uc, intno, _):
        ax = uc.reg_read(UC_X86_REG_AX)
        bx = uc.reg_read(UC_X86_REG_BX)
        cx = uc.reg_read(UC_X86_REG_CX)
        dx = uc.reg_read(UC_X86_REG_DX)
        if intno == 0x1A:
            assert ax >> 8 == 0, 'unexpected BIOS call'
            uc.reg_write(UC_X86_REG_DX, self.tick)
            self.cf()
            return
        assert intno == 0x21, f'unexpected interrupt {intno:x}'
        ah = ax >> 8
        address = uc.reg_read(UC_X86_REG_DS) * 16 + dx
        if ah == 0x48:
            self.allocations.append(bx * 16)
            assert bx <= 0x1000 or (self.allocation_failure and bx == 0xFFFF), 'startup-sized allocation'
            if self.allocation_failure:
                uc.reg_write(UC_X86_REG_AX, 8)
                uc.reg_write(UC_X86_REG_BX, 0x0A00)
                self.cf(True)
            else:
                uc.reg_write(UC_X86_REG_AX, RING // 16)
                self.cf()
            return
        if ah == 0x49:
            assert not self.running and not self.queue, 'freed a DMA-owned ring'
            self.frees.append(uc.reg_read(UC_X86_REG_ES) * 16)
            self.cf()
            return
        if ah in (0x3D, 0x3C):
            path = self.cstring(address)
            if ah == 0x3C:
                self.files[path] = bytearray()
            if path not in self.files:
                uc.reg_write(UC_X86_REG_AX, 2)
                self.cf(True)
                return
            handle = self.next_handle
            self.next_handle += 1
            self.handles[handle] = [path, 0]
            uc.reg_write(UC_X86_REG_AX, handle)
            self.cf()
            return
        assert bx in self.handles, f'invalid DOS handle {bx}'
        path, position = self.handles[bx]
        if ah == 0x42:
            displacement = cx * 65536 + dx
            if ax & 255 == 2:
                position = len(self.files[path]) + displacement
            elif ax & 255 == 0:
                position = displacement
            else:
                raise AssertionError('unexpected seek origin')
            self.handles[bx][1] = position
            uc.reg_write(UC_X86_REG_AX, position & 65535)
            uc.reg_write(UC_X86_REG_DX, position >> 16)
        elif ah == 0x3F:
            if path.endswith('BOOT.PCM') and self.read_error_at is not None and position >= self.read_error_at:
                uc.reg_write(UC_X86_REG_AX, 5)
                self.cf(True)
                return
            data = bytes(self.files[path][position:position + cx])
            assert RING <= address and address + len(data) <= RING + RING_BYTES, 'PCM escaped the 64 KiB ring'
            slots = set(range((address - RING) // 2048, (address + max(1, len(data)) - 1 - RING) // 2048 + 1))
            assert not slots.intersection(item[0] for item in self.queue), 'read overwrote a current/queued PCM slot'
            if self.advance_during_read:
                self.advance(self.advance_during_read, tick=False)
            uc.mem_write(address, data)
            self.handles[bx][1] += len(data)
            self.dos_reads.append((path, position, len(data)))
            uc.reg_write(UC_X86_REG_AX, len(data))
        elif ah == 0x40:
            assert path.endswith('AUDIO.LOG'), 'unexpected log target'
            data = bytes(uc.mem_read(address, cx))
            self.files[path][position:position + cx] = data
            self.handles[bx][1] += cx
            uc.reg_write(UC_X86_REG_AX, cx)
        elif ah == 0x3E:
            if path.endswith('BOOT.PCM'):
                assert not self.running and not self.queue, 'closed startup before verified DMA cleanup'
            del self.handles[bx]
        else:
            raise AssertionError(f'unexpected DOS AH={ah:x}')
        self.cf()

    def memory_write(self, _, __, address, size, ___, ____):
        bdl = self.symbols['ac97_bdl']
        if bdl <= address < bdl + 256:
            slot = (address - bdl) // 8
            assert slot not in [item[0] for item in self.queue], 'rewrote a queued/prefetched descriptor'

    def port_in(self, _, port, size, __):
        if port == NABM + 0x1B:
            return 2 if self.reset_failure else 0
        if port == NABM + 0x16:
            return self.sr_flags | (0 if self.running else 1)
        if port == NABM + 0x14:
            return self.civ
        if port in (NAM + 2, NAM + 4, NAM + 0x18):
            return 0
        raise AssertionError(f'unexpected IN {port:x}/{size}')

    def port_out(self, _, port, size, value, __):
        if port == NABM + 0x1B:
            if value & 2:
                assert not self.running, 'reset active PCM engine'
                self.queue.clear()
                self.lvi = None
                self.civ = 0
                self.sr_flags = 0
                self.resets += 1
            self.run_requested = bool(value & 1)
            self.running = self.run_requested and bool(self.queue)
            if self.running:
                self.civ = self.queue[0][0]
            return
        if port == NABM + 0x10:
            self.bdl_base = value
            if not value:
                assert not self.running, 'detached live BDL'
                self.queue.clear()
                self.lvi = None
            return
        if port == NABM + 0x16:
            self.sr_flags &= ~(value & 0x1C)
            return
        if port == NABM + 0x15:
            first = 0 if self.lvi is None else (self.lvi + 1) & 31
            count = ((value - first) & 31) + 1
            for n in range(count):
                slot = (first + n) & 31
                assert slot not in [item[0] for item in self.queue], 'published a live descriptor twice'
                address, samples, flags = struct.unpack('<IHH', self.uc.mem_read(self.bdl_base + slot * 8, 8))
                assert 0 < samples <= 32766 and samples % 2 == 0 and flags == 0, 'invalid descriptor length/flags'
                assert RING <= address and address + samples * 2 <= RING + RING_BYTES, 'descriptor outside ring'
                data = bytes(self.uc.mem_read(address, samples * 2))
                self.queue.append((slot, address, data))
                self.published += 1
            self.lvi = value & 31
            assert len(self.queue) <= 31, 'one-slot guard lost'
            if self.run_requested:
                self.running = True
                self.civ = self.queue[0][0]
            return
        raise AssertionError(f'unexpected OUT {port:x}/{size}')

    def advance(self, descriptors, tick=True):
        if tick:
            self.tick = (self.tick + 1) & 65535
            self.uc.mem_write(0x046C, struct.pack('<H', self.tick))
        for _ in range(descriptors):
            if not self.running:
                return
            slot, address, data = self.queue.pop(0)
            assert bytes(self.uc.mem_read(address, len(data))) == data, 'DMA source changed after publication'
            self.output += data
            self.civ = self.queue[0][0] if self.queue else slot
            if not self.queue:
                self.running = False
                self.sr_flags |= 4

    def logs(self):
        return self.files.get('\\SYSTEM\\AUDIO.LOG', bytearray()).decode().splitlines()

    def prepare(self):
        assert self.call('sfx_init') == 0
        assert self.allocations == [RING_BYTES]
        assert len(self.logs()) == 1 and 'event=I' in self.logs()[0]
        assert 'pci=8086:2485' in self.logs()[0] and 'codec=4352:5930' in self.logs()[0]
        assert 'power=000F master=0000 hp=0000 pcm=0000' in self.logs()[0]

    def finish(self):
        self.call('sfx_release')
        assert self.frees == [RING]
        assert not self.handles
        assert bytes(self.uc.mem_read(RING - 32, 32)) == b'\xA5' * 32
        assert bytes(self.uc.mem_read(RING + RING_BYTES, 32)) == b'\xA5' * 32


def make_pcm(size):
    return bytes((index * 29 + (index // 2048) * 17) & 255 for index in range(size))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    asm = args.output / 'startup_audio.asm'
    binary_path = args.output / 'startup_audio.bin'
    asm.write_text(fixture_source())
    subprocess.run(['nasm', '-f', 'bin', str(asm), '-o', str(binary_path)], cwd=ROOT, check=True)
    binary = binary_path.read_bytes()
    checks = []
    for size, step, concurrent in [(245760, 5, 0), (245764, 7, 0), (2052, 1, 0),
                                   (524288, 8, 1), (245760, 31, 0)]:
        payload = make_pcm(size)
        cpu = AudioCPU(binary, payload)
        cpu.prepare()
        assert cpu.call('sfx_boot_pcm', result=4) == 1
        assert len(cpu.queue) <= 31 and cpu.get('sfx_boot_loaded', 4) <= 31 * 2048
        cpu.advance_during_read = concurrent
        for _ in range(300):
            if not cpu.get('sfx_boot_active', 1):
                break
            cpu.advance(step)
            previous_logs = len(cpu.logs())
            result = cpu.call('sfx_poll')
            if cpu.get('sfx_boot_active', 1):
                assert result == 1 and len(cpu.logs()) == previous_logs, 'ordinary poll performed log I/O'
        else:
            raise AssertionError('startup failed to complete')
        assert bytes(cpu.output) == payload, (
            f'PCM missing/replayed/reordered: size={size} step={step} concurrent={concurrent} '
            f'output={len(cpu.output)} result={result} logs={cpu.logs()}'
        )
        assert cpu.get('sfx_boot_loaded', 4) == size and cpu.get('sfx_boot_played', 4) == size
        assert cpu.allocations == [RING_BYTES] and not cpu.frees, 'startup used another allocation'
        assert [line.split('event=')[1][0] for line in cpu.logs()] == ['I', 'S', 'E']
        if step == 31 and size > 31 * 2048:
            assert cpu.get('sfx_boot_restarts', 1) > 0, 'delayed poll did not exercise drained-queue recovery'
        # Ordinary event playback still owns the same ring after startup EOF.
        cpu.set('sfx_op', 4)
        assert cpu.call('sfx_play') == 1
        cpu.advance(1)
        assert cpu.call('sfx_poll') == 0
        assert bytes(cpu.output[size:]) == bytes(cpu.files['\\SYSTEM\\SOUNDS\\INFO.PCM'])
        cpu.finish()
        checks.append({'pcm_bytes': size, 'advance_per_poll': step, 'advance_during_read': concurrent,
                       'descriptors': cpu.published, 'resets': cpu.resets, 'exact_pcm': True})

    cpu = AudioCPU(binary, make_pcm(4))
    cpu.prepare()
    cpu.tick = 0xFFFF
    cpu.uc.mem_write(0x046C, struct.pack('<H', cpu.tick))
    assert cpu.call('sfx_boot_pcm', result=4) == 1
    cpu.advance(1)                    # BIOS low tick word wraps to zero
    assert cpu.call('sfx_poll') == 1 and cpu.get('mock_cleanups') == 0
    assert cpu.call('sfx_poll') == 1 and cpu.get('mock_cleanups') == 0
    cpu.advance(0)
    assert cpu.call('sfx_poll') == 0 and 'event=E' in cpu.logs()[-1]
    cpu.finish()
    checks.append({'eof_fifo_drain_tick': True, 'bios_low_word_wrap': True})

    for failure in ('stop', 'read', 'fifo', 'reset', 'timeout', 'open', 'size', 'empty'):
        cpu = AudioCPU(binary, make_pcm(245760))
        cpu.prepare()
        if failure == 'read':
            cpu.read_error_at = 70000
        if failure == 'reset':
            cpu.reset_failure = True
        if failure == 'open':
            del cpu.files['\\SYSTEM\\BOOT.PCM']
        if failure == 'size':
            cpu.files['\\SYSTEM\\BOOT.PCM'] = bytearray(b'x' * 5)
        if failure == 'empty':
            cpu.files['\\SYSTEM\\BOOT.PCM'] = bytearray()
        result = cpu.call('sfx_boot_pcm', result=4)
        if cpu.get('sfx_boot_active', 1):
            if failure == 'stop':
                result = cpu.call('sfx_stop')
            elif failure == 'fifo':
                cpu.sr_flags |= 0x10
                result = cpu.call('sfx_poll')
            elif failure == 'timeout':
                cpu.uc.mem_write(0x046C, struct.pack('<H', cpu.tick + 90))
                result = cpu.call('sfx_poll')
            else:
                for _ in range(30):
                    cpu.advance(8)
                    result = cpu.call('sfx_poll')
                    if not cpu.get('sfx_boot_active', 1):
                        break
        assert not cpu.get('sfx_boot_active', 1) and not cpu.running and not cpu.queue
        assert cpu.get('mock_cleanups') > 0 and not any(v[0].endswith('BOOT.PCM') for v in cpu.handles.values())
        assert 'event=' + ('X' if failure == 'stop' else 'F') in cpu.logs()[-1]
        assert result == (0 if failure == 'stop' else 4 if failure in ('read', 'open', 'size', 'empty') else 7)
        cpu.finish()
        checks.append({'failure': failure, 'result': result, 'cleaned_before_close': True})

    for stage in (1, 2, 3, 4, 5, 6):
        cpu = AudioCPU(binary, b'\0' * 4, fail_stage=stage, allocation_failure=stage == 6)
        result = cpu.call('sfx_init')
        assert result == (6 if stage == 6 else 3) and cpu.get('sfx_buffer') == 0
        assert len(cpu.logs()) == 1 and 'event=I' in cpu.logs()[0]
        assert 'stage=' + ('0008' if stage == 1 else f'{stage:04X}') in cpu.logs()[0]
        if stage == 6:
            assert cpu.get('sfx_audio_largest') == 0x0A00 and 'largest=0A00' in cpu.logs()[0]
        checks.append({'init_failure_stage': stage, 'result': result, 'logged': True})

    report = {'scope': 'Actual NASM startup/event/init routines; mocked DOS and Intel ICH register interface',
              'physical_codec_tested': False, 'passed': True, 'checks': checks,
              'sources': {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in
                          ('src/com/ui_sound_driver.inc', 'src/com/ui_sound_startup.inc')},
              'fixture_sha256': hashlib.sha256(binary).hexdigest()}
    (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS native startup audio: {len(checks)} actual-NASM ring/lifecycle/failure cases')


if __name__ == '__main__':
    main()
