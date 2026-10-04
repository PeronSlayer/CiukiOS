# CN32 private page mapping, 2 October 2026

The first synchronous CN32 process boundary uses a separate IA-32 page
directory for each invocation. This document records the implementation
decision and its limits; assembling the module alone is not a runtime pass.

## Primary references and repository check

- [Intel 80386 Programmer's Reference Manual, section 6.4](https://pdos.csail.mit.edu/6.828/2010/readings/i386/s06_04.htm): CPL3 access succeeds only when both the page-directory and page-table entries grant user access. A not-present PTE provides a guard page. The effective R/W permissions also depend on both levels.
- [Pinned Jemm/JLOAD VMM.ASM](https://github.com/Baron-von-Riedesel/Jemm/blob/e96bb6bb80cbdc3e6585544db79a6b133a5566dd/Tools/JLOAD/VMM.ASM): `_PageReserve` obtains supervisor linear pages from the JLOAD system range, `_PageCommit` backs them, and the map window at `FF800000h` exposes their PTEs. JLOAD's page directory is mapped at `F8001000h`, as defined in [JSYSTEM.INC](https://github.com/Baron-von-Riedesel/Jemm/blob/e96bb6bb80cbdc3e6585544db79a6b133a5566dd/Include/JSYSTEM.INC).
- In this repository, `src/vm/session_native_pages.inc` already commits zeroed Jemm pages and tracks a generation-tagged handle and owner. `src/vm/session_jlm.asm` checks the Jemm owner CR3 before dispatch. The owner page directory maps DOS and Jemm; giving it directly to a native ring-3 task would expose legacy user mappings.

## Mapping and ownership decision

`VM_OP_NATIVE_RUN` accepts a complete CN32 v1 image in the system V86 VM's
conventional block. The service checks its segment limit, every backing PTE,
header arithmetic and CRC-32 before allocating process pages. Each process
receives its own zero-filled Jemm allocations for page directory, page table,
code, data and stack; the image bytes are copied before entering ring 3.

The new directory copies the owner directory with U/S cleared in every
inherited PDE. PDE 256 is required to be vacant in the owner directory and is
replaced with a private user page table. Code starts at `40001000h` (user,
read-only), data at `40020000h` and stack at `40040000h` (user, writable).
The intervening PTEs stay not present. Jemm, the native gate, its IDT/TSS,
the kernel stack and the source DOS block remain supervisor-only. Each page
backing the user regions is derived from the corresponding fixed, committed
supervisor mapping; no physical frame is selected outside Jemm's allocator.

On return from normal exit, fault, exhausted instruction budget or setup
failure, the monitor calls `native_page_release_owner` for the invocation's
owner ID and checks the global live-record count against its entry value.
Failed frees remain recorded and block CVSESSION unload. The current entry
boundary permits at most 256 user instructions per synchronous invocation;
it is a bounded execution gate, not an asynchronous process scheduler.
The V86 return includes the native exit status in EBP after a normal exit;
faults return the exception error code there, with the vector in ESI and
the instruction count in EDI.

## Validation state

The CVSESSION and native entry objects assembled and linked with the pinned
JWasm/JWlink toolchain with no warnings or errors. A disposable full HDD image
with the updated CVSESSION ran `NATIVE.COM` twice in sequence: both CN32 sample
processes reported the expected two markers and returned to the DOS prompt.
`NATPAGE.COM` then passed its eight owner, zeroing and release probes in the
same boot. The report and serial trace are in
`build/tests/native-sequential-paging-20261002/`; the canonical source image
retained SHA-256 `6eaa63403525526491408e4c15a66561e8d3169be7fc7dbafdb8e05b6e52896a`.

A second disposable QEMU gate at 256 MiB ran `NWRITE.N32`, which wrote a
marker at `ESI+100h` beyond its initialized four data bytes, and then ran
`NREAD.N32` at the same virtual address. The reader observed zero; it would
exit with status 77 on stale data. Distinct successful exit statuses 3 and 4
prove that the launcher loaded the two requested images. The subsequent
`NATPAGE.COM` owner probe passed and both commands returned to the DOS prompt.
See `build/tests/native-isolation-distinct-20261002/report.json` and its serial
log; the canonical image hash was unchanged. This observes a zeroed new
process mapping at the same virtual address; it does not prove that Jemm
reused the same physical frame or that two processes ran at once.

These runs prove repeated normal invocation and a zeroed private mapping for
the bounded sample. A separate native fault gate was run by the integration
owner; its details are recorded in the native entry validation document. Two
simultaneous native processes and an asynchronous scheduler were not
implemented at the time of these runs.

## Bounded continuation under validation

[Intel's 80386 debug exception description](https://pdos.csail.mit.edu/6.828/2004/readings/i386/s12_03.htm)
says the single-step trap is raised after a completed instruction and saves
the next instruction pointer. Its [interrupt procedure rules](https://pdos.csail.mit.edu/6.828/2008/readings/i386/s09_06.htm)
require the entry to save and later restore flags through IRET. These rules
permit a bounded ring-3 quantum to save a post-instruction user frame and
resume it, provided the kernel owns that frame, recreates valid privilege
descriptors and keeps the process's private pages until terminal exit. The
pinned Jemm Host Scheduler Profile return-to-V86 callback is already used by
`jlm_poll` for audio, VM switching and owed timer ticks; the process manager
may run one native quantum there only with the owner CR3, outside DOS/FAT
critical sections and after urgent audio work.

The new `native_entry_step` ABI retains a 136-byte trusted context across
quantum yields. The manager keeps four generation-tagged process slots and
their owner-tagged pages, advances one ready slot per physical tick in round
robin order, and releases only that slot's pages on exit, fault or explicit
stop. `START`, `STATE` and `STOP` use separate V86 operations; the old
`NATIVE_RUN` service and its 60-byte context remain available. A stale handle
cannot access a reused slot. The source compiled and linked with the patched
Jemm/JLOAD; two-live-process execution is still awaiting its guest fixture.

The preexisting one-shot path passed a new disposable 128 MiB QEMU regression
with the continuation module and matching patched Jemm/JLOAD: the sample,
nine supervisor/readonly/I/O/guard/budget/FPU/ABI/exit cases, reentry after
every case and the page allocator probe. See
`build/tests/native-cont-old-abi-20261002/report.json`; the canonical image
SHA-256 remained `6eaa63403525526491408e4c15a66561e8d3169be7fc7dbafdb8e05b6e52896a`.
