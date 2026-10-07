#!/usr/bin/env python3
"""Run actual NASM SAV3D instructions with mocked CVSESSION/DOS services.

This validates the diagnostic client and its evidence, not physical GPU output.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

from unicorn import Uc, UC_ARCH_X86, UC_HOOK_CODE, UC_HOOK_INTR, UC_MODE_16
from unicorn.x86_const import (
    UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_CS, UC_X86_REG_CX,
    UC_X86_REG_DI, UC_X86_REG_DS, UC_X86_REG_DX, UC_X86_REG_EFLAGS,
    UC_X86_REG_ES, UC_X86_REG_IP, UC_X86_REG_SP, UC_X86_REG_SS,
)

ROOT = Path(__file__).resolve().parents[1]
INFO_MAGIC = 0x534D5643
DISPLAY_MAGIC = 0x44475643
TRIANGLE_CAP = 0x00100000


def run_case(binary: bytes, case: str) -> dict:
    uc = Uc(UC_ARCH_X86, UC_MODE_16)
    uc.mem_map(0, 0x100000)
    uc.mem_write(0x10100, binary)
    uc.mem_write(0x20100, b'\xcb')  # Actual RETF executes after mock service.
    for reg in (UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS):
        uc.reg_write(reg, 0x1000)
    uc.reg_write(UC_X86_REG_IP, 0x100)
    uc.reg_write(UC_X86_REG_SP, 0xfffe)
    state = {'exit': None, 'printed': [], 'draws': 0, 'info_calls': 0,
             'triangle_count': 8, 'blits': 10, 'words': 100,
             'log': b'', 'key_waits': 0}

    def carry(value: bool):
        flags = uc.reg_read(UC_X86_REG_EFLAGS)
        uc.reg_write(UC_X86_REG_EFLAGS, (flags | 1) if value else (flags & ~1))

    def string(address: int, terminator: int) -> bytes:
        data = bytearray()
        for i in range(512):
            value = uc.mem_read(address + i, 1)[0]
            if value == terminator:
                return bytes(data)
            data.append(value)
        raise AssertionError('Unterminated client string')

    def interrupt(_uc, number, _data):
        ax = uc.reg_read(UC_X86_REG_AX)
        if number == 0x2f:
            assert ax == 0x1684 and uc.reg_read(UC_X86_REG_BX) == 0x4349
            uc.reg_write(UC_X86_REG_ES, 0 if case == 'no_monitor' else 0x2000)
            uc.reg_write(UC_X86_REG_DI, 0 if case == 'no_monitor' else 0x100)
        elif number == 0x16:
            assert state['draws'] == 1 and ax >> 8 == 0
            state['key_waits'] += 1
            uc.reg_write(UC_X86_REG_AX, 0x011b)
        elif number == 0x21:
            ah = ax >> 8
            address = (uc.reg_read(UC_X86_REG_DS) << 4) + uc.reg_read(UC_X86_REG_DX)
            if ah == 0x4a:
                carry(False)
            elif ah == 9:
                state['printed'].append(string(address, ord('$')).decode('ascii'))
            elif ah == 0x3c:
                assert string(address, 0) == b'\\SYSTEM\\VIDEO\\GPU3D.LOG'
                failed = case in ('log_create_failure', 'hardware_and_log_failure')
                carry(failed)
                uc.reg_write(UC_X86_REG_AX, 5 if failed else 7)
            elif ah == 0x40:
                assert uc.reg_read(UC_X86_REG_BX) == 7
                count = uc.reg_read(UC_X86_REG_CX)
                assert count == 400
                state['log'] = bytes(uc.mem_read(address, count))
                uc.reg_write(UC_X86_REG_AX, count - 1 if case == 'short_log_write' else count)
                carry(False)
            elif ah == 0x3e:
                assert uc.reg_read(UC_X86_REG_BX) == 7
                carry(False)
            elif ah == 0x4c:
                state['exit'] = ax & 255
                uc.emu_stop()
            else:
                raise AssertionError(f'Unexpected DOS service {ah:02x}')
        else:
            raise AssertionError(f'Unexpected interrupt {number:02x}')

    def monitor(_uc, address, _size, _data):
        if address != 0x20100:
            return
        operation = uc.reg_read(UC_X86_REG_AX)
        assert operation & 0x100, 'client must request NO_SWITCH'
        operation &= 255
        destination = (uc.reg_read(UC_X86_REG_ES) << 4) + uc.reg_read(UC_X86_REG_DI)
        if operation == 0:
            assert uc.reg_read(UC_X86_REG_CX) == 64
            packet = bytearray(64)
            struct.pack_into('<IHH', packet, 0, INFO_MAGIC, 0x100, 64)
            struct.pack_into('<I', packet, 8, 0 if case == 'no_native' else TRIANGLE_CAP)
            struct.pack_into('<I', packet, 12, int(case == 'active_dos'))
            struct.pack_into('<I', packet, 40, 0 if case == 'unbound' else 1)
            if case == 'bad_query_abi':
                struct.pack_into('<H', packet, 4, 0x200)
            uc.mem_write(destination, bytes(packet))
            uc.reg_write(UC_X86_REG_AX, 0x100)
            carry(False)
        elif operation == 0x14:
            assert uc.reg_read(UC_X86_REG_CX) == 192
            packet = bytearray(192)
            struct.pack_into('<IHH', packet, 0, DISPLAY_MAGIC, 0x100, 192)
            struct.pack_into('<IIIIII', packet, 8, 0 if case == 'no_native' else 3,
                             1, 640, 480, 2560, 32)
            struct.pack_into('<I', packet, 60, 0x80000002)
            struct.pack_into('<IIII', packet, 64, 1, 3, 0, 0x8c2e5333)
            struct.pack_into('<III', packet, 64 + 48, 7, state['blits'], state['triangle_count'])
            struct.pack_into('<I', packet, 64 + 76, 1)
            struct.pack_into('<I', packet, 64 + 88, state['words'])
            struct.pack_into('<I', packet, 64 + 92, 1)
            struct.pack_into('<I', packet, 64 + 96, 1)
            if case == 'no_triangle_probe':
                struct.pack_into('<I', packet, 64 + 96, 0)
            if case == 'bad_display_abi':
                struct.pack_into('<H', packet, 4, 0x200)
            if case == 'too_small':
                struct.pack_into('<I', packet, 20, 200)
            if case in ('hardware_failure', 'hardware_and_log_failure') and state['draws']:
                struct.pack_into('<II', packet, 64, 0, 0)
                struct.pack_into('<I', packet, 64 + 8, 6)
            uc.mem_write(destination, bytes(packet))
            state['info_calls'] += 1
            # Prove client uses its saved DI rather than untrusted returned DI.
            uc.reg_write(UC_X86_REG_DI, 0xdead)
            uc.reg_write(UC_X86_REG_AX, 0)
            carry(False)
        elif operation == 0x16:
            assert uc.reg_read(UC_X86_REG_CX) == 64
            packet = bytes(uc.mem_read(destination, 64))
            assert struct.unpack_from('<IIII', packet) == (0x33545643, 64, 0, 0)
            expected = [(32.0,64.0,.5,0xffff0000), (192.0,64.0,.5,0xff00ff00),
                        (32.0,224.0,.5,0xff0000ff)]
            for i, vertex in enumerate(expected):
                assert struct.unpack_from('<fffI', packet, 16 + 16 * i) == vertex
            state['draws'] += 1
            failed = case in ('hardware_failure', 'hardware_and_log_failure')
            if not failed and case != 'counter_did_not_advance':
                state['triangle_count'] += 1
                state['blits'] += 2
                state['words'] += 47
            uc.reg_write(UC_X86_REG_AX, 9 if failed else 0)
            carry(failed)
        else:
            raise AssertionError(f'Unexpected monitor operation {operation:02x}')

    uc.hook_add(UC_HOOK_INTR, interrupt)
    uc.hook_add(UC_HOOK_CODE, monitor)
    uc.emu_start(0x10100, 0x100000, count=30000)
    assert state['exit'] is not None, 'client did not terminate within instruction bound'
    expected_exit = {
        'success': 0,
        'no_monitor': 1, 'no_native': 1, 'unbound': 1,
        'no_triangle_probe': 1, 'too_small': 1,
        'bad_query_abi': 2, 'bad_display_abi': 2, 'active_dos': 2,
        'hardware_failure': 2, 'counter_did_not_advance': 2,
        'log_create_failure': 3, 'short_log_write': 3,
        'hardware_and_log_failure': 3,
    }[case]
    assert state['exit'] == expected_exit, state
    expected_draws = int(case in ('success', 'hardware_failure', 'counter_did_not_advance',
                                 'log_create_failure', 'short_log_write', 'hardware_and_log_failure'))
    assert state['draws'] == expected_draws
    if state['log']:
        assert state['log'][:8] == b'CG3D\x00\x01\x10\x00'
        result, error = struct.unpack_from('<II', state['log'], 8)
        if case == 'success':
            assert result == 0 and error == 0 and state['key_waits'] == 1
            before = struct.unpack_from('<I', state['log'], 16 + 64 + 56)[0]
            after = struct.unpack_from('<I', state['log'], 208 + 64 + 56)[0]
            assert after == before + 1
        elif case in ('hardware_failure', 'counter_did_not_advance'):
            assert result == 3
    if case in ('hardware_failure', 'hardware_and_log_failure', 'counter_did_not_advance'):
        assert not state['printed'], 'failed GPU operation must not write its owned framebuffer'
    if expected_exit == 1:
        assert any('unsupported' in message for message in state['printed'])
    return {key: value for key, value in state.items() if key != 'log'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT/'build/tests/sav3d-client')
    args = parser.parse_args()
    output = args.output.resolve()
    if ROOT/'build' not in output.parents:
        parser.error('--output must be under ignored build/')
    output.mkdir(parents=True, exist_ok=True)
    binary_path = output/'SAV3D.COM'
    subprocess.run(['nasm', '-f', 'bin', '-I', str(ROOT)+'/', str(ROOT/'src/com/sav3d.asm'),
                    '-o', str(binary_path)], cwd=ROOT, check=True)
    binary = binary_path.read_bytes()
    names = ['success', 'no_monitor', 'no_native', 'unbound', 'no_triangle_probe',
             'too_small', 'bad_query_abi', 'bad_display_abi', 'active_dos',
             'hardware_failure', 'counter_did_not_advance', 'log_create_failure',
             'short_log_write', 'hardware_and_log_failure']
    results = {name: run_case(binary, name) for name in names}
    record = {'binary_sha256': hashlib.sha256(binary).hexdigest(),
              'binary_bytes': len(binary), 'scenarios': results, 'passed': True,
              'physical_gpu_qualification': False}
    (output/'result.json').write_text(json.dumps(record, indent=2)+'\n')
    print(f'SAV3D actual NASM client: {len(names)} scenarios PASS')


if __name__ == '__main__':
    main()
