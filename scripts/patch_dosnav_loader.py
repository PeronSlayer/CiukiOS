#!/usr/bin/env python3
"""Patch the DOSNavigator COM launcher so EXIT terminates the launcher.

The stock loader sends every command-line request to COMMAND.COM and then
restarts DN.PRG.  Consequently `exit` only exits that temporary command
processor.  This bounded patch recognizes an exact four-character EXIT
request and follows the launcher's existing termination path instead.
"""

from __future__ import annotations

import argparse
from pathlib import Path


ENTRY_OFFSET = 0x10B
EXIT_OFFSET = 0x166
INITIAL_CONTINUE_OFFSET = 0x110
AFTER_COMMAND_OFFSET = 0x14F
AFTER_COMMAND_CONTINUE_OFFSET = 0x156
RESIZE_OFFSET = 0x6C5
RESIDENT_BYTES = 0x1200
COMMAND_LENGTH_ADDRESS = 0x0295
COMMAND_TEXT_ADDRESS = 0x0296
COMMAND_STATUS_ADDRESS = 0x01C7


class PatchBuilder:
    def __init__(self, origin: int) -> None:
        self.origin = origin
        self.code = bytearray()
        self.labels: dict[str, int] = {}
        self.short_fixups: list[tuple[int, str]] = []

    def emit(self, *values: int) -> None:
        self.code.extend(values)

    def label(self, name: str) -> None:
        self.labels[name] = self.origin + len(self.code)

    def short_jump(self, opcode: int, label: str) -> None:
        self.emit(opcode, 0)
        self.short_fixups.append((len(self.code) - 1, label))

    def near_jump(self, target: int) -> None:
        instruction = self.origin + len(self.code)
        displacement = target - (instruction + 3)
        self.emit(0xE9, displacement & 0xFF, (displacement >> 8) & 0xFF)

    def finish(self) -> bytes:
        for displacement_index, label in self.short_fixups:
            next_instruction = self.origin + displacement_index + 1
            displacement = self.labels[label] - next_instruction
            if not -128 <= displacement <= 127:
                raise ValueError(f"short jump to {label} is out of range")
            self.code[displacement_index] = displacement & 0xFF
        return bytes(self.code)


def make_exit_patch(origin: int) -> bytes:
    patch = PatchBuilder(origin)
    # DN.PRG is returning to the resident COM loader here, but DOS does not
    # guarantee that DS still addresses the loader.  The command buffer is a
    # CS-relative resident object, so every read needs an explicit CS override.
    # Command tails produced by different DN.PRG paths contain either EXIT or
    # a DOS-style leading separator (" EXIT").  Accept both exact shapes, but
    # reject longer arguments instead of treating prefixes such as EXITNOW as
    # a request to terminate the resident loader.
    patch.emit(0xBE, COMMAND_TEXT_ADDRESS & 0xFF,
               COMMAND_TEXT_ADDRESS >> 8)           # mov si,0296h
    patch.emit(0x2E, 0xA0, COMMAND_LENGTH_ADDRESS & 0xFF,
               COMMAND_LENGTH_ADDRESS >> 8)         # mov al,[cs:0295h]
    patch.emit(0x3C, 0x04)                          # cmp al,4
    patch.short_jump(0x74, "check_exit")
    patch.emit(0x3C, 0x05)                          # cmp al,5
    patch.short_jump(0x75, "continue")
    patch.emit(0x2E, 0x80, 0x3C, 0x20)              # cmp byte [cs:si],' '
    patch.short_jump(0x75, "continue")
    patch.emit(0x46)                                # inc si (skip separator)

    patch.label("check_exit")
    patch.emit(0xFC)                                # cld
    patch.emit(0x2E, 0xAD)                          # lodsw from cs:[si]
    patch.emit(0x0D, 0x20, 0x20)                    # lower-case both letters
    patch.emit(0x3D, 0x65, 0x78)                    # "ex"
    patch.short_jump(0x75, "continue")

    patch.emit(0x2E, 0xAD)                          # lodsw from cs:[si]
    patch.emit(0x0D, 0x20, 0x20)
    patch.emit(0x3D, 0x69, 0x74)                    # "it"
    patch.short_jump(0x75, "continue")

    patch.label("exit")
    patch.near_jump(EXIT_OFFSET)

    patch.label("continue")
    patch.emit(0x0A, 0xD2)                          # or dl,dl
    patch.short_jump(0x75, "after_command")

    # Initial DN.PRG return: reproduce the five bytes displaced by the hook.
    patch.emit(0xB8, 0x01, 0x33)                    # mov ax,3301h
    patch.emit(0xB2, 0x01)                          # mov dl,1
    patch.near_jump(INITIAL_CONTINUE_OFFSET)

    # COMMAND.COM return: restore the saved status, reproduce the displaced
    # status/ctrl-break sequence, then continue with the normal DN.PRG loop.
    # The hook covers the complete original MOV AX,3301h, so execution can
    # resume at the original XOR DL,DL instead of duplicating that tail here.
    patch.label("after_command")
    patch.emit(0x58)                                # pop ax
    patch.emit(0x2E, 0xA2, COMMAND_STATUS_ADDRESS & 0xFF,
               COMMAND_STATUS_ADDRESS >> 8)        # mov [cs:01C7h],al
    patch.emit(0xB8, 0x01, 0x33)                    # mov ax,3301h
    patch.near_jump(AFTER_COMMAND_CONTINUE_OFFSET)
    return patch.finish()


def near_jump(source: int, target: int) -> bytes:
    displacement = target - (source + 3)
    return bytes((0xE9, displacement & 0xFF, (displacement >> 8) & 0xFF))


def patch_loader(source: Path, output: Path) -> None:
    data = bytearray(source.read_bytes())
    if data[ENTRY_OFFSET:ENTRY_OFFSET + 3] != b"\xB8\x01\x33":
        raise SystemExit("unexpected DOSNavigator loader entry signature")
    if data[AFTER_COMMAND_OFFSET:AFTER_COMMAND_OFFSET + 7] != b"\x2E\xA2\xC7\x01\xB8\x01\x33":
        raise SystemExit("unexpected DOSNavigator post-command signature")
    if data[RESIZE_OFFSET:RESIZE_OFFSET + 5] != b"\x8B\xDF\x83\xC3\x10":
        raise SystemExit("unexpected DOSNavigator resident-resize signature")

    patch_offset = len(data)
    exit_patch = make_exit_patch(patch_offset)
    patched_end = 0x100 + patch_offset + len(exit_patch)
    if patched_end > RESIDENT_BYTES:
        raise SystemExit("DOSNavigator EXIT patch exceeds reserved resident area")

    entry_hook = b"\xB2\x00" + near_jump(ENTRY_OFFSET + 2, patch_offset)
    command_hook = b"\x50\xB2\x01" + near_jump(AFTER_COMMAND_OFFSET + 3, patch_offset) + b"\x90"
    data[ENTRY_OFFSET:ENTRY_OFFSET + len(entry_hook)] = entry_hook
    data[AFTER_COMMAND_OFFSET:AFTER_COMMAND_OFFSET + len(command_hook)] = command_hook
    # DN.COM normally shrinks its MCB to a dynamic data pointer and releases
    # its initialization tail.  Keep a bounded 4.5 KiB resident block so the
    # appended hook remains owned while DN.PRG and a child shell run.
    data[RESIZE_OFFSET:RESIZE_OFFSET + 5] = bytes(
        (0xBB, RESIDENT_BYTES & 0xFF, RESIDENT_BYTES >> 8, 0x90, 0x90)
    )
    data.extend(exit_patch)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(data)
    print(
        f"[dosnav-patch] patched EXIT launcher path: {source} -> {output} "
        f"({len(exit_patch)} appended bytes, resident=0x{RESIDENT_BYTES:04X})"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    patch_loader(args.source, args.output)


if __name__ == "__main__":
    main()
