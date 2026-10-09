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
CACHE_DIAG_HEADER_SIZE = 16
CACHE_RECORD_SIZE = 576
CACHE_TIME_SIZE = 32
CACHE_DIAG_SIZE = CACHE_DIAG_HEADER_SIZE + CACHE_RECORD_SIZE + CACHE_TIME_SIZE
TRACE_RECORD_SIZE = 16 + MODE_SIZE
TRACE_MAX_RECORDS = 128


def u16(buf, offset):
    return struct.unpack_from('<H', buf, offset)[0]


def u32(buf, offset):
    return struct.unpack_from('<I', buf, offset)[0]


def u64(buf, offset):
    return struct.unpack_from('<Q', buf, offset)[0]


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
    has_mach64_status = bool(flags & 0x40000000)
    if has_s3_status and has_mach64_status:
        raise ValueError('ambiguous native GPU status flags')
    if flags & 0x3FFFFFF8:
        raise ValueError(f'nonzero reserved CVGD flags 0x{flags & 0x3FFFFFF8:08X}')
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
                  'has_mach64_status': has_mach64_status,
                  'capabilities': flags & 7},
    }
    if has_mach64_status:
        names = (
            'ready caps error_stage device_id mmio_physical framebuffer_physical '
            'aperture_physical vram_bytes scratch_offset scratch_bytes pitch bpp '
            'fills blits triangles fifo_timeouts idle_timeouts last_status binds '
            'fill_selftests width height command_words owns_engine triangle_selftests '
            'triangle_probe_failures probe_actual probe_expected '
            'panel_width panel_height panel_flags copy_selftests'
        ).split()
        status = {name: u32(data, 64 + i * 4) for i, name in enumerate(names)}
        result['mach64_status'] = status
        result['mach64_engine_qualified'] = (
            result['backend'] == 4 and status['ready'] == 1 and
            status['owns_engine'] == 1 and status['caps'] == 5)
        result['mach64_triangles_qualified'] = False
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
        # Newer snapshots retain the low-byte stage and tag stage-1 rejects
        # with one preflight predicate, without extending the 128-byte ABI.
        stage = status['error_stage'] & 0xFF
        reason = status['error_stage'] & 0x000FFF00
        predicates = {
            0x100: ('format', 'mapped_bytes'),
            0x200: ('geometry', 'mapped_bytes'),
            0x400: ('pitch', 'mapped_bytes'),
            0x800: ('extent', 'mapped_bytes'),
            0x1000: ('missing_device', 'requested_framebuffer_physical'),
            0x2000: ('pci_class', 'raw_pci_class'),
            0x4000: ('memory_decode', 'raw_pci_command'),
            0x8000: ('mmio_bar', 'raw_bar0'),
            0x10000: ('framebuffer_bar', 'raw_bar1'),
            0x20000: ('aperture_bar', 'raw_bar2'),
            0x40000: ('framebuffer_address', 'requested_framebuffer_physical'),
            0x80000: ('mmio_span', 'mmio_base'),
        }
        status['stage'] = stage
        status['preflight'] = None
        status['setup_readback'] = None
        status['fill_readback'] = None
        status['release_readback'] = None
        if stage == 1 and reason:
            name, detail = predicates.get(reason, ('unknown', 'raw_detail'))
            status['preflight'] = {'reason_code': reason, 'reason': name,
                                   detail: status['last_status']}
        if stage == 3 and status['error_stage'] >> 16:
            check = status['error_stage'] >> 16
            pixelbytes = status['bpp'] // 8
            descriptor = (0x10000001 | (status['bpp'] << 16) |
                          (status['pitch'] // pixelbytes)) if pixelbytes else None
            readbacks = {
                1: ('MM48C18 enabled', 0x0e, 8),
                2: ('MM8168 GBD low', 0xffffffff, 0),
                3: ('MM816C GBD high', 0xffffffff, descriptor),
                4: ('CR50 execution width/depth', 0xf1,
                    0xf1 if status['bpp'] == 32 else 0xd1),
                5: ('CR31', 0x0d, 0x0c),
                6: ('MM8128 plane write mask', 0xffffffff, 0xffffffff),
                7: ('MM48C18 disabled', 0x0e, 0),
                8: ('CR66 engine enable', 1, 1),
                9: ('MM8170 initial PBD low', 0xffffffff, 0),
                10: ('MM8174 initial PBD high', 0xffffffff,
                     descriptor & ~1 if descriptor is not None else None),
                11: ('MM8178 initial SBD low', 0xffffffff, 0),
                12: ('MM817C initial SBD high', 0xffffffff,
                     descriptor & ~1 if descriptor is not None else None),
                13: ('MM8144 32-bit register access', 0x200, 0x200),
            }
            name, mask, expected = readbacks.get(check, ('unknown', 0xffffffff, None))
            status['setup_readback'] = {
                'id': check, 'register': name, 'mask': mask, 'expected': expected,
                'observed': status['last_status'],
                'masked_observed': status['last_status'] & mask,
            }
        fill_tag = status['error_stage'] & 0xf0000000
        if stage == 8 and fill_tag in (0xd0000000, 0xe0000000):
            check = (status['error_stage'] >> 24) & 0xf
            names = {1: 'DWORD restore completion', 2: 'MM8144 write completion',
                     3: 'MM8144 documented payload', 4: 'original CONTROL completion',
                     5: 'original RSF replay completion'}
            misc_mask = 0xbaf if fill_tag == 0xe0000000 else 0xbbf
            if fill_tag == 0xe0000000:
                names[3] = 'MM8144 persistent controls (RSF excluded)'
            status['release_readback'] = {
                'id': check, 'reason': names.get(check, 'unknown'),
                'expected_misc_payload': (status['error_stage'] >> 12) & 0xbbf,
                'observed': status['last_status'],
                'mask': misc_mask if check == 3 else None,
                'masked_observed': status['last_status'] & misc_mask if check == 3 else None,
                'original_error_stage': status['triangle_probe_pixel'],
                'original_last_status': status['triangle_probe_expected'],
            }
        status['packed_2d'] = bool(status['caps'] & 8)
        if stage == 4 and fill_tag == 0xf0000000:
            check = (status['error_stage'] >> 24) & 7
            pixel = 1 <= check <= 5
            predicate = (f'private packed fill DWORD {check - 1}' if check <= 4 else
                         {5: 'private packed two-row copy', 6: 'packed fill completion',
                          7: 'packed copy completion'}.get(check, 'unknown'))
            mask = (0xffffff if status['bpp'] == 32 else 0xffffffff) if pixel else None
            status['fill_readback'] = {
                'id': check, 'predicate': predicate, 'interface': 'packed_mmio',
                'pixel_proof_failed': pixel, 'completion_failed': not pixel,
                'original_cr50': (status['error_stage'] >> 8) & 0xff,
                'active_cr50': (status['error_stage'] >> 16) & 0xff,
                'original_32bit_register_access': bool(status['error_stage'] & 0x08000000),
                'observed': status['triangle_probe_pixel'] if pixel else status['last_status'],
                'expected': status['triangle_probe_expected'] if pixel else None, 'mask': mask,
                'masked_observed': status['triangle_probe_pixel'] & mask if pixel else None,
                'foreground_color': status['last_status'] if pixel else None,
            }
        if stage == 4 and fill_tag in (0xa0000000, 0xb0000000, 0xc0000000):
            check = (status['error_stage'] >> 24) & (7 if fill_tag == 0xc0000000 else 0xf)
            predicate = {1: 'MM8170 PBD low', 2: 'MM8174 PBD high'}.get(
                check, f'private fill DWORD {check - 3}' if 3 <= check <= 6 else 'unknown')
            mask = 0xffffff if 3 <= check <= 6 and status['bpp'] == 32 else 0xffffffff
            status['fill_readback'] = {
                'id': check, 'predicate': predicate,
                'pixel_proof_failed': True,
                'original_cr50': (status['error_stage'] >> 8) & 0xff,
                'active_cr50': (status['error_stage'] >> 16) & 0xff,
                'observed': status['triangle_probe_pixel'],
                'expected': status['triangle_probe_expected'], 'mask': mask,
                'masked_observed': status['triangle_probe_pixel'] & mask,
                'foreground_color': status['last_status']
                    if fill_tag in (0xb0000000, 0xc0000000) else None,
                'original_32bit_register_access': bool(status['error_stage'] & 0x08000000)
                    if fill_tag == 0xc0000000 else None,
            }
        result['s3_status'] = status
        result['s3_engine_qualified'] = bool(
            result['backend'] == 3 and status['ready'] == 1
            and status['owns_engine'] == 1
            and status['caps'] & 1
            and status['fill_selftests'] > 0
            and status['fill_readback'] is None
            and status['release_readback'] is None
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
    qualification = None
    if result_code in (0, 1, 2, 3):
        result_name = ('completed', 'unsupported', 'session validation failed',
                       'hardware/counter verification failed')[result_code]
    else:
        phase, state, prefix = result_code & 0xff, (result_code >> 8) & 0xff, result_code >> 16
        if (phase not in (1, 2, 3, 4, 5) or state not in (1, 2, 3) or
                (prefix and (phase != 2 or not 1 <= prefix <= 21))):
            raise ValueError(f'unknown CG3D result {result_code}')
        result_name = ('before call', 'returned success', 'returned error')[state - 1]
        qualification = {'phase': phase, 'setup_prefix': prefix, 'state': result_name}
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
            'result_name': result_name, 'qualification': qualification,
            'error': error, 'before': snapshots[0], 'after': snapshots[1]}


def parse_cache_diagnostics(data):
    if len(data) != CACHE_DIAG_SIZE:
        raise ValueError(f'expected exactly {CACHE_DIAG_SIZE} bytes for CVFD, got {len(data)}')
    if data[:4] != b'CVFD':
        raise ValueError(f'bad CVFD magic {data[:4]!r}; expected b\'CVFD\'')
    version, size = u16(data, 4), u16(data, 6)
    cache_size, timing_size = u32(data, 8), u32(data, 12)
    if version != 0x0100:
        raise ValueError(f'bad CVFD ABI version 0x{version:04X}; expected 0x0100')
    if size != CACHE_DIAG_SIZE:
        raise ValueError(f'bad CVFD size {size}; expected {CACHE_DIAG_SIZE}')
    if cache_size != CACHE_RECORD_SIZE or timing_size != CACHE_TIME_SIZE:
        raise ValueError('CVFD embedded record sizes do not match version 1')
    cache = data[CACHE_DIAG_HEADER_SIZE:CACHE_DIAG_HEADER_SIZE + CACHE_RECORD_SIZE]
    timing = data[CACHE_DIAG_HEADER_SIZE + CACHE_RECORD_SIZE:]
    if cache[:8] != b'CVFBCACH':
        raise ValueError(f'bad cache-record magic {cache[:8]!r}')
    if u32(cache, 8) != CACHE_RECORD_SIZE:
        raise ValueError(f'bad CVFBCACH size {u32(cache, 8)}')
    if timing[:8] != b'CVFBTIME':
        raise ValueError(f'bad copy-time magic {timing[:8]!r}')
    variable_count = u64(cache, 48) & 0xff
    if variable_count > 32:
        raise ValueError(f'CVFBCACH reports {variable_count} variable MTRRs; maximum is 32')
    ranges = []
    for index in range(variable_count):
        base = u64(cache, 64 + index * 16)
        mask = u64(cache, 72 + index * 16)
        ranges.append({'index': index, 'base': base, 'mask': mask,
                       'type': base & 0xff, 'enabled': bool(mask & (1 << 11))})
    cycles = u32(timing, 12) | (u32(timing, 16) << 32)
    last_start = u32(timing, 24) | (u32(timing, 28) << 32)
    return {
        'format': 'CVFD framebuffer cache diagnostics', 'size_bytes': len(data),
        'abi_version': version, 'header_size': CACHE_DIAG_HEADER_SIZE,
        'cache_record': {
            'live': u32(cache, 12), 'physical_address': u32(cache, 16),
            'extent_bytes': u32(cache, 20), 'first_pte': u32(cache, 24),
            'cpuid_edx': u32(cache, 28), 'cr0': u32(cache, 32),
            'cr4': u32(cache, 36), 'pat': u64(cache, 40),
            'mtrrcap': u64(cache, 48), 'mtrr_def_type': u64(cache, 56),
            'variable_range_count': variable_count, 'variable_ranges': ranges,
        },
        'copy_timing': {'calls': u32(timing, 8), 'cycles': cycles,
                        'max_cycles': u32(timing, 20), 'last_start_tsc': last_start},
    }


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
    parser.add_argument('--cache', type=Path, help='path to a 624-byte CVFD framebuffer cache snapshot')
    parser.add_argument('--trace', type=Path, help='path to a CVT1 mode-attempt trace')
    parser.add_argument('--output', type=Path, help='write JSON here instead of stdout')
    args = parser.parse_args(argv)
    try:
        result = parse_log(args.video.read_bytes())
        if args.gpu:
            result['gpu'] = parse_gpu_log(args.gpu.read_bytes())
        if args.cache:
            result['cache_diagnostics'] = parse_cache_diagnostics(args.cache.read_bytes())
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
