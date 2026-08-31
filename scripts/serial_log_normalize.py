#!/usr/bin/env python3
"""Normalize duplicated CiukiOS console characters in QEMU serial logs.

Some console paths are mirrored to the same emulated serial port.  Depending
on the active path, a printable character can consequently appear once, twice,
or three times.  A blind character squeeze would corrupt legitimate text such
as ``SHELL`` and ``APPS``.  This tool instead detects strong duplicated
run-length streaks within each logical line and divides only those streaks by
their factor.  Text without strong duplication evidence is left untouched.

Raw logs remain the source evidence; test harnesses consume this normalized
view for deterministic marker matching.
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path


MIN_STREAK_RUNS = 4


def _runs(data: bytes) -> list[tuple[int, int]]:
    if not data:
        return []

    result: list[tuple[int, int]] = []
    current = data[0]
    count = 1
    for value in data[1:]:
        if value == current:
            count += 1
            continue
        result.append((current, count))
        current = value
        count = 1
    result.append((current, count))
    return result


def _normalize_line(line: bytes) -> bytes:
    runs = _runs(line)
    normalized_counts = [count for _, count in runs]
    claimed = [False] * len(runs)
    printable_indexes = [
        index
        for index, (value, _) in enumerate(runs)
        if value == 9 or 0x20 <= value <= 0x7E
    ]

    # Test three first: a genuine x3 stream can contain x6 runs for doubled
    # source letters, and those x6 runs are also divisible by two.  Normalize
    # only strong contiguous streaks, so clean prefixes and legitimate words
    # such as BOOKKEEPER remain byte-for-byte intact.
    for factor in (3, 2):
        index = 0
        while index < len(runs):
            value, count = runs[index]
            eligible = (
                not claimed[index]
                and (value == 9 or 0x20 <= value <= 0x7E)
                and count >= factor
                and count % factor == 0
            )
            if not eligible:
                index += 1
                continue

            end = index + 1
            while end < len(runs):
                next_value, next_count = runs[end]
                if (
                    claimed[end]
                    or not (next_value == 9 or 0x20 <= next_value <= 0x7E)
                    or next_count < factor
                    or next_count % factor != 0
                ):
                    break
                end += 1

            streak_size = end - index
            covers_all_printable = printable_indexes == list(range(index, end))
            if streak_size >= MIN_STREAK_RUNS or (
                streak_size >= 3 and covers_all_printable
            ):
                for run_index in range(index, end):
                    claimed[run_index] = True
                    normalized_counts[run_index] //= factor
            index = end

    normalized = bytearray()
    for (value, _), count in zip(runs, normalized_counts):
        normalized.extend(bytes((value,)) * count)
    return bytes(normalized)


def normalize(data: bytes) -> bytes:
    output = bytearray()
    line = bytearray()

    for value in data:
        if value in (10, 13):
            output.extend(_normalize_line(bytes(line)))
            line.clear()
            # CR/LF bytes themselves may be duplicated.  One separator is
            # sufficient; alternating CR/LF pairs and blank lines survive.
            if not output or output[-1] != value:
                output.append(value)
            continue
        line.append(value)

    output.extend(_normalize_line(bytes(line)))
    return bytes(output)


def _expand_printable(data: bytes, factor: int) -> bytes:
    expanded = bytearray()
    for value in data:
        if value in (10, 13):
            expanded.extend(bytes((value,)) * factor)
        else:
            expanded.extend(bytes((value,)) * factor)
    return bytes(expanded)


def self_test() -> None:
    canonical = b"CiukiOS SHELL C:\\APPS>\r\nSHELL keeps APPS and BOOKKEEPER\r\n"
    for factor in (1, 2, 3):
        actual = normalize(_expand_printable(canonical, factor))
        if actual != canonical:
            raise AssertionError(
                f"x{factor} normalization mismatch: {actual!r} != {canonical!r}"
            )

    mixed = b"\x1b[0m" + _expand_printable(b"CiukiOS C:\\APPS>", 3)
    expected = b"\x1b[0mCiukiOS C:\\APPS>"
    actual = normalize(mixed)
    if actual != expected:
        raise AssertionError(f"mixed-stream normalization mismatch: {actual!r}")

    taxonomy = (
        b"[STAGE1-SERIAL] READY\r\n"
        b"CiukiOS SHELL C:\\APPS\\WOLF3D>\r\n"
        b"CiukiOS C:\\APPS\\WOLF3D>\r\n"
        b"[MZ] run WOLF3D.EXE\r\n"
    )
    for factor in (1, 2, 3):
        actual = normalize(_expand_printable(taxonomy, factor))
        if actual != taxonomy:
            raise AssertionError(
                f"taxonomy x{factor} mismatch: {actual!r} != {taxonomy!r}"
            )

    print("[serial-normalize] PASS")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", nargs="?", type=Path, help="raw serial log, or stdin when omitted")
    parser.add_argument("--offset", type=int, default=0, help="zero-based byte offset")
    parser.add_argument("--self-test", action="store_true", help="run deterministic unit cases")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.self_test:
        self_test()
        return 0
    if args.offset < 0:
        raise SystemExit("--offset must be non-negative")

    if args.log is None:
        data = sys.stdin.buffer.read()
    else:
        with args.log.open("rb") as stream:
            stream.seek(args.offset)
            data = stream.read()

    try:
        sys.stdout.buffer.write(normalize(data))
    except BrokenPipeError:
        # grep -q may close its input after the first match.
        os._exit(0)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
