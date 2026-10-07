#!/usr/bin/env python3
"""Decode CiukiOS CVB1 SYSTEM/VIDEO/DISPLAY.LOG captures."""
import argparse
import json
import struct
import sys
from pathlib import Path

HEADER_SIZE = 32
MODE_SIZE = 256
CONTROLLER_SIZE = 512
EDID_SIZE = 128
TOTAL_SIZE = HEADER_SIZE + MODE_SIZE + CONTROLLER_SIZE + EDID_SIZE
DISPLAY_INFO_SIZE = 192
S3_STATUS_SIZE = 128
GPU_LOG_SIZE = 16 + 2 * DISPLAY_INFO_SIZE
TRACE_RECORD_SIZE = 16 + MODE_SIZE
TRACE_MAX_RECORDS = 128


def u16(buf, offset):
    return struct.unpack_from('<H', buf, offset)[0]


def u32(buf, offset):
    return struct.unpack_from('<I', buf, offset)[0]


def parse_display_info(data):
    if len(data) != DISPLAY_INFO_SIZE:
        raise ValueError(f'expected exactly {DISPLAY_INFO_SIZE} bytes for CVGD, got {len(data)}')
    if data[:4] != b'CVGD':
        raise ValueError(f'bad CVGD magic {data[:4]!r}; expected b\'CVGD\'')
    version, size = u16(data, 4), u16(data, 6)
    if version != 0x0100:
        raise ValueError(f'bad CVGD ABI version 0x{version:04X}; expected 0x0100')
    if size != DISPLAY_INFO_SIZE:
        raise ValueError(f'bad CVGD size {size}; expected {DISPLAY_INFO_SIZE}')
    flags = u32(data, 60)
    has_s3_status = bool(flags & 0x80000000)
    if flags & 0x7FFFFFF8:
        raise ValueError(f'nonzero reserved CVGD flags 0x{flags & 0x7FFFFFF8:08X}')
    result = {
        'format': 'CVGD', 'size_bytes': len(data), 'abi_version': version,
        'backend': u32(data, 8), 'phase': u32(data, 12),
        'width': u32(data, 16), 'height': u32(data, 20),
        'pitch': u32(data, 24), 'bpp': u32(data, 28),
        'error': u32(data, 32), 'command_words': u32(data, 36),
        'completions': u32(data, 40), 'physical_address': u32(data, 44),
        'framebuffer_bytes': u32(data, 48), 'edid_bytes': u32(data, 52),
        'active_dos_count': u32(data, 56),
        'flags': {'raw': flags, 'has_s3_status': has_s3_status,
                  'capabilities': flags & 7},
    }
    if has_s3_status:
        s3 = data[64:64 + S3_STATUS_SIZE]
        names = (
            'ready caps error_stage device_id mmio_physical framebuffer_physical '
            'aperture_physical vram_bytes scratch_offset scratch_bytes pitch bpp '
            'fills blits triangles fifo_timeouts idle_timeouts last_status binds '
            'fill_selftests width height command_words owns_engine triangle_selftests '
            'triangle_probe_failures triangle_probe_pixel triangle_probe_expected '
            'panel_width panel_height panel_flags copy_selftests'
        ).split()
        status = {name: u32(s3, i * 4) for i, name in enumerate(names)}
        result['s3_status'] = status
        result['s3_engine_qualified'] = bool(
            result['backend'] == 3 and status['ready'] == 1
            and status['owns_engine'] == 1
            and status['caps'] & 1
            and status['fill_selftests'] > 0
        )
        result['s3_triangles_qualified'] = bool(
            result['s3_engine_qualified'] and status['caps'] & 2 and status['caps'] & 4
            and status['triangle_selftests'] > 0 and status['copy_selftests'] > 0
            and status['triangle_probe_pixel'] == status['triangle_probe_expected']
        )
    else:
        result['s3_status'] = None
        result['s3_engine_qualified'] = False
        result['s3_triangles_qualified'] = False
    return result


def parse_gpu_log(data):
    if len(data) == DISPLAY_INFO_SIZE:
        return {'format': 'CVGD snapshot', 'snapshot': parse_display_info(data)}
    if len(data) != GPU_LOG_SIZE:
        raise ValueError(f'expected exactly {DISPLAY_INFO_SIZE} (CVGD) or {GPU_LOG_SIZE} (CG3D) bytes, got {len(data)}')
    if data[:4] != b'CG3D':
        raise ValueError(f'bad CG3D magic {data[:4]!r}; expected b\'CG3D\'')
    version, header_size = u16(data, 4), u16(data, 6)
    if version != 0x0100:
        raise ValueError(f'bad CG3D ABI version 0x{version:04X}; expected 0x0100')
    if header_size != 16:
        raise ValueError(f'bad CG3D header size {header_size}; expected 16')
    result_code, error = u32(data, 8), u32(data, 12)
    if result_code not in (0, 1, 2, 3):
        raise ValueError(f'unknown CG3D result {result_code}')
    snapshots = []
    for label, offset in (('before', 16), ('after', 208)):
        raw = data[offset:offset + DISPLAY_INFO_SIZE]
        if raw == bytes(DISPLAY_INFO_SIZE):
            if result_code == 0:
                raise ValueError(f'zero {label} CG3D snapshot is invalid for a completed result')
            snapshots.append({'available': False, 'reason': 'unavailable snapshot in error result'})
        else:
            snapshots.append({'available': True, 'data': parse_display_info(raw)})
    return {'format': 'CG3D native client log', 'size_bytes': len(data),
            'abi_version': version, 'header_size': header_size,
            'result': result_code,
            'result_name': ('completed', 'unsupported', 'session validation failed',
                            'hardware/counter verification failed')[result_code],
            'error': error, 'before': snapshots[0], 'after': snapshots[1]}


def parse_trace(data):
    if len(data) % TRACE_RECORD_SIZE:
        raise ValueError(f'CVT1 trace size must be a multiple of {TRACE_RECORD_SIZE}, got {len(data)}')
    count = len(data) // TRACE_RECORD_SIZE
    if count > TRACE_MAX_RECORDS:
        raise ValueError(f'CVT1 trace has {count} records; maximum is {TRACE_MAX_RECORDS}')
    records = []
    for index in range(count):
        offset = index * TRACE_RECORD_SIZE
        record = data[offset:offset + TRACE_RECORD_SIZE]
        if record[:4] != b'CVT1':
            raise ValueError(f'bad CVT1 magic in record {index}: {record[:4]!r}')
        result_code = record[7]
        if result_code not in (0, 1):
            raise ValueError(f'bad CVT1 result {result_code} in record {index}; expected 0 or 1')
        if record[10] not in (0, 1, 2):
            raise ValueError(f'bad CVT1 LFB mode {record[10]} in record {index}; expected 0, 1, or 2')
        reserved = u16(record, 14)
        if reserved:
            raise ValueError(f'nonzero CVT1 reserved field in record {index}: 0x{reserved:04X}')
        mode = record[16:]
        records.append({
            'mode': u16(record, 4), 'stage': record[6],
            'result': result_code, 'result_name': 'success' if result_code == 0 else 'failed',
            'vbe_4f01_ax': u16(record, 8),
            'lfb_mode': record[10],
            'framebuffer_path': ('banked', 'local', 'protected')[record[10]],
            'start_attempt': record[11],
            'failed_internal_status': u16(record, 12) if result_code else None,
            'mode_info': {'planes': mode[24], 'attributes': u16(mode, 0),
                          'width': u16(mode, 18), 'height': u16(mode, 20),
                          'bpp': mode[25], 'memory_model': mode[27],
                          'banked_pitch': u16(mode, 16),
                          'linear_pitch': u16(mode, 50),
                          'physical_base': u32(mode, 40)},
        })
    return {'format': 'CVT1', 'record_size_bytes': TRACE_RECORD_SIZE,
            'record_count': count, 'records': records}


def parse_log(data):
    if len(data) != TOTAL_SIZE:
        raise ValueError(f'expected exactly {TOTAL_SIZE} bytes, got {len(data)}')
    if data[:4] != b'CVB1':
        raise ValueError(f'bad magic {data[:4]!r}; expected b\'CVB1\'')
    mode = data[HEADER_SIZE:HEADER_SIZE + MODE_SIZE]
    controller = data[HEADER_SIZE + MODE_SIZE:HEADER_SIZE + MODE_SIZE + CONTROLLER_SIZE]
    edid = data[-EDID_SIZE:]
    flags = data[11]
    ui_vbe = data[8]
    width, height = u16(data, 4), u16(data, 6)
    mode_id = u16(data, 12)
    pitch = u16(data, 14)
    readback_ax, readback_bx = u16(data, 28), u16(data, 30)
    edid_header_ok = edid[:8] == bytes.fromhex('00ffffffffffff00')
    edid_version_ok = edid[18] == 1
    edid_checksum_ok = sum(edid) % 256 == 0
    preferred_timing = bool(edid[24] & 0x02)
    dtd = edid[54:72]
    dtd_clock_nonzero = u16(dtd, 0) != 0
    edid_width = edid_height = None
    preferred_edid_valid = (edid_header_ok and edid_version_ok and edid_checksum_ok
                            and preferred_timing and dtd_clock_nonzero)
    if preferred_edid_valid:
        edid_width = dtd[2] | ((dtd[4] & 0xF0) << 4)
        edid_height = dtd[5] | ((dtd[7] & 0xF0) << 4)
        if edid_width < 640 or edid_height < 480:
            edid_width = edid_height = None
            preferred_edid_valid = False
    window_attrs = mode[2]
    window_granularity_kb, window_size_kb = u16(mode, 4), u16(mode, 6)
    window_segment = u16(mode, 8)
    banked_pitch = u16(mode, 16)
    bpp = mode[25]
    memory_model = mode[27]
    linear_pitch = u16(mode, 50)
    masks = {
        'red': [mode[31], mode[32]], 'green': [mode[33], mode[34]],
        'blue': [mode[35], mode[36]], 'reserved': [mode[37], mode[38]],
        'linear_red': [mode[54], mode[55]], 'linear_green': [mode[56], mode[57]],
        'linear_blue': [mode[58], mode[59]], 'linear_reserved': [mode[60], mode[61]],
    }
    rgb_fields = [masks[channel] for channel in ('red', 'green', 'blue')]
    rgb_masks_valid = all(size > 0 and pos + size <= bpp for size, pos in rgb_fields)
    for i, (size_a, pos_a) in enumerate(rgb_fields):
        for size_b, pos_b in rgb_fields[i + 1:]:
            mask_a = ((1 << size_a) - 1) << pos_a if size_a else 0
            mask_b = ((1 << size_b) - 1) << pos_b if size_b else 0
            if mask_a & mask_b:
                rgb_masks_valid = False
    active_path = ('VGA mode 12h (16 colors, 4-bit depth); raw VBE ModeInfo is inactive'
                   if not ui_vbe else 'VBE mode')
    transport = []
    if flags & 1:
        transport.append('renderer entered bank-window path after mapping validation')
    if flags & 2:
        transport.append('renderer entered local-LFB path after range validation')
    rows = u32(data, 24)
    if rows:
        transport.append(f'{rows} protected session row transfers completed')
    checks = {
        'vbe_mode_readback_matches': not ui_vbe or (readback_ax == 0x004F and (readback_bx & 0x3FFF) == mode_id),
        'vbe_lfb_readback_matches_header': not ui_vbe or bool(readback_bx & 0x4000) == bool(data[10]),
        'modeinfo_geometry_matches_log': not ui_vbe or (u16(mode, 18) == width and u16(mode, 20) == height),
        'edid_header_valid': edid_header_ok,
        'edid_version_1': edid_version_ok,
        'edid_checksum_valid': edid_checksum_ok,
        'edid_preferred_timing_valid': preferred_edid_valid,
        'edid_preferred_geometry_matches_log': (edid_width, edid_height) == (width, height) if edid_width is not None else None,
        'full_color_direct_mode': bool(ui_vbe and bpp in (15, 16, 24, 32) and memory_model == 6),
        'rgb_masks_valid': bool(ui_vbe and rgb_masks_valid),
        'header_bpp_matches_modeinfo': not ui_vbe or data[9] == bpp,
        'usable_banked_window': bool((window_attrs & 5) == 5 and window_granularity_kb and 64 % window_granularity_kb == 0 and window_size_kb >= 64 and window_segment in (0, 0xA000)),
        'linear_framebuffer_descriptor': bool(mode[0] & 0x80 and u32(mode, 40) and (linear_pitch or u16(controller, 4) < 0x0300)),
    }
    return {
        'format': 'CVB1', 'size_bytes': len(data), 'geometry': {'width': width, 'height': height},
        'renderer': {'ui_vbe': ui_vbe, 'active_path': active_path, 'bpp_header': data[9], 'vclfb_header': data[10], 'mode_id': mode_id, 'active_pitch': pitch},
        'transport_flags': {'raw': flags, 'bank_window_path_entered': bool(flags & 1), 'local_lfb_path_entered': bool(flags & 2), 'reserved_bits': flags & ~3},
        'transport_evidence': transport,
        'attempts': {'last_failure_stage': data[16], 'start_attempts': data[17], 'failed_mode': u16(data, 18), 'failed_status_ax': u16(data, 20)},
        'session': {'bound': data[22], 'gpu': data[23], 'row_calls': rows},
        'readback': {'ax': readback_ax, 'bx': readback_bx, 'current_mode_id': readback_bx & 0x3FFF, 'lfb_active': bool(readback_bx & 0x4000)},
        'mode_info': {'width': u16(mode, 18), 'height': u16(mode, 20), 'bpp': bpp, 'memory_model': memory_model, 'modeinfo_pitch_field': banked_pitch, 'linear_pitch': linear_pitch, 'window_attributes': window_attrs, 'window_granularity_kb': window_granularity_kb, 'window_size_kb': window_size_kb, 'window_segment': window_segment, 'masks': masks},
        'controller': {'signature': controller[:4].decode('ascii', errors='replace'), 'version': u16(controller, 4)},
        'edid': {'header_valid': edid_header_ok, 'version_1': edid_version_ok, 'checksum_valid': edid_checksum_ok, 'preferred_timing_bit': preferred_timing, 'preferred_timing_valid': preferred_edid_valid, 'preferred_width': edid_width, 'preferred_height': edid_height},
        'checks': checks,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--video', required=True, type=Path, help='path to SYSTEM/VIDEO/DISPLAY.LOG')
    parser.add_argument('--gpu', type=Path, help='path to a 192-byte CVGD snapshot or 400-byte CG3D log')
    parser.add_argument('--trace', type=Path, help='path to a CVT1 mode-attempt trace')
    parser.add_argument('--output', type=Path, help='write JSON here instead of stdout')
    args = parser.parse_args(argv)
    try:
        result = parse_log(args.video.read_bytes())
        if args.gpu:
            result['gpu'] = parse_gpu_log(args.gpu.read_bytes())
        if args.trace:
            result['trace'] = parse_trace(args.trace.read_bytes())
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    rendered = json.dumps(result, indent=2, sort_keys=True) + '\n'
    if args.output:
        args.output.write_text(rendered, encoding='utf-8')
    else:
        sys.stdout.write(rendered)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
