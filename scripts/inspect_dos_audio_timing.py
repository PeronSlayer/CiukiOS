#!/usr/bin/env python3
"""Decode the optional fixed128-byte CVAT DOS transport timing packet."""
import argparse
import json
from pathlib import Path
import struct

FIELDS = (
    'generation active dac_rate source_rate sb_rate sb_block_units '
    'sb_units_left sb_format services buffers_rendered sb_boundaries '
    'boundary_yields late_acks wait_polls dma_halts fifo_errors last_gap_us '
    'max_gap_us last_render_cycles max_render_cycles total_render_lo '
    'total_render_hi source_frames output_frames queued partial_frames '
    'irq_status dma_address dma_count flags'
).split()

def parse_audio_timing(data):
    if len(data) != 128 or data[:4] != b'CVAT':
        raise ValueError('expected exact128-byte CVAT record')
    version, size = struct.unpack_from('<HH', data, 4)
    if version != 0x0100 or size != 128:
        raise ValueError('unsupported CVAT version/size')
    values = dict(zip(FIELDS, struct.unpack_from('<30I', data, 8)))
    values.update(format='CVAT', abi_version=version, size_bytes=size)
    values['total_render_cycles'] = values['total_render_lo'] | values['total_render_hi'] << 32
    values['sound_blaster'] = dict(
        bits=values['sb_format'] & 255, stereo=bool(values['sb_format'] & 256),
        auto_init=bool(values['sb_format'] & 512), active=bool(values['sb_format'] & 1024))
    return values

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('record', type=Path)
    args = parser.parse_args()
    print(json.dumps(parse_audio_timing(args.record.read_bytes()), indent=2))
