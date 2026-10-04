#!/usr/bin/env python3
"""Build the deterministic freestanding CN32 sample image."""
from __future__ import annotations

import argparse
import shutil
import struct
import subprocess
import tempfile
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/native/sample.asm"
DEFAULT_OUTPUT = ROOT / "build/native/NATIVE32.N32"
HEADER = struct.Struct("<4sHHIIIIIII")
MAGIC = b"CN32"
VERSION = 1
HEADER_SIZE = HEADER.size
FLAGS_FLAT_IA32 = 1
U32_MAX = (1 << 32) - 1
MIN_STACK_BYTES = 4096
ENTRY_OFFSET = 0


def assemble(nasm: str, source: Path, output: Path, define: str) -> bytes:
    command = [nasm, "-f", "bin", "-D", define, "-o", str(output), str(source)]
    subprocess.run(command, check=True, cwd=ROOT)
    return output.read_bytes()


def build(output: Path, stack_bytes: int, nasm: str) -> dict[str, int | str]:
    if not SOURCE.is_file():
        raise SystemExit(f"Missing payload source: {SOURCE}")
    if not MIN_STACK_BYTES <= stack_bytes <= U32_MAX:
        raise SystemExit(
            f"stack size must be between {MIN_STACK_BYTES} and {U32_MAX} bytes"
        )
    if not shutil.which(nasm):
        raise SystemExit(f"NASM executable not found: {nasm}")

    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ciuki-native-") as temp_dir:
        temp = Path(temp_dir)
        code = assemble(nasm, SOURCE, temp / "code.bin", "NATIVE_CODE_ONLY=1")
        data = assemble(nasm, SOURCE, temp / "data.bin", "NATIVE_DATA_ONLY=1")

    if not code:
        raise SystemExit("native image must contain code")
    if len(code) > U32_MAX:
        raise SystemExit(f"code exceeds the 32-bit image format: {len(code)}")
    if len(data) > U32_MAX:
        raise SystemExit(f"data exceeds the 32-bit image format: {len(data)}")
    payload = code + data
    if len(payload) > U32_MAX:
        raise SystemExit(f"payload exceeds the 32-bit image format: {len(payload)}")
    if ENTRY_OFFSET >= len(code):
        raise SystemExit("entry offset is outside the code segment")

    checksum = zlib.crc32(payload) & 0xFFFFFFFF
    header = HEADER.pack(
        MAGIC,
        VERSION,
        HEADER_SIZE,
        FLAGS_FLAT_IA32,
        ENTRY_OFFSET,
        len(code),
        len(data),
        stack_bytes,
        len(payload),
        checksum,
    )
    image = header + payload
    output.write_bytes(image)
    return {
        "output": str(output),
        "header_bytes": HEADER_SIZE,
        "entry_offset": ENTRY_OFFSET,
        "code_bytes": len(code),
        "data_bytes": len(data),
        "stack_bytes": stack_bytes,
        "payload_bytes": len(payload),
        "crc32": f"{checksum:08x}",
        "image_bytes": len(image),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--stack-bytes", type=int, default=4096)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    result = build(args.output.resolve(), args.stack_bytes, args.nasm)
    for key, value in result.items():
        print(f"{key}: {value}")


if __name__ == "__main__":
    main()
