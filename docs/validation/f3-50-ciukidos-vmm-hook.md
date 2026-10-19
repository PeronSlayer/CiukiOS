# f3-50 implementation and source decisions

Research performed before implementation on 2026-10-19. This note describes
the private Ciuki handoff, not a Microsoft multiplex API or NTVDM interface.

Sources consulted:

- [Microsoft DOS 3.3 Programmer's Reference](https://www.pcjs.org/documents/books/mspl13/msdos/dosref33/): PSP, FCB and process services.
- RBIL 61: [PSP/26h](https://fd.lod.bz/rbil/interrup/dos_kernel/2126.html),
  [55h](https://fd.lod.bz/rbil/interrup/dos_kernel/2155.html),
  [SFT/SysVars](https://fd.lod.bz/rbil/interrup/dos_kernel/2152.html),
  [SDA](https://fd.lod.bz/rbil/interrup/dos_kernel/215d06.html),
  [65h](https://fd.lod.bz/rbil/interrup/dos_kernel/2165.html),
  [59h](https://fd.lod.bz/rbil/interrup/dos_kernel/2159_bx0000.html),
  [1203h](https://fd.lod.bz/rbil/interrup/dos_kernel/2f1203.html),
  [1216h](https://fd.lod.bz/rbil/interrup/dos_kernel/2f1216.html),
  [1220h](https://fd.lod.bz/rbil/interrup/dos_kernel/2f1220.html),
  [1226h](https://fd.lod.bz/rbil/interrup/dos_kernel/2f1226.html), and
  [122Dh](https://norton-guides.davep.dev/rbint/rbint-3558542.html).
- FreeDOS upstream [fcbfns.c](https://raw.githubusercontent.com/FDOS/kernel/master/kernel/fcbfns.c):
  FcbReadWrite, FcbRandomIO, FcbRandomBlockIO, FcbOpen, FcbParseFname;
  [inthndlr.c](https://raw.githubusercontent.com/FDOS/kernel/master/kernel/inthndlr.c).
  Behavioral research only; no FreeDOS implementation is incorporated.

Decisions: selected PSP and immutable EXEC ancestry are separate. Ordinary
EXEC shares the DOS drive/cwd; DTA restoration is per process. AH=26h copies
the caller-CS PSP and sets guest parent to zero without selecting it; AH=55h
inherits the selected PSP, applies SI memory size and selects the child.
The native SFT remains authoritative even when a guest overwrites its mirror.
FCB reserved bytes carry no native pointer. CP437 has no DBCS ranges; NLS
tables use stable guest addresses and unsupported country/page requests fail.
65h/01 accepts the documented minimum CX=5, returning the bounded prefix of
the 41-byte country result and its actual length in CX. The guarded host
cases cover every requested length from 5 through 45; lengths below 5 fail.
1226h is admitted only on the registered DOS internal stack and uses the same
description service. Unknown multiplex calls chain without advertising ANSI
or a redirector. The FreeDOS master source is not a pinned release oracle;
executed differential comparison belongs to f3-51.

## DFVM version 1 wire layout

Every integer is little endian. Far pointers are a u16 offset followed by a
u16 real-mode segment. Native pointers/selectors are never exported. Discovery
requires all four AX=C148h/BX=4644h/CX=1/DX=564Dh words. It returns AX=0,
CX=1, ES:DI at the immutable synthetic-ROM descriptor. Before binding the VMM
also checks the owning personality's CIUKIDOS image signature. Signature
mismatches pass to the previous multiplex handler unchanged.

Descriptor offsets: 0 `DFVM` (4), 4 version (u16=1), 6 size (u16=20),
8 features (u32=7: file gate, lifecycle, CP437 tables), 12 file gate (far),
16 event gate (far). Gates are F000:1800 and F000:1804; the critical return
is F000:1808. All start with INT FE. The first two end with RETF, the last
with a monitor-only continuation. Compile-time assertions reserve these
against BIOS vectors, mouse return and both complete DPMI stub regions.
The descriptor is at F000:1810. Tables occupy 1840–1B53; the callable
country case mapper at 1828 is a register-preserving far routine.

The BIND record is exactly 68 bytes in v1:

| Offset | Width | Field |
| --- | --- | --- |
| 0 | 2 | version = 1 |
| 2 | 2 | record byte size = 68 |
| 4 | 4 | saved legacy INT21 entry |
| 8 | 4 | SDA |
| 12 | 2 | SDA byte extent, at least 20h |
| 14 | 4 | List-of-Lists |
| 18 | 4 | first SFT block, including six-byte link/count header |
| 22 | 2 | SFT entry size, exactly 59 |
| 24 | 2 | SFT entry count, 5–128 (personality supplies 20) |
| 26 | 4 | selected-PSP word address |
| 30 | 4 | current DTA far-pointer field address |
| 34 | 4 | one-byte dispatch result address |
| 38 | 4 | live InDOS byte address |
| 42 | 4 | 26 media descriptor bytes |
| 46 | 4 | shared DOS default-drive byte address |
| 50 | 4 | four checked far pointers to CON/AUX/PRN/NUL headers |
| 54 | 4 | private C: DPB (at least 21h bytes) |
| 58 | 4 | reserved expanded-JFT arena, 16 × 232 bytes |
| 62 | 4 | internal DOS stack start |
| 66 | 2 | internal DOS stack extent |

AX selects event 0–8; BX names the affected/selected PSP and CX its immutable
EXEC parent (owning PSP for EXEC_BEGIN). BIND requires BX=CX, a valid root
PSP, checked readable code/driver records and writable control/mirror extents.
Control records must not overlap one another or the saved entry. The VMM
allocates/mounts its candidate before publishing the binding and unwinds it
on failure. The source provider uses the existing VM overlay/read cache and
the system block-cache workspace; there is no new data cache or memory cap.

Events 1–8 use an eight-byte DS:SI record: version u16=1, size u16=8,
creation kind u16 (0 normally, 26h/55h for PSP_CREATE), reserved u16=0.
The VMM chooses the appropriate checked slot in the registered JFT arena.
A copied AH26 PSP can subsequently be replaced by AH55. Event result is
AX=0/CF=0, otherwise DOS AX/CF=1. Reversible process publication keeps native
reference counts and the previous selected/owning identity on failed mirrors.

At the file gate's INT FE, the stack contains only the far CALL's return
address before the original interrupt image:

```text
SS:SP + 0  hook-return IP
      + 2  hook-return CS (checked personality segment)
      + 4  original interrupt IP
      + 6  original interrupt CS
      + 8  original saved FLAGS
```

CHAIN preserves registers/live FLAGS and jumps to the saved entry once.
DONE returns result registers and patches saved FLAGS before RETF/IRET.
PENDING exists only in the monitor: INT24 returns through its checked ROM
continuation, then restores the suspended gate frame. The guest never polls
or repeats a partly completed request. Native adapter operations do not
recursively issue INT21. EXEC creates its own internal request frame and
opens/seeks/reads/closes the same descriptions; COM/MZ/overlay loaders use
that adapter whenever bound. An outer EXEC's InDOS byte survives these
internal transfers.

## Routing and retained fallback

The owner handles INT21 0D/0E, 0F–17, 19/1A/1B/1C, 21–24, 27/28/29,
2F/36/38, 39–47, 4E/4F, 56/57/59, 5A/5B/5C, 60/65/66/67/68/6C.
43/57/5C admit subfunctions 0–1, 6C only 00. Device console transfers and
unsupported device-specific IOCTLs chain to the personality. 26 remains PSP
creation; 48–55/process services keep the existing allocator and EXEC code.
INT2F handles 1000/1203/1216/1220/1226/122D. Unknown multiplex calls chain.
No 11xx redirector or 1Axx ANSI installation is advertised.

FCBs use checked 37-byte input or a seven-byte extended prefix plus 37 bytes.
The bounded native map holds VM-lifetime/PSP/FCB-address/name identities;
reserved FCB bytes convey no native authority. Find results use drive plus
the DOS 32-byte directory image, with the original extended prefix when
present. Reads pad a short final record; AL is 0/1/2/3, CX counts block
records. Single random I/O does not advance random position; block calls do.
Errors also update the per-process extended-error record.

No sector label is deleted: all remain reachable in early boot or the
explicitly disabled-binding personality. The active EXEC branch bypasses
`int21_resolve_and_find_path` and `int21_exec_load_to_es`'s sector body.
The file INT21 gate bypasses legacy file/directory entries before their
prologue. `int21_sync_sft_table` becomes inactive when bound. The old CLFN
sector-service discovery is refused while bound, preventing a resident
LFN.COM from installing a second FAT owner; it remains available with the
binding disabled. This is an interface refinement for lead review, not
new LFN/71xx qualification. The original command interpreter is unchanged;
its AUTOEXEC wrapper is the existing batch facility, not a new general BAT
interpreter.

The unchanged f3-48 `truename` COM exposed an inherited f3-49 input-register
error: AH60 must read DS:SI, as specified by [RBIL 60h](https://fd.lod.bz/rbil/interrup/dos_kernel/2160.html),
not DS:DX. The production service and the old direct harness were corrected
together; the unchanged COM supplies independent coverage.

AH55 also exposed a pre-existing private-PSP adoption in the legacy path.
[FreeDOS `child_psp` and dispatcher](https://raw.githubusercontent.com/FDOS/kernel/master/kernel/task.c)
initialize PSP/JFT/current selection without changing the caller's allocated
MCB owner. The bound path now retains that owner, so the allocating parent can
free the block after selecting itself again. The disabled-binding path keeps
its previous adoption behavior. FreeDOS master explicitly disputes RBIL's
AH26 zero-parent detail; this implementation follows the documented zero-parent
contract, and the pinned FreeDOS 1.3 differential remains an f3-51 review item.

Bound relative EXEC now canonicalizes through the private AH60 adapter before
lookup, rather than prefixing the old sector personality's cwd buffer. A
production COM fixture changes cwd to RELBASE and executes its TSR helper by
relative name, then checks failed EXEC after TSR does not retain a provisional
child. Malformed MZ relocation and a successful relocated overlay use the
same description adapter. Overlay scratch has its own checked 64 KiB bound;
it cannot inherit a previous COM/MZ placement's smaller copy limit.

Partial handle-read Retry retains the completed byte count and resumes at the
remaining buffer suffix and native position. The guarded harness faults the
second 4 KiB, alters one byte in the completed prefix during suspension and
verifies that Retry preserves it, returns AX=8192 and ends at position 8192.
The changed C units are additionally compiled with the real i686 kernel flags
and the existing 4096-byte frame ceiling. Separate handle/FCB/internal service
functions keep their large path workspaces out of unrelated call stacks.

Executed host evidence and explicit qualification limits are retained in
`build/codex/f3-50-report.md`. The maintained focused harness is
`tests/host/dos/run_file_api.sh`; the production CPU fixture is
`tests/host/dos/run_hook_cpu.sh`. The full kernel host script runs both
sequentially. CPU transport waits between probes for the monitor's existing
16-record/s limit and distinguishes a probe's cooperative 1680h yield from a
command prompt; it does not change guest COM bytes or production DOS rules.

Version-1 ROM gates additionally contain an immutable fourth-byte role marker:
D1h dispatch, E1h event, C1h critical return. CiukiDOS validates the descriptor
extent, canonical ROM segment, protected-page bounds and `INT FE; RETF; role`
before BIND. Swapped, overlapping, DPMI-occupied and out-of-range pointers
therefore fail locally with ownership unchanged, like absent/old descriptors.
These code bytes identify Ciuki gates; they are not Microsoft multiplex data.
