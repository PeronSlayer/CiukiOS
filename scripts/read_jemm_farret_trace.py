#!/usr/bin/env python3
"""Decode the optional CVFRET01 ring from a physical-memory dump."""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path


MAGIC = b"CVFRET01"
HEADER_SIZE = 56
RECORD_SIZE = 40
RECORD_COUNT = 32


def _read_u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def parse_dump(data: bytes) -> dict[str, object]:
    """Return the first complete trace found in *data*, in chronological order."""
    offsets: list[int] = []
    pos = 0
    while True:
        pos = data.find(MAGIC, pos)
        if pos < 0:
            break
        if pos + HEADER_SIZE + RECORD_COUNT * RECORD_SIZE <= len(data):
            offsets.append(pos)
        pos += 1
    if not offsets:
        raise ValueError("no complete CVFRET01 trace found")
    if len(offsets) > 1:
        raise ValueError("multiple complete CVFRET01 traces found at " + ", ".join(hex(x) for x in offsets))

    base = offsets[0]
    next_index = _read_u32(data, base + 8)
    count = _read_u32(data, base + 12)
    sequence = _read_u32(data, base + 16)
    frozen = _read_u32(data, base + 20)
    fault_names = ("cs", "ip", "ss", "sp", "eax", "eflags", "int")
    fault = {name: _read_u32(data, base + 24 + index * 4) for index, name in enumerate(fault_names)}

    if next_index >= RECORD_COUNT:
        raise ValueError(f"invalid ring next index {next_index}")
    if count > RECORD_COUNT:
        raise ValueError(f"invalid ring record count {count}")

    oldest = (next_index - count) % RECORD_COUNT
    records = []
    kind_names = {0: "far_ret", 1: "far_call"}
    for age in range(count):
        index = (oldest + age) % RECORD_COUNT
        offset = base + HEADER_SIZE + index * RECORD_SIZE
        values = struct.unpack_from("<10I", data, offset)
        ss_sp = values[2]
        target = values[6]
        kind = values[1]
        records.append(
            {
                "ring_index": index,
                "sequence": values[0],
                "kind": kind,
                "operation": kind_names.get(kind, "unknown"),
                "ss": ss_sp >> 16,
                "sp": ss_sp & 0xFFFF,
                "cs": values[3],
                "ip": values[4],
                "eax": values[5],
                "target_cs": target >> 16,
                "target_ip": target & 0xFFFF,
                "caller_eip": values[7],
                "int": values[8],
                "eflags": values[9],
            }
        )

    return {
        "magic": MAGIC.decode("ascii"),
        "physical_offset": base,
        "next_index": next_index,
        "count": count,
        "sequence": sequence,
        "frozen": frozen,
        "fault": fault,
        "records_oldest_first": records,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path, help="physical-memory dump containing CVFRET01")
    args = parser.parse_args()
    try:
        result = parse_dump(args.dump.read_bytes())
    except (OSError, ValueError) as exc:
        print(f"{args.dump}: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
