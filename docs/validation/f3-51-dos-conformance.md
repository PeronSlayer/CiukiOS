# f3-51 DOS conformance integration

Date: 2026-10-19. Implementation and host evidence are distinct from reference,
game and physical acceptance. No QEMU, image build or hardware run is performed
by Codex. The lead supplies those results against the final image SHA-256.

## Research and implementation decisions

Sources checked before implementation:

- The [official FreeDOS 1.3 build report](https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.3/official/report.html)
  identifies the release package set, FreeCOM 0.85a and SHARE. A vendored master
  source tree cannot identify the release binaries. Package and member hashes
  must be obtained from the actual official archives; absent media is an error.
- Microsoft's [MS-DOS 3.3 Programmer's Reference](https://www.pcjs.org/documents/books/mspl13/msdos/dosref33/)
  describes EXEC, inherited handles and process termination. Run unchanged COM
  children with separate scratch directories and verify return and cleanup.
- [RBIL INT24](https://fd.lod.bz/rbil/interrup/dos_kernel/24.html) requires a real
  underlying critical error, with Retry/Abort/Fail semantics. A missing file or
  direct callback invocation cannot qualify a critical-error fixture.
- [RBIL 2F/1100](https://fd.lod.bz/rbil/interrup/network/2f1100.html) and
  [2F/1203](https://fd.lod.bz/rbil/interrup/dos_kernel/2f1203.html) distinguish an
  installed redirector from DOS internal state. No network stack is advertised.
- The [Windows 95 DDK Ring0 FileIO topic](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S312A.HTM)
  distinguishes owned ring-0 handles from INT21 handles and context record locks.
  Retain the f3-49/f3-50 owner and cleanup interfaces. The missing full hook/SFT
  reflection documentation from f3-48 remains an access limitation.
- [FreeDOS dosfns.c](https://github.com/FDOS/kernel/blob/master/kernel/dosfns.c)
  (`DosOpenSft`, `DosRWSft`, `DosLockUnlock`) and
  [fcbfns.c](https://github.com/FDOS/kernel/blob/master/kernel/fcbfns.c)
  (`FcbReadWrite`, `FcbRandomIO`, `FcbRandomBlockIO`) are behavioral references,
  not a release pin; no implementation is copied.
- The [Open Watcom DOS/4GW translation chapter](https://open-watcom.github.io/open-watcom-1.9/pguide.html#DOS_4GW__Interrupt_21H_Functions)
  documents register and pointer translation, not executed game calls.
  [DOOM WAD access](https://github.com/id-Software/DOOM/blob/master/linuxdoom-1.10/w_wad.c),
  [Quake DOS file access](https://github.com/id-Software/Quake/blob/master/WinQuake/sys_dos.c)
  and [Wolf3D cache access](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/ID_CA.C)
  support the inventory's source patterns. Reachability and no-drop execution
  traces must be supplied separately; Quake is go32 and Wolf3D uses LZEXE.

The sandbox cannot resolve download hostnames. The official report is accessible
through browsing, but no FreeDOS 1.3 archive or release binary is present in this
worktree. Never substitute master or invent a package/member hash.

## Acceptance ledger

| Gate | Status |
| --- | --- |
| H51-01 protocol | Host subset PASS (selector, read-back, framing, input, process cleanup); reference and media-fault integration OPEN |
| H51-02 inventory | PASS 12-file audit reproduction and seven LZEXE load-module audits; LE/go32/runtime reachability and title traces OPEN |
| H51-03 differential | Open: pinned reference and two complete same-image lead runs required |
| H51-04 clients/games | Host SHELL/EXEC/DPMI/DOS4GW regressions PASS; same-image game/audio/contention/hardware evidence OPEN |
| H51-05 performance | Open: calibrated T23 measurements required; host timings cannot close this gate |
| H51-06 fallback | Host preferences, native launcher replay, kernel/COW policy and lifetime PASS; actual dual-kernel launch OPEN |

For the LZEXE audit, researched the maintained upstream
[UNLZEXE reader](https://raw.githubusercontent.com/mywave82/unlzexe/master/unlzexe.c)
(`mkreltbl`, `unpack`, `getbit`). Its stream descriptor identifies the compressed
load module and original CS:IP. The new bounded reader derives a load-module
hash and disassembles from that entry; unresolved indirect transfers stay open.
It does not rewrite a game, infer execution from byte adjacency or claim that
static traversal covers demo/play/save paths.

The bundled FreeDOS reference contains the pinned shareware corpus. If an
owner-local workload base is attached, its FreeDOS override is refused: switching
kernel must not silently switch game editions or data. That separate library
needs its own approved reference fixture before dual-kernel qualification.


## Delivered host evidence

`nice -n 19 python3 -B -m unittest discover -s tests/host/dos -p 'test_*.py' -v`
passes 39 tests. The original battery remains 38 unchanged cases plus its EXEC
helper; the shell-return witness is separate controller glue. The focused
H49/H50 production sanitizer harness passes 5,640,894 checks, and the full
kernel host script exits 0. Its canonical-DOSBOOT-dependent block is skipped
because no canonical image is built here; independent CPU fixtures execute
SHELL, COM/MZ/overlay, DPMI16/32, DOS/4GW and the bridge-disabled policy.
The final production VMM replay after the local-workload refusal change passes
62,283,776 checks, with a maximum evidence record of 237 bytes.

The native launcher tests pass its typing replay and 13 window scenarios,
including failed reference creation with preference retention, dialog selection,
focus release/restore, pixel restoration and closing while the dialog is open.
All seven changed production C units compile with the canonical i686 flags and
4096-byte frame ceiling. `python3 scripts/gen_api_reference.py` and
`nice -n 19 make CAP= apps` pass. No image build, QEMU, scope, commit or diary
edit is performed. Detailed command logs and remaining work are recorded in
`build/codex/f3-51-report.md`.

The static [load-module audit](f3-51-static-reachability.json) records original
and decompressed hashes, original entry points, decoded direct edges and every
unresolved transfer for the seven LZEXE executables. Other body formats stay
open. These are possible static paths, never loader/menu/play/save execution
counts. No newly observed service is claimed without a trace.

## Open implementation and qualification

This section records the initial partial delivery. The continuation below
supersedes its implementation gaps; runtime qualification remains open.

The removable `E:\D48FAULT.DAT` provider and its post-open one-shot/persistent
completion injection/reset are not implemented. The controller fails these
cases with `fault_backend_unavailable`, never a skip or synthetic result, and
cannot emit a successful complete END. The official 1.3 archive/member/license
and boot-sector pins are absent. Reference provisioning code is host-tested
with synthetic inputs; actual reference build/boot remains unqualified.

The upstream [FreeDOS block driver](https://raw.githubusercontent.com/FDOS/kernel/master/kernel/dsk.c)
contains BIOS retries in `LBA_Transfer`. One failing INT13 attempt can be
recovered before INT24, so the release-specific completion boundary needs
qualification. No direct callback invocation substitutes for a media error.

`core/output.c::emit_line` allocates one sequence for serial and disk-only
records. The parser rejects duplicate/backwards sequences and declared loss;
generic gaps require the lead's disk log. Complete no-drop title traces and
DOS/4GW/go32 body/runtime mapping remain open, alongside both differential
pairs, actual dual-kernel game runs and calibrated T23 DF6 measurements. All
runtime H51 gates remain OPEN. Targets and existing game audio/video thresholds
are unchanged.

## Continuation research (Amendment 1)

The official archives are provisioned in the main checkout; this worktree uses
ignored read-only symlinks to those exact files. Their package, binary and
license hashes are checked against `third_party/freedos/1.3/UPSTREAM.json`.
The preceding absence statements describe the initial partial delivery.

The pinned `kernel.zip` embeds `SOURCE/KERNEL/SOURCES.ZIP` (SHA-256
`a977497b7d1b2990dc50542cf1a8da2965079f74c37508d5894ecc02a4c8a9d5`).
Its `hdr/device.h` defines `N_RETRY=5`; `kernel/dsk.c::LBA_Transfer`
repeats a failed INT13 transfer five times, resetting the disk between attempts.
These release sources, rather than current master, determine the test boundary.
The [upstream block driver](https://raw.githubusercontent.com/FDOS/kernel/master/kernel/dsk.c)
was also researched online; the release tag endpoint was inaccessible to the
browser. One injected reference media episode therefore fails five BIOS
completions, versus one candidate provider completion. The first subsequent
DOS Retry can succeed. Persistent episodes remain armed through Fail/Abort.
[RBIL INT24](https://fd.lod.bz/rbil/interrup/dos_kernel/24.html) and the
[Microsoft reference](https://www.pcjs.org/documents/books/mspl13/msdos/dosref33/)
require the real error path; no test calls the guest handler directly.

`kernel/initdisk.c::ReadAllPartitionTables`, `ProcessDisk` and
`init_readdasd` were checked in the same release sources and against
[upstream initdisk.c](https://raw.githubusercontent.com/FDOS/kernel/master/kernel/initdisk.c).
FreeDOS discovers C: through a partition table, unlike CiukiDOS's private
superfloppy layout. The reference therefore needs an ordinary MBR and FAT16
partition at LBA 63, retaining the exact pinned VBR code with deterministic BPB
fields and the same 128 MiB virtual disk capacity. The disposable second BIOS
disk has two FAT16 partitions (D:, E:), reports removable/change-line media and
contains 4096 `F` bytes in E:\\D48FAULT.DAT. The candidate maps that exact E:
volume through its existing logical VFS provider. A fresh VM resets provider
identity, fault state and cache lifetime between cases.

For protected bodies, consulted the [DJGPP COFF file header](https://www.delorie.com/djgpp/doc/coff/filhdr.html),
[section header](https://www.delorie.com/djgpp/doc/coff/scnhdr.html),
[Open Watcom executable definitions](https://raw.githubusercontent.com/open-watcom/open-watcom-v2/master/bld/watcom/h/exeos2.h)
and the [IBM module format specification](https://www.bitsavers.org/pdf/ibm/pc/os2/OS2_OMF_and_LX_Object_Formats_Revision_8_199406.pdf).
Only structurally identified executable sections/objects are traversed.
Unresolved indirect edges and runtime paths remain explicit. The id Software
[Quake DOS implementation](https://github.com/id-Software/Quake/blob/master/WinQuake/sys_dos.c),
[DOOM file access](https://github.com/id-Software/DOOM/blob/master/linuxdoom-1.10/w_wad.c),
[Wolf3D cache](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/ID_CA.C)
and [DOS/4GW translation documentation](https://open-watcom.github.io/open-watcom-1.9/pguide.html#DOS_4GW__Interrupt_21H_Functions)
remain source/translation evidence, never proof of an executed call.

The provisioned floppy VBR is specifically FAT12. Release `boot/boot.asm`
contains separate FAT12/FAT16 cluster decoders; changing BPB fields cannot
change that executable algorithm. The installed VBR now uses the **exact
FAT16 template embedded in the pinned release's SYS.COM**, not rebuilt code.
The new `boot_fat16` pin and 512-byte `boot16.bin` preserve its provenance;
the original floppy pin is retained. A host CPU test stops the SYS unpacker at
its first INT21 (AX=4A00, CS:IP=1000:0197) and reproduces the template at
1000:2FC8. No SYS installation or DOS/block service is executed. SHA-256:
`39586cd0f55f7732791a990770163d4af9827623e418203921d6f23819b94cb5`.
The release source selection is cross-checked against upstream
[boot.asm](https://raw.githubusercontent.com/FDOS/kernel/master/boot/boot.asm)
and [sys.c](https://raw.githubusercontent.com/FDOS/kernel/master/sys/sys.c).
The source archive, GPL notice and reproduction procedure are documented in
[the reference README](../../third_party/freedos/1.3/README.md). This boot
correction needs lead review and actual reference boot evidence.

## Continuation delivery and qualification ledger

The runtime controller opens the existing system-volume write gate: selected
probes otherwise start read-only. It owns and joins a bounded boot-log drain
task because the ordinary storage writer is absent in probe mode. Each case
still owns a fresh VM/private COW, checked successful open, fresh E: generation,
unaltered COM/helper bytes, shell-return witness and teardown checks.

DOS tracing now includes real-kernel console/process/multiplex calls alongside
hook-served calls. Four bounded fragments retain all registers, PSP/origin,
entry/return, byte counts and monitor/wall time. Dispatch and return drain the
ring promptly. Export serialization prevents a scheduling/reentrant dump from
interleaving one VM's fragments. Process termination and VM cancellation are
explicit terminal edges, never invented return results. Nonreturning exit
calls no longer count as dropped returns; unexplained returns still fail.

Protected UART trace rows do not consume the bounded disk-log budget. Existing
disk-only records and trace rows share one output sequence; the closing ledger
records its exact watermark and the selected run's actual first sequence,
preserving any earlier boot sequence allocation. The differential runner reads bounded persistent
logs directly through its stopped qcow2 overlay, reconciles the sequence union,
checks complete per-VM call/fragment/teardown ledgers, then removes the overlay
on every outcome. No conversion, full-image copy or mount is used. The reader
follows the primary [QEMU qcow2 format](https://www.qemu.org/docs/master/interop/qcow2.html)
and rejects unsupported feature/cluster formats. UART gaps cannot qualify from
an asserted zero-drop flag. Standalone title reconciliation additionally checks
the canonical image and original executable hashes, stage ranges and concrete
test coverage; unsupported DOS functions and unapproved failed calls fail.

The regenerated static audit maps DOOM's executable LE object and Quake's
DJGPP COFF text. From their actual entries it decodes 33,455 and 56,405
instructions respectively, with 42 and 3 possible INT21/2F sites. There are
92 and 195 unresolved transfers respectively. These counts describe static
possibilities only; neither full title paths nor go32/DOS4GW runtime libraries
are declared qualified from them. All 12 original hashes/sites still reproduce.

| Gate | Continuation status |
| --- | --- |
| H51-01 | Host parser/read-back, actual release pins, real provider critical errors, INT13 retry/reset, keyboard readiness, trace framing/loss and owned cleanup checks available; final-image runtime OPEN |
| H51-02 | Static 12-file/LZEXE/LE/COFF audit delivered; complete observed title paths and selector-to-test reconciliation OPEN |
| H51-03 | Official reference and canonical builder/read-back implementation delivered; actual canonical image/read-back and two independent differential guest pairs OPEN |
| H51-04 | Host client/launcher regressions pass; original-game video/input/audio/config/save/focus/contention and final-image hardware evidence OPEN |
| H51-05 | OPEN: calibrated T23 distributions, 60 µs / 2 ms maxima, INT13/cache/wait/cancel and final ledgers require physical measurements |
| H51-06 | Existing independent preferences, explicit override, reference errors and COW/close/relaunch host checks pass; actual same-game dual-kernel launches OPEN |

Final command results and source/artifact identities are in
`build/codex/f3-51b-report.md`, also appended as Continuation to the preserved
`build/codex/f3-51-report.md`. No canonical build, QEMU/systemd execution,
commit or diary edit is performed by Codex.
