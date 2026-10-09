#!/usr/bin/env python3
"""Decode the live DOS/XMS/VCPI resource snapshot from SYSTEM/DOSMEM.BIN."""

import argparse
import json
import struct
import sys
from pathlib import Path


RECORD = struct.Struct("<4sBBBBHBBBBIIIIH")
MAGIC = b"CDMR"
VERSION = 1
SIZE = 32

FLAG_XMS3 = 0x01
FLAG_XMS_QUERY_OK = 0x02
FLAG_VCPI = 0x04
FLAG_VCPI_QUERY_OK = 0x08
FLAG_FIRMWARE_CEILING = 0x10
FLAG_COMPLETE = 0x80

ERROR_XMS_MISSING_OR_OLD = 0x01
ERROR_XMS_QUERY = 0x02
ERROR_VCPI_ABSENT = 0x04
ERROR_VCPI_QUERY = 0x08
ERROR_FIRMWARE_QUERY = 0x10


def parse_record(data: bytes) -> dict[str, object]:
    if len(data) != SIZE:
        raise ValueError(f"DOSMEM record must be exactly {SIZE} bytes, got {len(data)}")
    (magic, version, size, flags, errors, xms_version, xms_status,
     vcpi_presence_status, vcpi_query_status, reserved, xms_largest_kib,
     xms_total_kib, vcpi_free_pages, firmware_usable_kib, reserved_word) = RECORD.unpack(data)

    if magic != MAGIC or version != VERSION or size != SIZE:
        raise ValueError("unknown DOSMEM signature, version, or record size")
    if flags & 0x60 or errors & 0xE0 or reserved or reserved_word:
        raise ValueError("DOSMEM reserved bits or fields are nonzero")
    if not flags & FLAG_COMPLETE:
        raise ValueError("DOSMEM record is incomplete")
    if not flags & FLAG_FIRMWARE_CEILING and firmware_usable_kib:
        raise ValueError("DOSMEM firmware ceiling value is set without its validity flag")
    if flags & FLAG_XMS_QUERY_OK and not flags & FLAG_XMS3:
        raise ValueError("DOSMEM marks an XMS query successful without XMS 3.0")
    if flags & FLAG_VCPI_QUERY_OK and not flags & FLAG_VCPI:
        raise ValueError("DOSMEM marks a VCPI query successful without VCPI")

    return {
        "signature": magic.decode("ascii"),
        "version": version,
        "record_size": size,
        "complete": bool(flags & FLAG_COMPLETE),
        "result_flags": {
            "xms3_present": bool(flags & FLAG_XMS3),
            "xms_query_ok": bool(flags & FLAG_XMS_QUERY_OK),
            "vcpi_present": bool(flags & FLAG_VCPI),
            "vcpi_query_ok": bool(flags & FLAG_VCPI_QUERY_OK),
            "firmware_ceiling_present": bool(flags & FLAG_FIRMWARE_CEILING),
        },
        "error_flags": {
            "xms_missing_or_old": bool(errors & ERROR_XMS_MISSING_OR_OLD),
            "xms_query_error": bool(errors & ERROR_XMS_QUERY),
            "vcpi_absent": bool(errors & ERROR_VCPI_ABSENT),
            "vcpi_query_error": bool(errors & ERROR_VCPI_QUERY),
            "firmware_query_error": bool(errors & ERROR_FIRMWARE_QUERY),
        },
        "xms": {
            "version_bcd": f"{xms_version:04X}",
            "query_status": xms_status,
            "largest_free_kib": xms_largest_kib,
            "total_free_kib": xms_total_kib,
        },
        "vcpi": {
            "presence_status": vcpi_presence_status,
            "free_page_query_status": vcpi_query_status,
            "free_4k_pages": vcpi_free_pages,
            "free_kib": vcpi_free_pages * 4 if flags & FLAG_VCPI_QUERY_OK else None,
        },
        "firmware_ceiling": {
            "usable_kib_above_1m": firmware_usable_kib if flags & FLAG_FIRMWARE_CEILING else None,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path, help="captured SYSTEM/DOSMEM.BIN")
    args = parser.parse_args()
    try:
        report = parse_record(args.path.read_bytes())
    except (OSError, ValueError) as exc:
        print(f"{args.path}: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
