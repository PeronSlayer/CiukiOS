#!/usr/bin/env python3
"""Validate a flat CiukiDOS kernel image and report its declared memory map."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any


ABI_INCLUDE = Path(__file__).resolve().parents[1] / "src/runtime/ciukidos_abi.inc"
DEFAULT_MAX_BYTES = 0xF000
KERNEL_SEGMENT = 0x0300
LOAD_SEGMENT = 0x0900
EXPECTED_SERVICES = 13
HEADER_SIZE = 26
TABLE_OFFSET = 29
TABLE_HEADER_SIZE = 10
DESCRIPTOR_SIZE = 8
LAYOUT_MAGIC = b"CMLY"

_LAYOUT_SPEC = {
    1: ("kernel", None, None, 1),
    2: ("sysvars", None, 0x70, 1),
    3: ("exec", None, 0x100, 1),
    4: ("stage2", None, 0x20, 2),
    5: ("meta", None, 0x100, 1),
    6: ("fat", None, 0x100, 1),
    7: ("io", None, 0x30, 1),
    8: ("env", None, 0x50, 1),
    9: ("com_arena", None, 0, 4),
}


def _u16(data: bytes, offset: int, what: str) -> int:
    if offset < 0 or offset + 2 > len(data):
        raise ValueError(f"truncated {what}")
    return int.from_bytes(data[offset : offset + 2], "little")


def _need(data: bytes, offset: int, length: int, what: str) -> None:
    if offset < 0 or length < 0 or offset + length > len(data):
        raise ValueError(f"truncated {what}")


def _paragraphs(size: int) -> int:
    return (size + 15) // 16


def inspect_kernel(data: bytes, max_bytes: int = DEFAULT_MAX_BYTES) -> dict[str, Any]:
    """Validate ABI2, RTSV and CMLY data, returning a structured memory map.

    No image code is executed. All offsets and records are read as little-endian
    metadata from the flat image.
    """
    if len(data) > max_bytes:
        raise ValueError(f"kernel image exceeds maximum size ({len(data)} > {max_bytes})")
    _need(data, 0, HEADER_SIZE, "kernel header")
    if data[2:10] != b"CIUKIDOS":
        raise ValueError("bad CIUKIDOS magic")

    header = {
        "header_size": _u16(data, 10, "kernel header"),
        "abi_version": _u16(data, 12, "kernel header"),
        "service_count": _u16(data, 14, "kernel header"),
        "descriptor_size": _u16(data, 16, "kernel header"),
        "capabilities": _u16(data, 18, "kernel header"),
        "declared_size": _u16(data, 20, "kernel header"),
        "table_offset": _u16(data, 22, "kernel header"),
        "load_segment": _u16(data, 24, "kernel header"),
    }
    if header["header_size"] != HEADER_SIZE:
        raise ValueError("unsupported kernel header size")
    if header["abi_version"] != 2:
        raise ValueError("unsupported kernel ABI version")
    if header["service_count"] != EXPECTED_SERVICES:
        raise ValueError("kernel service count must be 13")
    if header["descriptor_size"] != DESCRIPTOR_SIZE:
        raise ValueError("unsupported kernel service descriptor size")
    if header["declared_size"] != len(data):
        raise ValueError("declared kernel size does not match image length")
    if header["table_offset"] != TABLE_OFFSET:
        raise ValueError("unexpected RTSV table offset")
    if header["load_segment"] != LOAD_SEGMENT:
        raise ValueError("unexpected kernel load segment")
    if header["capabilities"] != 0x003F:
        raise ValueError("unexpected kernel capability mask")

    table = header["table_offset"]
    _need(data, table, TABLE_HEADER_SIZE, "RTSV table header")
    if data[table : table + 4] != b"RTSV":
        raise ValueError("bad RTSV magic")
    table_abi = _u16(data, table + 4, "RTSV table")
    table_count = _u16(data, table + 6, "RTSV table")
    table_desc_size = _u16(data, table + 8, "RTSV table")
    if (table_abi, table_count, table_desc_size) != (2, EXPECTED_SERVICES, DESCRIPTOR_SIZE):
        raise ValueError("RTSV header disagrees with kernel ABI header")
    table_end = table + TABLE_HEADER_SIZE + table_count * table_desc_size
    _need(data, table, table_end - table, "RTSV service table")
    services = []
    for index in range(table_count):
        off = table + TABLE_HEADER_SIZE + index * table_desc_size
        service_id = _u16(data, off, "RTSV descriptor")
        version = _u16(data, off + 2, "RTSV descriptor")
        handler = _u16(data, off + 4, "RTSV descriptor")
        reserved = _u16(data, off + 6, "RTSV descriptor")
        if service_id != index + 1:
            raise ValueError("RTSV service IDs must be ordered 1 through 13")
        if version != 1:
            raise ValueError(f"unsupported RTSV service version for ID {service_id}")
        if handler < table_end or handler >= len(data):
            raise ValueError(f"RTSV handler offset out of bounds for ID {service_id}")
        if reserved != 0:
            raise ValueError(f"nonzero RTSV reserved field for ID {service_id}")
        services.append({"id": service_id, "version": version, "handler_offset": handler})

    positions = []
    cursor = 0
    while True:
        found = data.find(LAYOUT_MAGIC, cursor)
        if found < 0:
            break
        positions.append(found)
        cursor = found + 1
    if len(positions) != 1:
        raise ValueError("kernel image must contain exactly one CMLY descriptor")
    layout_offset = positions[0]
    _need(data, layout_offset, 12, "CMLY header")
    if layout_offset < table_end:
        raise ValueError("CMLY descriptor overlaps RTSV table")
    version = _u16(data, layout_offset + 4, "CMLY header")
    layout_size = _u16(data, layout_offset + 6, "CMLY header")
    count = _u16(data, layout_offset + 8, "CMLY header")
    record_size = _u16(data, layout_offset + 10, "CMLY header")
    if version != 1:
        raise ValueError("unsupported CMLY version")
    if count != len(_LAYOUT_SPEC):
        raise ValueError("CMLY must contain exactly nine memory records")
    if record_size != 8:
        raise ValueError("CMLY record size must be 8")
    expected_layout_size = 12 + count * record_size
    if layout_size != expected_layout_size:
        raise ValueError("CMLY size does not match record count")
    _need(data, layout_offset, layout_size, "CMLY descriptor")

    regions: dict[str, dict[str, int]] = {}
    ordered = []
    for index in range(count):
        off = layout_offset + 12 + index * record_size
        kind, start, paragraphs, flags = (
            _u16(data, off + delta, "CMLY memory record") for delta in (0, 2, 4, 6)
        )
        if kind not in _LAYOUT_SPEC:
            raise ValueError(f"unknown CMLY region kind {kind}")
        name, _, required_paragraphs, required_flags = _LAYOUT_SPEC[kind]
        if kind in regions:
            raise ValueError(f"duplicate CMLY region kind {kind}")
        if flags != required_flags:
            raise ValueError(f"unexpected CMLY flags for {name}")
        if required_paragraphs is not None and paragraphs != required_paragraphs:
            raise ValueError(f"unexpected paragraph count for {name}")
        if start + paragraphs > 0xA000:
            raise ValueError(f"CMLY region {name} exceeds conventional memory")
        region = {"kind": kind, "start_segment": start, "paragraphs": paragraphs, "flags": flags}
        regions[name] = region
        ordered.append((name, start, paragraphs, flags))
    if set(r[0] for r in ordered) != {v[0] for v in _LAYOUT_SPEC.values()}:
        raise ValueError("CMLY region kinds are incomplete")

    image_paragraphs = _paragraphs(len(data))
    kernel = regions["kernel"]
    sysvars = regions["sysvars"]
    exec_region = regions["exec"]
    stage2 = regions["stage2"]
    meta = regions["meta"]
    fat = regions["fat"]
    io = regions["io"]
    env = regions["env"]
    com = regions["com_arena"]
    if kernel["start_segment"] != KERNEL_SEGMENT:
        raise ValueError("kernel CMLY region must start at segment 0x0300")
    if kernel["paragraphs"] != image_paragraphs:
        raise ValueError("kernel CMLY paragraph count does not match actual image size")
    kernel_end = KERNEL_SEGMENT + image_paragraphs
    if sysvars["start_segment"] != kernel_end:
        raise ValueError("SYSVARS must immediately follow the actual kernel paragraphs")
    if exec_region["start_segment"] != sysvars["start_segment"] + sysvars["paragraphs"]:
        raise ValueError("EXEC must immediately follow SYSVARS")
    if stage2["start_segment"] != exec_region["start_segment"] + 0x80:
        raise ValueError("stage2 must occupy the declared temporary EXEC subrange")
    if stage2["start_segment"] + stage2["paragraphs"] > exec_region["start_segment"] + exec_region["paragraphs"]:
        raise ValueError("stage2 temporary region must be contained by EXEC")
    if meta["start_segment"] != exec_region["start_segment"] + 0x100:
        raise ValueError("META must immediately follow EXEC")
    if fat["start_segment"] != meta["start_segment"] + 0x100:
        raise ValueError("FAT must immediately follow META")
    if io["start_segment"] != fat["start_segment"] + 0x100:
        raise ValueError("IO must immediately follow FAT")
    if env["start_segment"] != io["start_segment"] + 0x30:
        raise ValueError("ENV must immediately follow IO")
    if com["start_segment"] != env["start_segment"] + 0x50:
        raise ValueError("COM arena marker must immediately follow ENV")

    # Stage2 overlaps EXEC only during a separate boot lifetime. All other
    # positive-size regions must be disjoint.
    positive = [(n, s, s + p) for n, s, p, _ in ordered if p]
    for i, (name_a, start_a, end_a) in enumerate(positive):
        for name_b, start_b, end_b in positive[i + 1 :]:
            if {name_a, name_b} == {"exec", "stage2"}:
                continue
            if start_a < end_b and start_b < end_a:
                raise ValueError(f"overlapping CMLY regions: {name_a} and {name_b}")

    return {
        "components": {
            "image_bytes": len(data),
            "image_paragraphs": image_paragraphs,
            "rtsv_table_offset": table,
            "rtsv_table_end": table_end,
            "cmly_offset": layout_offset,
            "cmly_bytes": layout_size,
        },
        "header": header,
        "services": services,
        "layout": {"version": version, "record_count": count, "record_size": record_size},
        "regions": regions,
    }


def read_build_parameters(path: Path = ABI_INCLUDE) -> dict[str, int]:
    """Read the four simple numeric defines consumed by build_full.sh."""
    try:
        source = path.read_text(encoding="ascii")
    except OSError as exc:
        raise ValueError(f"cannot read ABI include {path}: {exc}") from exc
    wanted = ("ABI_VERSION", "SERVICE_COUNT", "KERNEL_MAX_BYTES", "RUNTIME_SEG")
    found: dict[str, int] = {}
    pattern = re.compile(r"^\s*%define\s+(?:CIUKIDOS_)?([A-Z0-9_]+)\s+([0-9A-Fa-fxX]+)\s*(?:;.*)?$")
    for line in source.splitlines():
        match = pattern.match(line)
        if not match:
            continue
        key, value = match.groups()
        if key not in wanted:
            continue
        try:
            found[key] = int(value, 0)
        except ValueError:
            raise ValueError(f"non-numeric ABI define {key}") from None
    missing = [key for key in wanted if key not in found]
    if missing:
        raise ValueError(f"missing ABI defines: {', '.join(missing)}")
    if found["ABI_VERSION"] != 2 or found["SERVICE_COUNT"] != EXPECTED_SERVICES:
        raise ValueError("ABI include must declare ABI_VERSION=2 and SERVICE_COUNT=13")
    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, help="flat kernel image to validate")
    parser.add_argument("--json", action="store_true", help="emit inspected image as JSON")
    parser.add_argument("--build-parameters", action="store_true", help="print ABI build values")
    parser.add_argument("--abi-include", type=Path, default=ABI_INCLUDE)
    args = parser.parse_args(argv)
    try:
        if args.build_parameters:
            params = read_build_parameters(args.abi_include)
            print(" ".join(str(params[k]) for k in ("ABI_VERSION", "SERVICE_COUNT", "KERNEL_MAX_BYTES", "RUNTIME_SEG")))
            return 0
        if not args.kernel:
            parser.error("--kernel is required unless --build-parameters is used")
        result = inspect_kernel(args.kernel.read_bytes())
        if args.json:
            print(json.dumps(result, indent=2, sort_keys=True))
        else:
            print(f"valid CiukiDOS ABI2 image: {result['components']['image_bytes']} bytes")
            for name, region in result["regions"].items():
                print(f"{name}: segment={region['start_segment']:04X} paragraphs={region['paragraphs']:04X} flags={region['flags']:04X}")
        return 0
    except (OSError, ValueError) as exc:
        print(f"ciukidos_image: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
