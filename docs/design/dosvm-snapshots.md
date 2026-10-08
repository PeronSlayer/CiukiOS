# Physical DOS-window snapshots

## Sources and decision

The [original DOOM startup code](https://raw.githubusercontent.com/id-Software/DOOM/master/linuxdoom-1.10/d_main.c)
prints `ST_Init` before graphics and the main game loop begin. This source
establishes startup ordering, not the implementation of the original DOS sound
library. A successful presenter call likewise does not establish gameplay.

The [upstream HDPMI 3.24 source](https://raw.githubusercontent.com/Baron-von-Riedesel/HX/f2276db9accfc57facf2588bc016a27130597bb1/Src/HDPMI/HDPMI.ASM)
provides client lifecycle and IRQ return boundaries. CiukiOS keeps file I/O in
the desktop application's foreground event handling, outside those callbacks
and the band painter. The handle-based create/write/close contracts are
documented in Microsoft's [MS-DOS Encyclopedia, System Calls](https://www.pcjs.org/documents/books/mspl13/msdos/encyclopedia/section5/);
the writer checks the exact byte count and the host decoder rejects short files.

Repository contracts checked before implementation:

- `session_abi.inc`: operation 4Ah returns state in BX, exit code in DX and
  slot generation in CX. Generation and exit code survive DEAD-to-FREE;
  creation increments the generation.
- `session_vmm.inc`: VIDEO_STATE and DEV_STATE can inspect a target VM while
  the caller's output buffer remains mapped. Freed VM instances cannot be
  inspected; a freed target falls back to the caller's session. Samples
  therefore require READY state and the original slot generation before and
  after their queries.
- `session_video.c`: VIDEO_STATE constructs a fresh packet from the raw model.
  The shared header's `live[]` is refreshed separately and can be older.
- `session_video_abi.inc` and `session_devices.h`: validated packet sizes are
  256 and 128 bytes, both version 0100h.

Each window keeps the latest validated video/device packets in RAM. Attempts
are at least 19 BIOS ticks apart, slightly over one second at 18.20648 Hz.
Each packet retains its own successful sample tick; a failed later query never
turns an old packet into a fresh one. Failed outputs are initialized and never
accepted without the expected magic, version and size.

One fixed record is saved after 219 ticks (about 12 seconds) while a VM remains
live, on observed natural exit, and before a user-requested kill. The timeout
save is attempted only once, including when the disk write fails. There are no
per-frame file writes. Snapshots overwrite `SYSTEM/DOSVM1.BIN` through
`SYSTEM/DOSVM3.BIN`, according to VM slot, and never grow. The latest validated
cache survives natural teardown. `DOSVM.LOG` identifies the command and records
save failures and lifecycle transitions.

## CDVS version 0100h layout

All integers are little-endian. The record is exactly 448 bytes; there are no
pointers, variable strings or padding.

| Offset | Size | Meaning |
| --- | --- | --- |
| 0 | 4 | Magic `CDVS` |
| 4 | 2 | Version 0100h |
| 6 | 2 | Record size, 448 |
| 8 | 2 | VM slot, 1–3 |
| 10 | 2 | Original slot generation |
| 12 | 2 | BIOS tick low word at persistence |
| 14 | 2 | Valid packet mask: bit 0 video, bit 1 devices |
| 16 | 2 | Reason: 1 live timeout, 2 observed exit, 3 user close |
| 18 | 2 | Last observed VM state: 0 free, 1 ready, 2 dead |
| 20 | 2 | Observed exit code, FFFFh while live/unknown, FFFEh killed |
| 22 | 2 | Latest video query result |
| 24 | 2 | Latest device query result |
| 26 | 2 | Last sample attempt tick |
| 28 | 2 | Last validated video sample tick |
| 30 | 2 | Last validated device sample tick |
| 32 | 32 | NUL-terminated window title, supplementary to DOSVM.LOG |
| 64 | 256 | Latest validated VIDEO_STATE (`CVVS`) packet, or zeros |
| 320 | 128 | Latest validated DEV_STATE (`CVDV`) packet, or zeros |

Query results are 0 for success, FFFEh for a rejected packet ABI, FFFFh when
the target/query is unavailable, or the returned CVSESSION error code.
Packet ages use unsigned 16-bit tick subtraction, including one tick wrap.
The decoder cannot determine elapsed time spanning an entire tick-word cycle.

Width, rows and text flag describe raw scanout geometry; the reported BIOS
mode can remain 03h after software programs VGA registers directly. Presenter
counts describe compositor activity and do not prove that a game rendered a
level. Protected-mode faults are normal VGA emulation accesses unless fatal
or unsupported counters also indicate an error. A user-close snapshot records
the state before the kill, not its unobserved eventual return code.

Use `python3 scripts/inspect_dosvm_snapshot.py SYSTEM/DOSVM1.BIN` to decode a
captured record. The implementation does not change guest memory allocation,
timer debt, IRQ policy or audio rendering.

`scripts/capture_physical_logs.py --mount <volume-root> --output <new-directory>`
copies the fixed diagnostic list, including DOSVM1..3.BIN and VIDEO/CACHE.LOG,
and decodes each available record. It only reads the source volume, records
mount options and hashes, and identifies records absent from earlier builds.
The diskseq59 read-only recheck exercised this collector against the actual
connected T23 disk; its older image has none of the new snapshot records.

## Bounded implementation checks, 2026-10-08

The canonical 16-bit OpenWatcom DOSVM compile passed with warning-as-error
flags in a 128 MiB memory / 64 MiB swap user scope. The resulting isolated
object is `build/tests/dosvm-snapshots-20261008/dosvm.obj`; this check does not
replace linking the full image or running the physical machine.

Fourteen host tests passed. The runtime test compiles the actual production
snapshot helper bodies and packed structures with mocked VM and DOS calls.
It checks the 19-tick sampling interval, tick wrap, failed queries retaining
their prior successful timestamp, malformed ABI rejection, slot reuse between
queries, same-generation teardown, unavailable targets, pre-kill persistence
and short writes. It then
decodes the exact 448-byte record emitted by those helpers. The remaining
tests exercise decoder identity/size checks, partial captures and lifecycle
interpretation. No guest QEMU or full build was run for these focused checks.
