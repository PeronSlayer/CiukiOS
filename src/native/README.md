# CiukiOS native image prototype

`CN32` is a small, little-endian, freestanding 32-bit image container for a
protected-mode CiukiOS loader. `NATIVE.COM` is the DOS-side file transport; it
passes a bounded image to CVSESSION, which validates and copies it before any
native address-space switch. A bounded CPL3 sample ran in QEMU on
2026-10-02. This is an entry prototype, not a multitasking native process
environment; runtime qualification is recorded separately.

## Header version 1

The header is 36 bytes, packed with no padding:

| Offset | Size | Field | Meaning |
| ---: | ---: | --- | --- |
| 0 | 4 | magic | ASCII `CN32` |
| 4 | 2 | version | `1` |
| 6 | 2 | header size | `36` |
| 8 | 4 | flags | `1`: flat 32-bit IA-32, no relocations |
| 12 | 4 | entry offset | Offset from the first code byte; must be within code |
| 16 | 4 | code size | Initialized executable bytes |
| 20 | 4 | data size | Initialized writable bytes, mapped after code |
| 24 | 4 | stack size | Requested zeroed stack bytes; not stored in the file |
| 28 | 4 | payload size | `code size + data size` |
| 32 | 4 | CRC-32 | IEEE CRC-32 of code followed by data |

Version 1 images have no relocation table, BSS, imports, or API table. Loader
implementations should reject unknown flags and arithmetic overflow. Each
invocation gets a private page directory, read-only code, writable data and
stack, with unmapped gaps. The kernel and syscall entry mappings remain
supervisor-only. Execution currently has an instruction budget and runs
synchronously while the DOS transport waits. Scheduling, a native heap and a
desktop app ABI remain to be implemented.

## DOS file transport

`NATIVE.COM [path]` opens one CN32 file through the standard DOS file-handle
API. The default path is `\VM\NATIVE32.N32`. The complete header and payload
must fit within 61,440 bytes and one conventional-memory segment; the native
host repeats all image validation after copying the bytes. The transport uses
the standard MS-DOS 3.3 [file-handle open/read/close and memory-block
services](https://www.pcjs.org/documents/books/mspl13/msdos/dosref33/), as
implemented here by CiukiDOS `INT 21h/3Dh`, `3Fh`, `3Eh`, `48h`, and `4Ah`.
The 61,440-byte ceiling applies only to this initial DOS transport, not to
the native process address space.

For the synchronous entry, `NATIVE.COM` prints `EXIT 0` for a zero native
status or `EXIT status=XXXXXXXX` for a nonzero status and returns its low byte
as the DOS process code. A fault or instruction-budget result is reported as
`FAIL native execution` with vector, error code and step count.

At entry the loader sets `ESI` to this process's private writable data base,
`EDI` to its data byte count, and `ESP` to the top of its private zeroed stack.
Other general registers are zero. A version-1 image can address its data
through `ESI` without a relocation table. The sample reports the first data
word as a second marker to verify this mapping.

## INT 80h syscall ABI

Calls are made with `int 0x80` from CPL3. The kernel must validate every
argument and return to the same process only for calls that return.

| EAX | Name | Inputs | Return |
| ---: | --- | --- | --- |
| 0 | exit | `EBX` = process status | Does not return |
| 1 | report | `EBX` = numeric tag, `ECX` = 32-bit numeric value | `EAX` = 0 on acceptance, negative on error |

The initial report call carries numeric values only; no user pointer is passed.
The sample reports tag `1`, value `0xC1A0`, and tag `2`, value `0x4349554B`,
then exits with status zero. The current gate handles these two calls, exits
or faults back to the DOS transport, and reports an instruction-budget result
if the process does not terminate. It is not yet a general native syscall ABI.
