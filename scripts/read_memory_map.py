#!/usr/bin/env python3
"""Decode the pre-Jemm BIOS E820 capture in SYSTEM/MEMMAP.BIN."""

import argparse
import json
import struct
from pathlib import Path

HEADER = struct.Struct('<4sBBHHHHH')
ENTRY = struct.Struct('<QQII')
STATUS = ('complete', 'unavailable_or_invalid', 'truncated')
PAGE_SIZE = 4096
INITIAL_32BIT_LIMIT = 1 << 32


def firmware_page_candidates(memory: dict) -> list[tuple[int, int]]:
    """Return 4 KiB type-1 ranges; these pages are not yet owned or free.

    A native allocator must additionally exclude its own image, boot data,
    page tables and every page currently controlled by JemmEx/XMS.
    """
    if memory['status'] != 'complete' or memory['overlapping_entries']:
        raise ValueError('cannot derive page candidates from an incomplete or overlapping map')
    ranges = []
    for entry in sorted(memory['entries'], key=lambda item: item['base']):
        if entry['type'] != 1 or not entry['attributes'] & 1:
            continue
        first = max(entry['base'], 0x100000)
        last = min(entry['end'], INITIAL_32BIT_LIMIT)
        first = (first + PAGE_SIZE - 1) // PAGE_SIZE * PAGE_SIZE
        last = last // PAGE_SIZE * PAGE_SIZE
        if first < last:
            if ranges and ranges[-1][1] == first:
                ranges[-1] = (ranges[-1][0], last)
            else:
                ranges.append((first, last))
    return ranges


def read_map(path: Path, require_complete: bool = False) -> dict:
    data = path.read_bytes()
    if len(data) < HEADER.size:
        raise ValueError('memory map header is incomplete')
    magic, version, status, count, conventional_kib, ebda_segment, size, reserved = HEADER.unpack_from(data)
    if magic != b'CMAP' or version != 1 or size != ENTRY.size or reserved:
        raise ValueError('unknown memory map format')
    if status >= len(STATUS) or count > 64 or len(data) != HEADER.size + count * size:
        raise ValueError('invalid memory map length or status')
    if require_complete and (status != 0 or count == 0):
        raise ValueError(f'firmware map is {STATUS[status]}')
    entries = []
    for index in range(count):
        base, length, kind, attributes = ENTRY.unpack_from(data, HEADER.size + index * size)
        end = base + length
        if not length or end > 1 << 64:
            raise ValueError(f'entry {index} has invalid length')
        entries.append(dict(base=base, length=length, end=end, type=kind, attributes=attributes))
    ordered = sorted(entries, key=lambda item: (item['base'], item['end']))
    overlaps = [i for i in range(1, len(ordered)) if ordered[i]['base'] < ordered[i - 1]['end']]
    usable_above_1m = sum(max(0, entry['end'] - max(entry['base'], 0x100000))
                          for entry in entries if entry['type'] == 1 and entry['attributes'] & 1)
    memory = dict(status=STATUS[status], conventional_kib=conventional_kib,
                ebda_segment=ebda_segment, entries=entries,
                usable_above_1m_bytes=usable_above_1m, overlapping_entries=overlaps)
    if status == 0 and not overlaps:
        pages = firmware_page_candidates(memory)
        memory['firmware_page_candidate_ranges'] = pages
        memory['firmware_page_candidate_count'] = sum((end - start) // PAGE_SIZE
                                                      for start, end in pages)
    return memory


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('path', type=Path)
    parser.add_argument('--require-complete', action='store_true')
    args = parser.parse_args()
    print(json.dumps(read_map(args.path, args.require_complete), indent=2))


if __name__ == '__main__':
    main()
