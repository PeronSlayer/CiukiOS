"""Validate raw stub records; never repair or normalize marker text."""
# SPDX-License-Identifier: GPL-2.0-only
import argparse
from pathlib import Path
import re


def check(data, expected_request=None, text=False, crc_error=False):
    if crc_error:
        if data != b"C":
            raise ValueError(f"expected the MBR CRC error C, got {data!r}")
        return []
    records = []
    for line in data.splitlines():
        if not line.startswith(b"CIUKI_TEST "):
            continue
        if len(line) > 240:
            raise ValueError("oversized record")
        fields = {}
        for item in line.decode("ascii").split()[1:]:
            key, value = item.split("=", 1)
            if key in fields or not value:
                raise ValueError("duplicate key or missing value")
            fields[key] = value
        if fields.get("v") != "1" or fields.get("probe") != "loader":
            raise ValueError("wrong version/probe")
        if not re.fullmatch(r"[0-9a-fA-F]{8}", fields.get("run", "")):
            raise ValueError("malformed run ID")
        if not re.fullmatch(r"[0-9]{6}", fields.get("seq", "")):
            raise ValueError("malformed sequence")
        if int(fields["seq"]) != len(records)+1:
            raise ValueError("missing, duplicate or reordered record")
        records.append(fields)
    if len(records) != 6 or [r["event"] for r in records] != [
            "BEGIN", "DATA", "DATA", "DATA", "DATA", "END"]:
        raise ValueError("missing or unexpected events")
    if records[-1].get("status") != "PASS":
        raise ValueError("stub did not pass")
    flags = int(records[1]["flags"], 16)
    count = int(records[1]["e820_count"])
    policy = int(records[1]["input_policy"])
    if not 1 <= count <= 128 or flags & ~0x7FF or not 1 <= (flags>>8)&7 <= 4:
        raise ValueError("invalid handoff flags/map count")
    if not flags & (1<<5):
        raise ValueError("SMBIOS QEMU was not discovered")
    request = b"" if expected_request is None else expected_request.encode("ascii")
    expected_id = "00000000" if not request else expected_request.split(" run=")[1][:8]
    if any(r["run"] != expected_id for r in records):
        raise ValueError("run ID differs from selection")
    if int(records[3]["request_len"]) != len(request):
        raise ValueError("request length differs")
    if records[3]["request_hex"] != (request.hex() if request else "-"):
        raise ValueError("request bytes differ")
    forced = bool(request and request.endswith(b" platform=e500"))
    if bool(flags & (1<<6)) != bool(request) or bool(flags & (1<<7)) != forced:
        raise ValueError("request/forced flags differ")
    if policy != int(forced):
        raise ValueError("unexpected QEMU input policy")
    if text:
        if records[2]["video"] != "text" or not flags & (1<<4):
            raise ValueError("missing text fallback")
    elif (records[2]["video"] != "lfb" or flags & (1<<4) or
          tuple(int(records[2][f]) for f in ("width", "height", "bpp")) !=
          (1024, 768, 32) or int(records[2]["pitch"]) < 4096):
        raise ValueError("unexpected standard VGA mode")
    if records[4]["entry_phys"] != "00100000":
        raise ValueError("ELF entry translation failed")
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("serial", type=Path)
    parser.add_argument("--request")
    parser.add_argument("--text", action="store_true")
    parser.add_argument("--crc-error", action="store_true")
    args = parser.parse_args()
    check(args.serial.read_bytes(), args.request, args.text, args.crc_error)
    print("PASS: raw loader-stub serial evidence")


if __name__ == "__main__":
    main()
