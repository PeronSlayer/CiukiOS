#!/usr/bin/env python3
"""Reject silent or constant PCM WAV captures produced by QEMU."""

from __future__ import annotations

import argparse
import array
import math
import pathlib
import struct
import sys


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate that a PCM WAV capture contains a changing signal."
    )
    parser.add_argument("wav", type=pathlib.Path)
    parser.add_argument("--label", default="audio-wav")
    parser.add_argument("--min-bytes", type=int, default=100_000)
    parser.add_argument("--min-ac-rms", type=float, default=100.0)
    parser.add_argument("--min-unique", type=int, default=32)
    parser.add_argument("--min-changes", type=int, default=500)
    parser.add_argument(
        "--start-byte",
        type=int,
        default=0,
        help="ignore this many bytes at the start of the PCM payload",
    )
    parser.add_argument(
        "--end-byte",
        type=int,
        help="stop at this byte offset in the PCM payload",
    )
    return parser.parse_args()


def pcm_payload(raw: bytes) -> tuple[bytes, int]:
    if len(raw) < 44 or raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise ValueError("capture has no RIFF/WAVE signature")

    offset = 12
    bits_per_sample = 0
    while offset + 8 <= len(raw):
        chunk_id = raw[offset : offset + 4]
        chunk_size = struct.unpack_from("<I", raw, offset + 4)[0]
        data_start = offset + 8

        if chunk_id == b"fmt ":
            if chunk_size < 16 or data_start + 16 > len(raw):
                raise ValueError("capture has a truncated fmt chunk")
            audio_format, _, _, _, _, bits_per_sample = struct.unpack_from(
                "<HHIIHH", raw, data_start
            )
            if audio_format != 1:
                raise ValueError(f"unsupported WAV format {audio_format}; expected PCM")
        elif chunk_id == b"data":
            if not bits_per_sample:
                raise ValueError("capture data chunk precedes its fmt chunk")
            # QEMU can leave the data length at zero when HMP closes the VM.
            # In that case the complete payload still follows the data header.
            data_end = len(raw) if chunk_size == 0 else min(len(raw), data_start + chunk_size)
            return raw[data_start:data_end], bits_per_sample

        offset = data_start + chunk_size + (chunk_size & 1)

    raise ValueError("capture has no PCM data chunk")


def main() -> int:
    args = parse_args()
    try:
        raw = args.wav.read_bytes()
        payload, bits_per_sample = pcm_payload(raw)
    except (OSError, ValueError) as exc:
        print(f"[{args.label}] FAIL {exc}", file=sys.stderr)
        return 1

    end_byte = len(payload) if args.end_byte is None else args.end_byte
    if args.start_byte < 0 or end_byte < args.start_byte or end_byte > len(payload):
        print(
            f"[{args.label}] FAIL invalid PCM byte window "
            f"{args.start_byte}:{end_byte} for {len(payload)} bytes",
            file=sys.stderr,
        )
        return 1
    payload = payload[args.start_byte:end_byte]

    if len(payload) < args.min_bytes:
        print(
            f"[{args.label}] FAIL audio capture is too small: {len(payload)} bytes",
            file=sys.stderr,
        )
        return 1
    if bits_per_sample != 16:
        print(
            f"[{args.label}] FAIL unsupported PCM depth {bits_per_sample}; expected 16-bit",
            file=sys.stderr,
        )
        return 1

    samples = array.array("h")
    samples.frombytes(payload[: len(payload) & ~1])
    if sys.byteorder != "little":
        samples.byteswap()
    if not samples:
        print(f"[{args.label}] FAIL capture contains no PCM samples", file=sys.stderr)
        return 1

    mean = sum(samples) / len(samples)
    ac_rms = math.sqrt(sum((sample - mean) ** 2 for sample in samples) / len(samples))
    unique = len(set(samples))
    changes = sum(left != right for left, right in zip(samples, samples[1:]))
    peak = max(abs(sample) for sample in samples)

    if (
        ac_rms < args.min_ac_rms
        or unique < args.min_unique
        or changes < args.min_changes
    ):
        print(
            f"[{args.label}] FAIL silent/constant audio: bytes={len(payload)} "
            f"ac_rms={ac_rms:.2f} peak={peak} unique={unique} changes={changes}",
            file=sys.stderr,
        )
        return 1

    print(
        f"[{args.label}] PASS waveform bytes={len(payload)} ac_rms={ac_rms:.2f} "
        f"peak={peak} unique={unique} changes={changes}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
