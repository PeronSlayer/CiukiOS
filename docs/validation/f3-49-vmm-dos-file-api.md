# f3-49 research and implementation record

Research date: 2026-10-19. This is host evidence, not a hardware or game result.
The lead approval is recorded in the f3-49 directive's approval addendum.

Sources read before implementation:

- [Microsoft MS-DOS 3.3 Programmer's Reference](https://www.pcjs.org/documents/books/mspl13/msdos/dosref33/),
  handle calls, PSP, sharing, critical errors and 5Ch. DOS handles denote
  descriptions; duplicates share position. Unlock specifies the exact original
  range; locks can extend beyond EOF. This edition cannot establish 6Ch behavior.
- [RBIL release 61, 3Dh](https://fd.lod.bz/rbil/interrup/dos_kernel/213d.html),
  table 01403, rather than DOS7's table 01404: compatibility is a distinct
  mode. Mixed opens are asymmetric; cells C/2 require INT24. Explicit modes
  check both directions. Compatibility reopen permission is PSP dependent.
- [RBIL 5Ch](https://fd.lod.bz/rbil/interrup/dos_kernel/215c.html): duplicates
  retain lock identity; an inherited handle has a different effective PSP.
- [RBIL 6C00h](https://fd.lod.bz/rbil/interrup/dos_kernel/216c00.html): return
  status 1/2/3 for opened/created/replaced; BH bits 5/6 are no-critical/commit.
- [RBIL 59h](https://fd.lod.bz/rbil/interrup/dos_kernel/2159_bx0000.html),
  tables 01680/82/83/84: collision class is **12**, not the proposed class 8
  for create-new error 80. The implementation uses 12/3/2. Other DF5 mappings
  are explicitly Ciuki policy, not measured Microsoft outputs. A sharing
  rejection can expose AX=5 while preserving extended error 32.
- [RBIL INT24](https://fd.lod.bz/rbil/interrup/dos_kernel/24.html): original
  register image follows the callback IRET frame. Retry/Ignore require their
  permission bits; 59h must see the underlying cause before the callback.
- [RBIL SFT](https://fd.lod.bz/rbil/interrup/dos_kernel/2152.html),
  [SDA](https://fd.lod.bz/rbil/interrup/dos_kernel/215d06.html), and
  [DTA](https://fd.lod.bz/rbil/interrup/dos_kernel/214e.html): use DOS4+
  offsets and a 43-byte search result. Mirrors contain guest addresses only.
- [Windows 95 DDK IFSMgr_Ring0_FileIO](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S312A.HTM)
  is accessible. Its context/global handle distinction supports an owned
  translation layer. The missing full DDK hook/reflection/SFT topics were
  searched for again and remain inaccessible through this mirror's search.
  [Microsoft's VxD architecture article](https://techshelps.github.io/MSDN/TECHART/html/msdn_chicvxd.htm)
  is accessible and explicitly explains that Windows95 can serve DOS calls
  in protected mode. No reconstruction of Microsoft's private internals is claimed.
- FreeDOS upstream [dosfns.c](https://github.com/FDOS/kernel/blob/master/kernel/dosfns.c)
  (`DosOpenSft`, `DosOpen`, `DosCloseSft`, `DosRWSft`, `DosSeek`, `DosLockUnlock`),
  [fcbfns.c](https://github.com/FDOS/kernel/blob/master/kernel/fcbfns.c) and
  [inthndlr.c](https://github.com/FDOS/kernel/blob/master/kernel/inthndlr.c)
  were read as behavioral cross-checks. Master is not the 1.3 reference.
  [The ke2043 dispatcher](https://raw.githubusercontent.com/FDOS/kernel/ke2043/kernel/inthndlr.c)
  was accessible; the corresponding raw dosfns/fcbfns URLs were not retrievable.
  No FreeDOS implementation is copied and no unverified master behavior is
  treated as a release oracle. [The official 1.3 package report](https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.3/official/report.html)
  remains the reference package entry point for f3-51.
- [Open Watcom 1.9 DOS/4GW translation](https://open-watcom.github.io/open-watcom-1.9/pguide.html#DOS_4GW__Interrupt_21H_Functions):
  translated real-mode calls must meet the same dispatcher, including chunked
  transfers; a source translation list is not a measured game trace.

Checkout reconciliation: at 91642502 the f3-47 provider has not landed;
`dosvm_disk.c` retains two windows. The approval addendum authorizes the
internal provider seam in this worktree. Existing VFS has 64 handles per table,
128 descriptions, 64 nodes and 16 tables, no record locks or DOS compatibility
mode, indefinitely retried admission and synchronous block transports. These
capacities are actual admission limits, independent of the guest's 232 JFT
slots. Native volumes must retain their existing registry and claims.

Additional reconciliations from the same primary register:

- [RBIL 42h](https://fd.lod.bz/rbil/interrup/dos_kernel/2142.html): a relative
  seek before byte zero can succeed; later I/O fails. Preserve the 32-bit DOS
  position and a negative-position marker rather than applying POSIX seek
  rejection. A valid subsequent seek clears the marker.
- [RBIL 67h](https://fd.lod.bz/rbil/interrup/dos_kernel/2167.html): requests
  up to 20 retain the embedded table. Ciuki's 232 ceiling is personality policy,
  not the count of available VFS descriptions. EXEC inherits only the first
  20 entries and honors the no-inherit bit.
- [RBIL 47h](https://fd.lod.bz/rbil/interrup/dos_kernel/2147.html): successful
  cwd lookup returns AX=0100h. Cwd/default drive are shared by the VM; selected
  PSP, executing PSP and immutable parent are separate state.
- [RBIL 440Bh](https://fd.lod.bz/rbil/interrup/dos_kernel/21440b.html): CX is
  the pause and DX the retry count; zero DX preserves the old count. Defaults
  are 1 and 3. The original CPU-loop pause is represented here by a scheduler
  millisecond per unit, capped by the original request deadline. This is
  explicit Ciuki timing policy, not historical timing equivalence.
- [RBIL 4400h](https://fd.lod.bz/rbil/interrup/dos_kernel/214400.html) and
  [4406h](https://fd.lod.bz/rbil/interrup/dos_kernel/214406.html): disk device
  information retains the drive, written-state and removable bits; regular
  input status is zero at EOF. Device driver pointers and flags are imported
  from checked personality registrations, rather than derived from JFT numbers.
- InDOS is zero inside INT24 while the critical flag is set; the callback
  stack contains the documented original register/return image. Only console
  functions 01h–0Ch and 59h/BX=0 may nest. Ignore is not offered for these file
  requests. A Retry consumes the same request deadline; Abort releases the
  executing child even if AH=50h selected its parent.

Implementation decisions:

- `dos_file.c` owns fixed PSP/JFT/SFT/search/trace/observer tables and a 4 KiB
  transfer buffer. `dos_file_vm.c` adapts V86 and existing DPMI reflection and
  return boundaries. No default guest is replaced and no hook is published
  without a personality binding. The internal C binding is not a packed guest
  protocol: f3-50 must version/marshal/check it explicitly.
- `dos_file_volume.c` mounts native FAT once. The logical block-device flag
  bypasses duplicate outer sector caching; the provider wraps the existing
  disk source/overlay functions. Raw and file operations use the same private
  VFS reservation. Raw directory writes refresh live entries or advance their
  mount generation. No second FAT parser or resident file-data cache is added.
- Global native views use the global registry and per-VM tables. Native
  opens, transfers, truncation and executable snapshots honor common DOS
  sharing/range claims. Native mutations retain their existing FAT durability
  sequence. Compatibility failures retain the asymmetric direct/INT24 result.
- Persistent FAT aliases are reused. Sources without aliases get a stable
  UTF-8 byte-order/id directory sort, real 8.3 reservations first, CP437 case
  conversion, and collision tails 1..999999. The existing read-only directory
  metadata limit becomes 512 entries to admit the required 300-name fixture;
  this does not change VM memory or commitment limits or add a data cache.
- Zero-length record locks currently occupy one exact-unlockable slot without
  denying any bytes. Overflow is rejected and EOF-spanning ranges are allowed.
  The zero-length policy is tested locally but still needs the pinned FreeDOS
  1.3/reference comparison: upstream SHARE source URLs were not retrievable.

Qualification boundary:

The focused production FAT/VFS/bridge harness and the full host kernel suite
are reproducible through `tests/host/dos/run_file_api.sh` and
`scripts/test/host_kernel_tests.sh`. Detailed counts, API coverage and H49
status are in the requested local report `build/codex/f3-49-report.md`.
The harness has fake time/media/guest memory, not an emulated DOS CPU; passing
it does not establish controller latency, live binding, FreeDOS equivalence or
T23/game performance.

**H49-08 remains open.** Initial namespace/volume admission can time out and
skip cancelled reservation tickets, but namespace reacquisition, the disk gate,
cache waits and synchronous ATA/USB transport still have blocking paths in this
checkout. Post-call checkpoints cannot establish the 2000 ms end-to-end bound.
Completing this requires the storage deadline/cancellation integration, and
tests holding the provider/transport and reacquisition paths. This is remaining
f3-49 work; it must not be silently reassigned to f3-50 or reported as passing.
Copied searches also have an unclosed lifetime case: exhausting one copy
currently retires the shared authority record and invalidates the other copies.
The report lists this and the remaining acceptance/fault-injection gaps.

f3-50 owns negotiated binding/ROM identities, driver and DPB registration,
VM-ledger allocation, lifecycle notifications and the FCB/NLS adapters. f3-51
owns the pinned reference/controller, executable COM replay after binding,
games and hardware timing. The lead owns f3-47 integration and source review.
The checkout used here predates the f3-47 cache commits; the source provider seam
is ready for their `disk_read_locked`/`disk_write_locked` implementations, but
the focused harness does not qualify the incoming LRU itself.

## Continuation after f1-83 (2026-10-19)

Amendment 1 supplies a task-scoped request context through the existing f1-83
pool, including controller-worker calldowns and nested private-image VFS access.
Logical FAT reads/writes/flushes use that same request interface. Provider-gate,
cache admission/fill/batch waits and namespace reacquisition observe its original
2000 ms deadline. Queued cancellation unlinks and completes a request even when
an unrelated command is issued; issued buffers remain owned until the existing
controller stop/retirement path completes. ATA/USB changes only consult this
request seam at existing polling/timeout boundaries; register programming and
uncertain-write retry policy are unchanged.

Cancellation is a caller outcome rather than a sticky cache-device failure.
Interrupted mutations retain FAT's existing ordered-publication/read-only
failure policy: a lost allocation is permitted after an uncertain failure;
duplicate ownership or a published short chain is not. Refused durable unmount
remains an error. A quiescent failed private mount can be discarded without I/O;
live tables, native/legacy claims, range locks or operations refuse discard.
Native mounts retain their existing policy. The bridge's first `cleanup_status`
is preserved in the controller's `cleanup_error`, including after successful
reference teardown. Bookkeeping reacquisition must retire its already-published
references under the original short lock; cancellation does not abandon a stack
fill or a VFS mutation guard. Its completion bound depends on the R1 bounded
namespace/cache holder and controller retirement contracts, not on forcibly
freeing a live buffer at the deadline.

An exhausted DTA no longer destroys authority for other copies. Identical
PSP/directory/pattern/mask queries reuse authority while retaining each DTA's
position. Authority is reclaimed at owning PSP exit, VM stop and mount-generation
change; another PSP's exit cannot revoke it. The fixed 32-authority admission
limit remains explicit: different queries can exhaust it until those lifetime
boundaries. Invisible guest copies cannot safely be counted or evicted at EOF.
This is a bounded resource policy, not unlimited copied-search storage.

The cache-only RO directory loops now cooperate during insertion-sort shifts,
duplicate detection, alias collision scans, assignment and lookup. The host
latency test instruments those production loops (512 reverse-order colliding
names), using 1 us per metadata-page traversal, 64 us per maximum-name comparison
and 2 us per pair of node copies. Its measured modeled longest span is 1.5962 ms;
these conservative CPU costs are not physical T23 measurements.

Primary-source reconciliation for the continuation:

- Microsoft [MS-DOS 3.3 reference, 40h/4Eh/4Fh](https://www.pcjs.org/documents/books/mspl13/msdos/dosref33/)
  and [RBIL FindNext](https://fd.lod.bz/rbil/interrup/dos_kernel/214f.html):
  search continuation resides in the DTA; preserve independent copied positions.
- [RBIL 6C00h](https://fd.lod.bz/rbil/interrup/dos_kernel/216c00.html):
  BH commit/no-critical bits retain their existing DOS meanings. Fault tests
  distinguish exact partial counts from errors and exercise real flush failures.
- Microsoft [DDK IOS services](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S2F60.HTM),
  [load groups](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S2F5A.HTM),
  [deadlocks](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S31A7.HTM),
  [IFSMgr FileIO](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S312A.HTM):
  retain packet/handle ownership and completion semantics across calldowns;
  do not claim NTVDM architecture or infer a historical numerical deadline.
- Microsoft [Q223282](https://helparchive.huntertur.net/document/108125) and
  [VxD architecture](https://techshelps.github.io/MSDN/TECHART/html/msdn_chicvxd.htm)
  remain accessible. Complete DDK hook/reflection/SFT topics remain unavailable;
  these two documents do not fill that source-access gap.
- FreeDOS [master dosfns.c](https://raw.githubusercontent.com/FDOS/kernel/master/kernel/dosfns.c)
  (`DosRWSft`, `DosCloseSft`) is accessible as a behavioral cross-check;
  [ke2043 inthndlr.c](https://raw.githubusercontent.com/FDOS/kernel/ke2043/kernel/inthndlr.c)
  is accessible, but the ke2043 dosfns URL remains unavailable. No continuation
  behavior is adopted from unpinned master and no FreeDOS implementation is
  copied. FreeDOS 1.3 executable equivalence remains for f3-51.
- Microsoft's [FAT on-disk specification](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf)
  establishes chains and owning directory entries. Cancellation does not change
  the separately contracted native FAT durability order or add f1-84 features.

The extended focused harness covers all baseline H49 groups plus copied-search
reclamation, initial/reacquired namespace, provider and controller request waits,
cache fill/writeback cancellation, and each observed allocation/I/O point in
create/write/truncate/rename/delete/close/autocommit, including stop injection.
Its read-only remount checks unique ownership and sufficient chain length after
each mutation fault. Full commands and final counts are in the local continuation
report `build/codex/f3-49b-report.md`. The production provider gate is additionally
executed by the DOS VM host harness. F3-50 binding is absent in this checkout;
H49-01 live CPU replay remains explicitly pending for f3-51. No QEMU, image build
or hardware qualification is claimed by this continuation.
