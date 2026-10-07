#!/usr/bin/env python3
"""Exercise the real shell help streaming instructions under Unicorn.

Run with: uv run --with unicorn python scripts/test_shell_help_stream.py
"""
from __future__ import annotations

import re
import subprocess
import tempfile
from pathlib import Path

from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_CODE, UC_HOOK_INTR
from unicorn.x86_const import (
    UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_CS, UC_X86_REG_CX,
    UC_X86_REG_DI, UC_X86_REG_DS, UC_X86_REG_DX, UC_X86_REG_EFLAGS, UC_X86_REG_ES,
    UC_X86_REG_IP, UC_X86_REG_SI, UC_X86_REG_SP,
)

ROOT = Path(__file__).resolve().parents[1]
SHELL = ROOT / "src/com/shell.asm"
HELP_DATA = ROOT / "src/com/shell_help_data.asm"
HELP_INC = ROOT / "src/com/shell_help_data.inc"
HELP_RLE = 0x01
DOS_CF = 1 << 0
MEMORY_SIZE = 0x100000
LOAD = 0x1000


def assemble(source: Path, output: Path, *defines: str) -> None:
    cmd = ["nasm", "-f", "bin", str(source), "-o", str(output)]
    for define in defines:
        cmd.extend(("-D", define))
    subprocess.run(cmd, cwd=ROOT, check=True, capture_output=True, text=True)


def routine_source() -> str:
    text = SHELL.read_text(encoding="ascii")
    start = text.index("print_dual_rle_help:\n%ifdef COMMAND_COMPAT")
    noncompat = text.index("%else\n    pushad", start) + len("%else\n")
    end = text.index("\n%endif", noncompat)
    return "print_dual_rle_help:\n" + text[noncompat:end]


def make_harness(source: Path, output: Path) -> dict[str, int]:
    code = f"""bits 16
org 0x100
HELP_RLE equ 0x01
start:
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov si, help_path
    call print_dual_rle_help
    hlt
{routine_source()}
print_dual_dollar_string:
.next:
    lodsb
    cmp al, '$'
    je .done
    call dual_putc
    jmp .next
.done:
    ret
dual_putc:
    ret
help_path db 'SYSTEM\\HELP.RLE', 0
msg_type_err db 'type: cannot open file', 13, 10, '$'
file_buf times 512 db 0
"""
    asm = output.with_suffix(".asm")
    asm.write_text(code, encoding="ascii")
    listing = output.with_suffix(".lst")
    subprocess.run(["nasm", "-f", "bin", str(asm), "-o", str(output), "-l", str(listing)],
                   cwd=ROOT, check=True, capture_output=True, text=True)
    lines = listing.read_text(encoding="ascii").splitlines()
    labels: dict[str, int] = {"start": 0x100}
    for index, line in enumerate(lines):
        if line.rstrip().endswith("dual_putc:"):
            for following in lines[index + 1:]:
                match = re.match(r"\s*\d+\s+([0-9A-F]{8})\s+[0-9A-F]{2}", following)
                if match:
                    labels["dual_putc"] = 0x100 + int(match.group(1), 16)
                    break
        for name in ("help_path", "file_buf"):
            if name in line:
                match = re.match(r"\s*\d+\s+([0-9A-F]{8})\s+[0-9A-F\[]", line)
                if match:
                    labels[name] = 0x100 + int(match.group(1), 16)
    if set(labels) != {"start", "dual_putc", "help_path", "file_buf"}:
        raise RuntimeError(f"Could not resolve harness labels from NASM listing: {labels}; "
                           f"listing begins {listing.read_text(encoding='ascii').splitlines()[:8]}")
    return labels


class DosHarness:
    def __init__(self, binary: bytes, labels: dict[str, int], data: bytes,
                 *, open_error: bool = False, read_error_at: int | None = None,
                 read_limit: int = 512, close_error: bool = False):
        self.uc = Uc(UC_ARCH_X86, UC_MODE_16)
        self.uc.mem_map(0, MEMORY_SIZE)
        self.uc.mem_write(LOAD + 0x100, binary)
        self.uc.reg_write(UC_X86_REG_CS, LOAD >> 4)
        self.uc.reg_write(UC_X86_REG_DS, LOAD >> 4)
        self.uc.reg_write(UC_X86_REG_ES, LOAD >> 4)
        self.uc.reg_write(UC_X86_REG_IP, 0x100)
        self.uc.reg_write(UC_X86_REG_SP, 0xFFFE)
        self.data = data
        self.pos = 0
        self.open_error = open_error
        self.read_error_at = read_error_at
        self.read_limit = read_limit
        self.close_error = close_error
        self.read_sizes: list[int] = []
        self.opened: list[str] = []
        self.open_modes: list[int] = []
        self.closed = 0
        self.output = bytearray()
        self.labels = labels
        self.uc.hook_add(UC_HOOK_INTR, self.on_interrupt)
        self.uc.hook_add(UC_HOOK_CODE, self.on_code)

    def on_code(self, uc: Uc, address: int, size: int, _user: object) -> None:
        physical = ((uc.reg_read(UC_X86_REG_CS) << 4) + uc.reg_read(UC_X86_REG_IP)) & 0xFFFFF
        target = LOAD + self.labels["dual_putc"]
        if physical == target:
            self.output.append(uc.reg_read(UC_X86_REG_AX) & 0xFF)

    def on_interrupt(self, uc: Uc, interrupt: int, _user: object) -> None:
        if interrupt != 0x21:
            raise AssertionError(f"Unexpected interrupt {interrupt:#x}")
        ax = uc.reg_read(UC_X86_REG_AX)
        ah, al = (ax >> 8) & 0xFF, ax & 0xFF
        flags = uc.reg_read(UC_X86_REG_EFLAGS)
        if ah == 0x3D:  # DOS open, read-only
            dx = uc.reg_read(UC_X86_REG_DX)
            ds = uc.reg_read(UC_X86_REG_DS)
            address = (ds << 4) + dx
            raw = bytearray()
            while (byte := uc.mem_read(address + len(raw), 1)[0]) != 0:
                raw.append(byte)
            self.opened.append(raw.decode("ascii"))
            self.open_modes.append(al)
            if self.open_error:
                uc.reg_write(UC_X86_REG_AX, 2)
                uc.reg_write(UC_X86_REG_EFLAGS, flags | DOS_CF)
            else:
                uc.reg_write(UC_X86_REG_AX, 5)
                uc.reg_write(UC_X86_REG_EFLAGS, flags & ~DOS_CF)
        elif ah == 0x3F:  # DOS read
            check(uc.reg_read(UC_X86_REG_BX) == 5, "read did not use the handle returned by open")
            check(uc.reg_read(UC_X86_REG_CX) == 512, "read request was not 512 bytes")
            check(((uc.reg_read(UC_X86_REG_DS) << 4) + uc.reg_read(UC_X86_REG_DX))
                  == LOAD + self.labels["file_buf"], "read did not target the shell file buffer")
            self.read_sizes.append(uc.reg_read(UC_X86_REG_CX))
            if self.read_error_at is not None and self.pos >= self.read_error_at:
                uc.reg_write(UC_X86_REG_AX, 5)
                uc.reg_write(UC_X86_REG_EFLAGS, flags | DOS_CF)
                return
            count = min(uc.reg_read(UC_X86_REG_CX), self.read_limit, len(self.data) - self.pos)
            if self.read_error_at is not None:
                count = min(count, self.read_error_at - self.pos)
            if count:
                ds = uc.reg_read(UC_X86_REG_DS)
                dx = uc.reg_read(UC_X86_REG_DX)
                uc.mem_write((ds << 4) + dx, self.data[self.pos:self.pos + count])
                self.pos += count
            uc.reg_write(UC_X86_REG_AX, count)
            uc.reg_write(UC_X86_REG_EFLAGS, flags & ~DOS_CF)
        elif ah == 0x3E:  # DOS close
            check(uc.reg_read(UC_X86_REG_BX) == 5, "close did not use the opened handle")
            self.closed += 1
            uc.reg_write(UC_X86_REG_AX, 5 if self.close_error else 0)
            uc.reg_write(UC_X86_REG_EFLAGS, flags | DOS_CF if self.close_error else flags & ~DOS_CF)
        else:
            raise AssertionError(f"Unexpected DOS function AH={ah:#x}, AL={al:#x}")

    def run(self) -> bytes:
        self.uc.emu_start(LOAD + 0x100, 0, count=2_000_000)
        return bytes(self.output)


def decode_rle(data: bytes) -> bytes:
    out = bytearray()
    i = 0
    while i < len(data):
        value = data[i]
        i += 1
        if value == ord("$"):
            return bytes(out)
        if value == HELP_RLE:
            if i + 1 >= len(data):
                raise ValueError("truncated test RLE stream")
            count, value = data[i], data[i + 1]
            i += 2
            out.extend(bytes((value,)) * count)
        else:
            out.append(value)
    raise ValueError("missing RLE terminator")


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="ciuki-help-stream-") as td:
        tmp = Path(td)
        harness = tmp / "help-harness.com"
        labels = make_harness(harness, harness)

        # The real table must cross DOS read calls cleanly and exactly match
        # the inline COMMAND.COM help data.
        help_file = tmp / "HELP.RLE"
        assemble(HELP_DATA, help_file)
        payload = help_file.read_bytes()
        expected = decode_rle(payload)
        run = DosHarness(harness.read_bytes(), labels, payload)
        actual = run.run()
        check(actual == expected, "real HELP.RLE output differs from decoded source table")
        check(run.opened == ["SYSTEM\\HELP.RLE"], "help path was not opened read-only")
        check(run.open_modes == [0], "help file was not opened read-only")
        check(run.read_sizes and all(n == 512 for n in run.read_sizes), "DOS reads were not 512-byte chunks")
        check(run.closed == 1, "normal stream did not close exactly once")

        # Put the run marker in the last byte of a 512-byte block so its count
        # and value must be fetched from the next block.
        crossing = b"A" * 511 + bytes((HELP_RLE, 4, ord("Z"))) + b"B" * 510 + b"$"
        run = DosHarness(harness.read_bytes(), labels, crossing)
        actual = run.run()
        check(actual == b"A" * 511 + b"Z" * 4 + b"B" * 510,
              "RLE token split across the 512-byte boundary was decoded incorrectly")
        check(len(run.read_sizes) >= 3 and run.closed == 1, "cross-block stream did not read and close")

        # DOS may legally return short successful reads; the decoder must keep
        # reading until the marker and close after its terminator.
        run = DosHarness(harness.read_bytes(), labels, payload, read_limit=73)
        check(run.run() == expected, "short successful reads changed help output")
        check(len(run.read_sizes) > 3 and run.closed == 1, "short reads did not complete and close")

        # Open failure prints the existing shell error and never closes an
        # unopened handle.
        run = DosHarness(harness.read_bytes(), labels, payload, open_error=True)
        check(run.run() == b"type: cannot open file\r\n", "open failure message changed")
        check(run.closed == 0, "open failure attempted to close an invalid handle")

        # Read errors and premature EOF both terminate and close the handle.
        partial = b"X" * 600 + b"$"
        run = DosHarness(harness.read_bytes(), labels, partial, read_error_at=512)
        check(run.run() == b"X" * 512, "read-error path emitted bytes after the failing read")
        check(run.closed == 1, "read-error path did not close exactly once")
        run = DosHarness(harness.read_bytes(), labels, b"unterminated")
        check(run.run() == b"unterminated", "premature EOF changed already-read output")
        check(run.closed == 1, "premature EOF path did not close exactly once")

        # A malformed zero-length run is rejected but still closes the file.
        run = DosHarness(harness.read_bytes(), labels, bytes((HELP_RLE, 0, ord("Q"), ord("$"))))
        check(run.run() == b"", "zero-count RLE token emitted output")
        check(run.closed == 1, "malformed token path did not close exactly once")

        # A failed close is ignored after the payload has already been read.
        run = DosHarness(harness.read_bytes(), labels, payload, close_error=True)
        check(run.run() == expected, "close error changed completed help output")
        check(run.closed == 1, "close error path did not attempt exactly one close")

        compat = tmp / "command.com"
        assemble(SHELL, compat, "COMMAND_COMPAT=1")
        check(compat.stat().st_size > 0, "COMMAND_COMPAT build produced an empty binary")

    print("shell help stream: real instructions passed RLE, 512-byte boundary, short-read, open/read/EOF/error-close, and COMMAND_COMPAT checks")


if __name__ == "__main__":
    main()
