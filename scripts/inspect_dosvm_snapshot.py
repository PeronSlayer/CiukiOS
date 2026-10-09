#!/usr/bin/env python3
"""Decode the fixed SYSTEM/DOSVM1.BIN..DOSVM3.BIN physical VM snapshots."""
import argparse
import json
from pathlib import Path
import struct
import sys

VERSION = 0x0100
HEADER_SIZE = 64
VIDEO_SIZE = 256
DEVICE_SIZE = 128
TOTAL_SIZE = HEADER_SIZE + VIDEO_SIZE + DEVICE_SIZE
PIT_TICKS_PER_SECOND = 1193182 / 65536
REASONS = {1: 'live_timeout', 2: 'observed_exit', 3: 'user_close', 4: 'launch_held'}
STATES = {0: 'free', 1: 'ready', 2: 'dead'}


def u16(data, offset):
    return struct.unpack_from('<H', data, offset)[0]


def u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]


def packet_abi(data, magic, size):
    if data[:4] != magic:
        raise ValueError(f'bad {magic.decode()} packet magic {data[:4]!r}')
    if u16(data, 4) != VERSION:
        raise ValueError(f'bad {magic.decode()} packet version 0x{u16(data, 4):04X}')
    if u16(data, 6) != size:
        raise ValueError(f'bad {magic.decode()} packet size {u16(data, 6)}; expected {size}')


def query_result(code):
    kind = {0: 'success', 0xFFFE: 'invalid_packet', 0xFFFF: 'unavailable'}.get(
        code, 'vm_error')
    return {'code': code, 'result': kind}


def sample_age(now, then):
    ticks = (now - then) & 0xFFFF
    return {'tick': then, 'age_ticks': ticks,
            'age_seconds': round(ticks / PIT_TICKS_PER_SECOND, 3)}


def parse_video(data):
    packet_abi(data, b'CVVS', VIDEO_SIZE)
    fields = {
        8: 'attached', 12: 'session_generation', 16: 'faults',
        20: 'instructions', 24: 'elements', 28: 'unsupported',
        32: 'bus_faults', 36: 'port_reads', 40: 'port_writes',
        44: 'bios_calls', 48: 'bios_unsupported', 52: 'bios_last_ax',
        56: 'mode_sets', 60: 'bios_mode', 64: 'width', 68: 'rows',
        72: 'scan_repeat', 76: 'text', 80: 'display_changes',
        84: 'changes', 88: 'fatal', 92: 'fatal_csip',
        112: 'presents', 116: 'present_rows', 120: 'present_pixels',
        124: 'frames', 128: 'full_redraws', 132: 'last_present_cycles',
        136: 'max_present_cycles', 148: 'host_mode', 156: 'host_faults',
        164: 'tsc_khz', 168: 'status_polls', 180: 'crtc_start',
        196: 'port_rejects', 200: 'last_fault_csip', 204: 'pm_faults',
        208: 'pm_instructions', 212: 'pm_unsupported', 216: 'pm_attached',
        220: 'passes', 224: 'string_io', 228: 'string_elements',
        232: 'host_display_sets', 236: 'damage_queries',
        240: 'captured_images', 244: 'converted_rows',
        248: 'surface_bytes', 252: 'surface_ready',
    }
    result = {name: u32(data, offset) for offset, name in fields.items()}
    result['fatal_instruction_bytes'] = data[96:112].hex()
    result['total_present_cycles'] = struct.unpack_from('<Q', data, 140)[0]
    result['fault_cycles'] = struct.unpack_from('<Q', data, 172)[0]
    result['scanout_kind'] = ('unavailable' if not result['attached'] or
                              not result['width'] or not result['rows'] else
                              'text' if result['text'] else 'graphics')
    return result


def parse_devices(data):
    packet_abi(data, b'CVDV', DEVICE_SIZE)
    names = (
        'active caps focused audio keys_forwarded keys_dropped aux_bytes lazy_pulls '
        'irqs_raised irq_failures port_reads port_writes unclaimed_io model_errors '
        'last_error last_port buffers_rendered underruns polls sb_blocks '
        'opl_writes dsp_commands ac97_nam ac97_nabm bridge_calls rate '
        'ac97_cursor ac97_status ac97_queued pm_irq_state'
    ).split()
    result = {name: u32(data, 8 + i * 4) for i, name in enumerate(names)}
    result['audio_state'] = {0: 'none', 1: 'running', 2: 'no_device',
                             3: 'no_rate'}.get(result['audio'], 'unknown')
    result['capabilities'] = [name for bit, name in (
        (1, 'keyboard'), (2, 'mouse'), (4, 'pic_pit'),
        (8, 'sound_blaster_dma'), (16, 'opl3')) if result['caps'] & bit]
    irq = result['pm_irq_state']
    result['protected_irq'] = {'claimed': bool(irq & 0x80000000),
                               'held_mask': (irq >> 16) & 0x7FFF,
                               'delivered_low_word': irq & 0xFFFF}
    return result


def parse_snapshot(data):
    if len(data) != TOTAL_SIZE:
        raise ValueError(f'expected exactly {TOTAL_SIZE} bytes, got {len(data)}')
    packet_abi(data, b'CDVS', TOTAL_SIZE)
    vm, mask, reason, state = (u16(data, offset) for offset in (8, 14, 16, 18))
    if vm not in (1, 2, 3):
        raise ValueError(f'bad VM slot {vm}; expected 1..3')
    if mask & ~3:
        raise ValueError(f'unknown valid-mask bits 0x{mask & ~3:04X}')
    if reason not in REASONS:
        raise ValueError(f'bad snapshot reason {reason}')
    if state not in STATES:
        raise ValueError(f'bad VM state {state}')
    title = data[32:64]
    if b'\0' not in title:
        raise ValueError('window title is not NUL-terminated')
    now = u16(data, 12)
    result = {
        'format': 'CDVS', 'abi_version': VERSION, 'size_bytes': TOTAL_SIZE,
        'vm': vm, 'generation': u16(data, 10), 'host_tick': now,
        'valid_mask': mask, 'reason': REASONS[reason],
        'state': STATES[state], 'exit_code': u16(data, 20),
        'title': title.split(b'\0', 1)[0].decode('cp437'),
        'last_attempt': sample_age(now, u16(data, 26)),
        'video_query': query_result(u16(data, 22)),
        'device_query': query_result(u16(data, 24)),
        'video': None, 'devices': None,
    }
    for bit, key, offset, size, tick_offset, query_key, parser in (
        (1, 'video', HEADER_SIZE, VIDEO_SIZE, 28, 'video_query', parse_video),
        (2, 'devices', HEADER_SIZE + VIDEO_SIZE, DEVICE_SIZE, 30,
         'device_query', parse_devices),
    ):
        packet = data[offset:offset + size]
        if mask & bit:
            result[key] = parser(packet)
            result[key]['sample'] = sample_age(now, u16(data, tick_offset))
            result[key]['latest_query_succeeded'] = result[query_key]['code'] == 0
        elif any(packet):
            raise ValueError(f'nonzero {key} packet without its valid-mask bit')
        elif result[query_key]['code'] == 0:
            raise ValueError(f'{key} query claims success without a validated packet')
    return result


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('path', type=Path)
    args = ap.parse_args(argv)
    try:
        result = parse_snapshot(args.path.read_bytes())
    except (OSError, ValueError) as error:
        print(f'{args.path}: {error}', file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
