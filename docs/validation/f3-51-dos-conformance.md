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
