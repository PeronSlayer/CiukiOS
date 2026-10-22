# Windows 98/ME-class resource management

Proposed contract, 2026-10-19. Author: Codex, f0-42 implementer. Claude's
cross-review is recorded below; implementation follows separate directives.
This document adds no code, public header, ABI, allocation change or
hardware qualification.

Authority: [f0-42](../directives/f0-42-windows-98-class-resource-management-audit-and-contract.md)
and the [owner's decision](../../dev_diary/2026-10-19-03-decisioni-modello-win98-me-completo-gate-qemu-sospesi.md).
The [research and gap audit](../validation/f0-42-resource-management-audit.md)
contains source access/provenance, physical records, baseline functions and
24 numbered gaps. Pxx/Eyy references below resolve there.

## Scope and interpretation

The target is complete cooperating resource management: adaptive shared
cache, demand-paged memory with a backing store, layered asynchronous I/O,
file coherence/locking, dynamic thread priorities and bounded interrupt and
deferred work, including DOS VMs. A background worker alone does not satisfy
this contract. A cache fix alone does not complete memory management.

One canonical image detects the hardware defined in
[hardware-baseline.md](hardware-baseline.md). Qualification here covers
128/256/512 MiB profiles; it neither removes the existing 64 MiB scope target
nor expands the current 768 MiB direct map or the eventual memory scope.
The F0–F8 order, POSIX native API, Win32 personality scope, DOS personality,
driver ownership and existing crash-recovery guarantees remain authoritative.
QEMU gates are suspended under the 2026-10-19 owner decision; host evidence,
the canonical build and image-specific physical evidence remain required of
later implementations. This delivery performs none of those builds/runs.

All new numbers in acceptance criteria below are **proposed CiukiOS policy**,
not undocumented Windows constants. Historical observations have their
version and source stated. The audit's research-closure table is a blocking
item for an assertion of exact historical policy parity. The lead's review
below permits mechanism work while historical constants remain open.
Resource Kit findings are resolved by P28–P38 on 2026-10-19 to the extent
stated in the audit; absence of a value in that edition does not verify it.

The detailed authorities are cross-referenced, not rewritten by this draft:
[storage](vfs-storage-contract.md), [boot/memory](boot-memory.md),
[execution ABI](execution-abi.md). Their existing behavior stays in force
until a reviewed subsystem directive reconciles it with this contract.

## Reference model: what is established and what is not

### Cache and file I/O

Windows 98's cache and swap sizes adapt to application demand; MapCache can
execute suitably laid-out code from cached pages. Cache capacity is bounded
by both RAM and system virtual-address space. `MinFileCache`/`MaxFileCache`
are administrator bounds in KiB, not a reason to dedicate all RAM to a cache.
The large-RAM failure in KB253912 is a virtual-address reservation failure;
copying its 800 MiB internal maximum is not an appropriate CiukiOS policy.
[Resource Kit excerpt](https://www.oocities.org/~budallen/98performance.html),
[KB108079](https://jeffpar.github.io/kbarchive/kb/108/Q108079/),
[KB253912](https://www.betaarchive.com/wiki/index.php/Microsoft_KB_Archive/253912).

VCACHE provides page/block lookup, holding, recency and reclamation services
to filesystem clients. VFAT supplies delayed-write semantics and knows file
and metadata dependencies. Calling VCACHE “unified” does not make all raw
block buffers and all file-mapping aliases automatically coherent. MapCache
executable reuse also does not prove general writable mapped-file coherence.
The original Win95 analysis describes held cache blocks and the memory
manager's cooperation; hit/miss, available-memory and fault feedback guide
resizing. [Mitchell, ch. 10–11](https://pdfcoffee.com/inside-the-windows95-file-system-3-pdf-free.html),
[VCMon investigation](https://jacobfilipp.com/DrDobbs/articles/DDJ/1996/9605/9605c/9605c.htm).

| Required historical behavior | Established model | Numerical/version limit |
| --- | --- | --- |
| Sizing triggers and eviction | Demand/pressure feedback; oldest-first LRU recycling for cache fills or memory-manager borrowing, with random/failed recycles also exposed | Resolved by P28/P36 on 2026-10-19: dynamic sizing and recycle roles confirmed; thresholds, cadence and growth chunks not in the Resource Kit. Dirty/held eligibility still requires DDK documentation. |
| Lazy writing | File writes may complete into cache; writeback, explicit flush, close/removal and shutdown are distinct events | Resolved by P29 on 2026-10-19: `BufferIdleTimeout`, `BufferAgeTimeout`, `VolumeIdleTimeout` named without values. No verified universal 98/ME flush interval or dirty percentage; CiukiOS C1 numbers stay explicit policy. |
| Ordering | Ordinary file commit writes to disk before returning; disabling synchronous commits weakens integrity | Resolved by P29 on 2026-10-19: commit role confirmed; `DriveWriteBehind=0` disables write-behind globally. `AsyncFileCommit` encoding and exact VFAT publication/crash ordering not specified in this edition; retain C2 barriers. |
| Read-ahead | Filesystems use VCACHE; CDFS has a separate cache/read-ahead scheme | Resolved by P31 on 2026-10-19: `ReadAheadThreshold` units/default/range/generation rule not in the Resource Kit. CDFS table values cannot supply them. C4 retains byte-valued CiukiOS policy. |
| Removable write-behind | Book documents enabling/disabling write-behind for removable disks (ZIP/JAZ examples) | Resolved by P30 on 2026-10-19: explicit installed default and SE/ME exceptions not stated. Keep CiukiOS write-through pending f1-85 review; safe removal still requires a drain. |
| Coherency | Memory-mapped files/executables use cache; mapped execution can share pages rather than duplicate them | Resolved by P32 on 2026-10-19: sharing role confirmed; writable aliases, truncate and raw-write coherence are not specified. C5 remains a separately tested protocol. |

For interface ancestry see [DDK IFSMgr_Ring0_FileIO](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S312A.HTM):
LFN operations, context-specific record locks, no-cache and privileged
swapper I/O are separate paths. The CiukiOS design below provides their
resource-management roles without copying their calling convention.

### Memory, residency and swap

The VMM reference is demand-paged virtual memory, with reserved addresses,
resident backing, locked pages and file/swap pagers distinguished. The
published Win95 physical-memory model describes global competition and
LRU-style replacement using access history. “Working set” here means an
owner's resident pages and their activity; it does not authorize importing
NT's balance-set manager, trimming intervals or per-process minimum/maximum
guarantees. Demand-zero avoids eagerly clearing untouched committed space;
private writes to shared read-only backing require private ownership (COW).
The exact 98 pager/COW contracts still require P02.
[King, physical/virtual memory](https://ftpmirror.your.org/pub/misc/bitsavers/pdf/microsoft/windows_95/King_Inside_Windows_95_1994.pdf),
[Mitchell, paging chapter](https://pdfcoffee.com/inside-the-windows95-file-system-3-pdf-free.html).

Windows 98's `WIN386.SWP` grows and shrinks with memory demand; there is no
verified universal “1.5× RAM” requirement. KB223294 specifically documents
idle prewrite through VFAT after pending filesystem writes finish.
`ConservativeSwapfileUsage=1` disables that mechanism and restores the older
behavior at a performance cost. It is not the desired default and not an
OOM policy. Growth increments, replacement scan cadence and precise 98/ME
resident-set trimming remain research gaps, not invented constants.
[KB223294](https://www.betaarchive.com/wiki/index.php/Microsoft_KB_Archive/223294),
[Resource Kit excerpt](https://www.oocities.org/~budallen/98performance.html).

Resolved by P33 on 2026-10-19: the full 1998 book confirms dynamic swap
growth/shrink and idle dirty-memory prewrite. The minimum/maximum UI fields
are in kilobytes; `[386Enh]` `MinPagingFileSize` remains supported, without
a stated default or growth increment. `ConservativeSwapfileUsage` is not in
the book; P05 independently documents default 0 for 98, 1 for 95, with 1
disabling async prewrite. No SE/ME constant is inferred from this edition.

The following is a **service-role checklist**, not a claim that the full
Windows 98 VMM service list has been retrieved:

| VMM/DDK service family to check | CiukiOS role |
| --- | --- |
| `_PageAllocate` / page free / linear lock-unlock | Constrained physical backing, pins and release |
| `_PageReserve`, `_PageCommit`, `_PageDecommit`, `_PageAttach` and pager registration/query | Reservations, objects, demand faults, aliases and backing providers |
| Thread creation/blocking, priority adjustment and time-slice services | Ready/wait queues, effective priority, bounded execution |
| Global/VM/thread events, timeout setup/cancellation | Deferred work and generation-safe timers |
| VPICD virtualization, EOI and virtual-interrupt callbacks | Separate physical acknowledgement and guest delivery |
| PageFile services, including `PageFile_Call_Async_Manager` | Nonrecursive swap requests and idle prewrite |

The complete 98 service names, async-safety flags, callback context and
initialization restrictions must be recorded from P02 before implementation.
The existing [DDK timeout correction](https://helparchive.huntertur.net/document/37467)
and [thread-service discussion](https://techshelps.github.io/MSDN/TECHART/html/msdn_chicvxd.htm)
show why a name list alone is insufficient.

### IOS, DMA and request scheduling

IOS has layered requests, device control blocks and asynchronous lifecycle
notifications; port drivers, type-specific drivers and volume trackers have
different jobs. IOP contains the request context, including IOR operation,
buffer, status and completion information. Submission and completion must
have defined ownership even when callers use a synchronous facade.
ESDI_506.PDR is the IDE port-driver example, not a new filesystem cache.
Priority flags do not establish a historical 20 ms I/O guarantee.
[DDK services](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S2F60.HTM),
[load layers](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S2F5A.HTM),
[KB196550](https://www.betaarchive.com/wiki/index.php/Microsoft_KB_Archive/196550).

DMA operates on locked physical buffers or a locked translation/bounce
buffer. PCI IDE scatter-gather PRDs describe regions; each region and the
descriptor table must respect 64 KiB boundaries, with zero PRD byte count
encoding 64 KiB. This does not imply that the entire request is limited to
64 KiB. ISA DMA is separate: 8-bit channel windows and 16-bit word-addressed
windows have different restrictions. CiukiOS already has BM IDE and ISA
audio paths; this contract extends their shared management, not their
qualified hardware scope. [Intel IDE interface](https://pdos.csail.mit.edu/6.1810/2004/readings/hardware/IDE-BusMaster.pdf),
[Creative DMA guide](https://pdos.csail.mit.edu/6.828/2018/readings/hardware/SoundBlaster.pdf).

### Scheduling, timers, interrupts and DOS

The historical Win95 thread model has 32 priority levels. Pietrek reports
class defaults idle 4, normal 7 in background / 9 in foreground, high 13,
realtime 24, ordinary relative offsets −2 through +2, and a nominal 20 ms
slice. He also describes temporary I/O-completion boosts, without specifying
their full decay algorithm. These are versioned observations; exact 98/ME
input, foreground and starvation boost amounts/durations remain open.
[Pietrek, pp. 124–142](https://manualzz.com/doc/46388115/microsoft-windows-system-programming-secrets-manual).
The historical SDK documents dependency-owner priority boosting for
Windows 95. [Priority inversion](https://techshelps.github.io/Win32Api%20Documentation/html/o9bn.3.htm).

Resolved by P34 on 2026-10-19: the Resource Kit confirms preemptive Win32
threads and cooperative Win16 behavior, but the requested foreground/input/
I/O/starvation boost magnitudes, decay and 98/ME quantum are **not in the
Resource Kit**. R2's numerical policies remain CiukiOS choices.

Timer precision, timer-callback delivery, scheduling quantum and maximum
service gap are different measurements. Historical `timeGetTime` documents
1 ms precision for Win95; this is not a 1 ms upper bound on dispatch latency.
Nor does a coarse user timer justify assigning the BIOS's approximately
55 ms cadence to every VMM scheduling/interrupt path.
[SDK timer topic](https://techshelps.github.io/MSDN/MULTIMED/devdoc/live/pdref/mmfunc_2q3p.htm).
VPICD callbacks acknowledge/virtualize physical interrupts; events allow
later work in the appropriate context. Blocking in an event while owning
critical state can stop multitasking. Windows 9x is not a source of a hard
real-time guarantee for arbitrary VxDs or firmware stalls.
[DDK virtual devices](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S22EB.HTM),
[DDK deadlock rules](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S31A7.HTM).

Resolved by P35 on 2026-10-19: the Resource Kit identifies VTD as a timer
virtualization driver and describes input IRQ-to-message queues. It supplies
no VTD/VPICD timing bound or service contract; R1/R3 deadlines and PIT cadence
remain CiukiOS policy, with full DDK service/context research still required.

DOS boxes use V86/PM clients, virtual hardware interrupts, and protected I/O
services; a VM is not itself a compatibility defect. DOS disk/file work must
ultimately participate in IOS/cache ordering. The Windows NT NTVDM
architecture is a different reference and is not adopted here. CiukiOS keeps
CIUKIDOS, private FAT16 VM disks, V86/DPMI and the separately contracted
host-volume bridge. [DDK V86 device interfaces](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S22EB.HTM),
[IFSMgr contexts](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S312A.HTM).

## Acceptance conventions and evidence

Each numbered criterion below requires both a production-code host test and,
where timing/device behavior matters, an image-specific physical record.
Host deterministic time proves scheduling/order invariants, not a Pentium
III's instruction cost. A host test that reimplements only the proposed
policy without exercising production paths is insufficient.

Physical records include source/image SHA-256, board/CPU/BIOS, RAM ledger,
device/transport/mode, workload and duration. Preserve maxima, sample counts,
histograms and errors; averages alone do not pass a maximum-latency gate.
Keep raw elapsed time, charged CPU work, IRQ time, ready wait, blocked I/O,
reschedule delay and firmware-attributed gaps distinct. Dropped evidence or
an uncalibrated clock makes the affected bound unqualified, not zero.

An execution span excludes sleeping and time another task executes; the
end-to-end service deadline includes both. Timer sampling alone cannot prove
a 100 µs interrupt-disabled maximum: explicit entry/exit timestamps and
call-site/owner evidence are required. No report may subtract SMI attribution
from raw audible or reader latency to turn a failure into a pass.

## Target storage and cache contract

### C1: dirty budgets, admission and bounded writeback (S01/S02/S05)

Let `C` be actual cache **data** bytes, excluding descriptor/workspace pages.
Initially propose background dirty threshold `B=min(1 MiB,C/8)`, high
threshold `H=2B`, and clean reserve `R=max(64 KiB,C/8)`. Apply both a global
limit and per-device accounting; independent per-device budgets must not
multiply the aggregate limit. Failed dirty blocks remain accounted/pinned.

The writer becomes runnable when dirty bytes reach B or oldest dirty age
reaches 1000 ms. It selects work within 10 ms on the host reference workload.
An eligible old block must be queued/in flight, or have a named dependency,
error or device-service blocker; resetting its timestamp on each rewrite
must not postpone it forever. Above H, only producers needing new dirty
credits wait until debt falls below B. Credits include not-yet-published
writes, and are returned on failure/cancellation.

Admission waits happen **before** taking VFS volume/file-publication locks.
No `cache_write` throttling wait may retain a reservation that prevents an
unrelated same-volume reader from entering. A mutation reserves its bounded
credit/space/node set, prepares an immutable operation, then publishes or
rolls it back under short locks. A stage that needs disk completion must
allow unrelated file operations while retaining stable references and its
conflict/dependency reservation. Conflicting writes/rename/truncate remain
serialized. This requires a reviewed extension of f1-79, not arbitrary lock
dropping in the existing FAT mutation.

Prefer clean victims; wake the writer and sleep without namespace/volume
ownership when there are only dirty candidates. Normal healthy replay
requires zero synchronous dirty evictions by readers. Any emergency fallback
is bounded, counted and fails the normal-service acceptance if it violates
the read bound. Failed pinned data is never discarded to make a statistic
pass. An unrelated cache hit does not wait for another block's transport.

Background batches contain at most 64 sectors or 32 KiB, whichever is
smaller, and yield after at most 1 ms of CPU work. Sort by LBA only within
one eligible ordering group; release the cache lock around transport and
between bookkeeping slices, retaining per-block busy/version ownership.
Same-block writers cannot modify DMA-visible bytes while they are in flight;
use a reserved immutable snapshot or block them with an event. Re-dirtied
versions stay dirty after an older version completes.

The exact proposed B/H/R defaults need the C4/M1 ledgers before activation;
no current 8 MiB allocation or boot-memory limit changes in this directive.

Historical comparison, resolved by P29/P36 on 2026-10-19: Windows 98 names
buffer/volume idle and buffer age timeouts but supplies no numeric defaults
or dirty-byte budget in this Resource Kit. Keep CiukiOS `B=min(1 MiB,C/8)`,
`H=2B`, `R=max(64 KiB,C/8)` and 1000 ms aging under the lead's f1-82 decision;
they address the measured reader/producer problem rather than implement a
verified Windows constant. Oldest-first cache recycling is documented, but
its dirty-victim and on-disk ordering rules are not; f1-83/f1-84 must preserve
the explicit dependency/durability protocol.

### C2: ordering, errors and removable media (S05/S07/S08)

Use monotonically numbered dependency groups per volume/device generation:
data initialization → detached chain construction → link publication →
directory size/name publication → advisory FSInfo. Retain the existing
operation-specific delete/rename ordering and FAT-copy divergence detection.
Do not replace these with a blanket statement that LBA order is safe.
A barrier captures a finite sequence/version frontier; later writes cannot
extend it forever. Completion means that frontier and its dependencies are
on stable media through a qualified flush or verified disabled device cache.

`fsync`, explicit sync, durable removal and shutdown wait for their required
frontier and propagate sticky per-device errors. Buffered `write` completion
and durable completion are distinguishable. Until ordered asynchronous
publication is reviewed and power-cut tested, retain current synchronous FAT
barriers even if this prevents meeting the throughput/latency target. Report
that gap rather than weakening recovery. No on-disk journal is introduced
implicitly.

Propose write-through completion for removable-media ordinary writes by
default; any later write-behind option requires explicit media policy and
the same safe-remove drain. Resolved by P29/P30 on 2026-10-19: Windows 98
documents a removable write-behind option, without an explicit installed
default; the normal commit API writes before returning, while disabling
synchronous commits risks data loss. CiukiOS keeps write-through for safety
and finite durable barriers; f1-85 owns any later media-policy adoption.
Surprise removal cancels unissued requests, invalidates generation-bound
aliases and records incomplete durability; it never reports a successful
flush of absent media. Issued DMA pins survive until hardware stop/completion
is proved. A newly inserted medium cannot inherit an old completion/error
epoch. Preserve refusal with active handles/cwd/VM/swap dependencies.

Host fault injection must cut every publication stage, reorder only permitted
groups, fail every write/flush, and race removal with dirty/in-flight work.
Existing FAT12/16/32 fsck/mtools comparisons, crash-reboot, boot-log and
per-device-error isolation tests remain green. A failed device must not
revoke unrelated healthy volumes.

### C3: same-volume reader fairness and asynchronous requests (S01/S06)

Use a bounded common request pool and per-controller queues. Proposed
request fields: operation, device/media generation, LBA/byte length,
page-vector/pin token, dependency frontier, priority/deadline, enqueue/start/
completion times, result/bytes, cancellation state and completion event.
Ownership is exactly one of prepared, queued, issued, completed/cancelled;
completion happens once, and only then can the waiter/pin reference retire.
Keep synchronous block/VFS wrappers while internal submission becomes
asynchronous. IRQ completion only records/acknowledges/wakes; no allocation,
filesystem lookup, user copy or general callback under IRQ-off execution.

Prioritize deadline-bearing demand/page-in reads over speculative reads and
bulk background writes. Metadata dependency work can inherit the priority
of a waiting demand request. A bounded active hardware command cannot be
preempted; splitting and maximum command time must be part of admission.
Cap a fast-disk command at 64 KiB initially, with smaller adaptive limits
when measured command time would consume the reader's service budget.
Keep current firmware-selected DMA/PIO fallback and stop-proof rules.
Do not assume a slow 64 KiB USB command can meet a 20 ms same-device bound.
f1-96 (2026-10-22): a controller whose commands cannot meet the budget pins
its admission size with `blk_queue_set_limits` (max and adaptive floor);
the usb-storage queue pins its negotiated READ(10)/WRITE(10) length (64 KiB
on UHCI) instead of ratcheting to the one-page floor, see
[usb-contract.md](usb-contract.md#f1-96-negotiated-64-kib-transfers-uhci-td-ring-pinned-queue-limit-2026-10-22).

Merge only adjacent requests with compatible device generation, priority,
dependency frontier and pin ownership. FIFO/age break ties and prevent
writeback starvation. Under the admitted reference load a queued background
batch begins within 100 ms; a saturated/unresponsive device instead exposes
its queue age, timeout and backpressure. No unbounded queue growth or retry
of an uncertain issued write is allowed.

Host reference replay: a 10 MiB file is copied from a 1 MiB/s source to a
20 MB/s destination while a separate task reads 16 KiB from another file on
that destination every 20 ms. Specify destination seek/command/flush costs
(including a ≤5 ms flush), cold/hot cache, fragmentation, data correctness
and final durability in the fixture. Require maximum arrival-to-completion
reader latency ≤20 ms, zero missed reads, and durable copy throughput within
±10% of 1 MiB/s. The negative control must reproduce ≥100 ms reader delay
with the old policy. Include the real VFS/FAT/cache/driver-adapter paths,
not just a cache-only pair of threads. Repeat with fragmented chains,
all-dirty cache, mixed devices, concurrent fsync, error and cancellation.

On physical hardware repeat copy + MP3 + input + DOS load for at least ten
minutes, reporting total and software-added read waits and throughput against
an isolated run on that exact device. The ≤20 ms target applies to unrelated
reads on the qualified destination; USB source commands, seeks or firmware
pauses exceeding that budget must be reported explicitly. A model pass does
not certify a physical total maximum. A physical miss remains a miss even
when a raw device/firmware delay explains it.

### C4: adaptive cache and read-ahead (S03/S04)

After M1 ledger review, propose total-cache floor 2 MiB, initial allocation
up to 8 MiB, and ceiling `min(32 MiB,U/8)`, where U is measured usable RAM.
All metadata, workspaces, mappings and pinned fills count against that
budget. The memory-pressure reserve takes precedence over cache growth.
These bounds are candidates for 128–512 MiB, not an approval to reserve a
new large system-arena window or change `cache_init` now.

Use 100 ms feedback windows: grow at most 256 KiB/window after two windows
of useful misses with free NORMAL above the high watermark; suppress growth
on pressure, poor prefetch usefulness or excess dirty debt. Reclaim unpinned
clean pages first, up to 256 KiB per ≤1 ms CPU slice, down to the reviewed
floor. Queue eligible dirty pages for writeback; do not synchronously flush
the device in a memory allocator or hold a PMM lock while reclaiming cache.
Pinned/error pages make the unreclaimable fraction explicit. Count shared
mapped-cache frames once, with aliases preventing premature reuse.

Per open stream keep last offset, sequential-run count, prefetch window,
generation, useful/wasted bytes and outstanding requests. After two adjacent
reads propose 16 KiB prefetch, growing on use to at most 64 KiB per stream;
cancel/decay on seek, close, pressure or competing demand. Total speculative
resident bytes ≤C/8. Retain bounded FAT metadata read-ahead, file/volume
extent checks and dirty-cache hits; never read across EOF or media generation.
No second private data cache is introduced. A compatibility setting analogous
to `ReadAheadThreshold` can be specified only after an original reference
fixes its units; CiukiOS's initial knobs have explicit byte units.

Historical comparison, resolved by P28/P31/P36 on 2026-10-19: adaptive
growth/shrink and memory-manager cache borrowing are **confirmed in role**.
The book gives no 100 ms/256 KiB growth policy, 2/8/32 MiB cache bounds,
growth chunk size, or `ReadAheadThreshold` units/default/range/request rule.
Keep these CiukiOS numbers and the two-read/16–64 KiB/C/8 prefetch policy
pending f0-48/f1-85 evidence. The book's separate CDFS cache table is not a
replacement disk-prefetch specification.

P29's role-profile tunables are paths/names rather than data-cache bytes:
Desktop 32/about 677 (8 KB), Mobile 16/about 337 (4 KB), Network server
64/about 2729 (16 KB). Mobile flushing is described as frequent without an
interval. Keep one detected image and measured M1/C4 budgets; f1-84/f1-85
may evaluate bounded path/name caches, rather than equate these profiles to
per-machine images. No profile or historical bound is activated here.

Host traces must show cache growth under useful repeated reads, shrink under
anonymous-memory pressure, zero reclaimed pinned/dirty pages, and bounded
CPU work. Sequential prefetch must reduce device commands; randomized
16 KiB reads must not be delayed by prefetch and waste cannot exceed the
speculation cap. Record steady-state CPU and RAM costs at 128/256/512 MiB.

### C5: one file identity and coherent mappings (S08)

Retain one node/open-description identity, LFN/8.3 alias validation, access/
share checks and byte-range ownership across native, Win32 and later DOS
bridge clients. Add mapping-page identity keyed by volume generation,
stable file identity and file-page index. Subsector block-cache views and
file-page aliases must share backing or have an explicit invalidation/write
protocol; two independent dirty copies of the same bytes are forbidden.

Specify file-backed MAP_SHARED write visibility, private COW, partial pages,
truncate invalidation, dirty-page fsync, close versus mapping lifetime,
rename/unlink identity, external/raw writes and media removal before enabling
them. Host tests must alternate mapped and ordinary writes/reads through two
processes, verify private writes stay private, and exercise truncate/EOF and
in-flight I/O/removal races. Byte-range/share-lock matrix tests cover native,
Win32 and DOS ownership; every close/death releases exactly its locks.

Historical comparison, resolved by P32 on 2026-10-19: Windows 98's mapped
files/executables use cache, and MapCache avoids duplicate copies of many
pages; 4 KB executable-section alignment supports this. C5 shared ownership
is **confirmed in role**, but writable mapping/ordinary-I/O visibility,
truncate and raw-write rules are not specified. Keep the stronger explicit
CiukiOS protocol; f1-84/f0-47 own its implementation and evidence.

## Target memory contract

### M1: accounting, pressure and pins (M01/M05/D01)

Add a single resource ledger with per-zone physical totals/free/reserved,
kernel/heap/stacks/page tables, DMA and device pins, cache data/metadata/
workspace, user/VM private/shared resident pages, commit promises and swap
capacity/used slots. Count a shared PFN once physically; separately count
each mapping and commitment. Reservation, commitment and residency are
different quantities. Publish consistent snapshots without traversing objects
whose lifetime can end outside protection.

Before setting new memory/cache/VM caps, obtain boot, settled desktop,
audio-active, copy-active, maximum-VM and teardown ledgers on 128/256/512 MiB
profiles. E19b's early 523260 KiB snapshot does not replace them. Reconcile
usable PFNs against free + all owner/reservation categories with zero missing
or double-counted pages. Keep DMA-zone headroom independent of NORMAL.
Propose low NORMAL watermark `max(4 MiB,U/32)` and high watermark twice low;
the DMA reserve is selected from measured worst-case qualified-device pins
plus one maximum in-flight admitted request, not an arbitrary percent.

Below low, wake reclaim and stop speculative/cache growth; resume ordinary
growth only above high. Account admission against RAM-backed/pagefile-backed
commit capacity after emergency and pin reserves. Deny an unsatisfiable
request before publishing partial state; do not claim pageable commitment
without reserved backing. OOM while touching already committed memory is a
fault/error-containment case, never permission to return someone else's data.

Add pin/unpin tokens carrying object generation, PFN range and device
constraints (address mask, alignment, segment boundary, transfer direction).
Pin code/stack/buffers needed for IRQ/deadline service before that service.
No reclaimable allocation or page fault is permitted in IRQ/audio-refill
critical work. PMM may not sleep or invoke filesystem reclaim with IF clear.
DMA/bounce buffers stay pinned until completion or proven stop, even during
owner exit. Host tests cover zone exhaustion, fragmented runs, double unpin,
failed admission, aliases and late completion; baseline reservations remain
unallocatable.

### M2: objects, faults, demand-zero and COW (M02/M06/D04)

Extend `aspace`/`ua_extent` with virtual-area/object backing, page index,
permissions, commitment, resident-set links and generation. A page descriptor
tracks PFN/refcount, accessed/dirty epoch, pin count, backing kind/slot,
I/O-in-flight and waiters. Keep object ownership outside a PTE: a nonpresent
PTE is not enough to decide whether it is demand-zero, swapped, mapped,
PROT_NONE, guard or invalid. Shared surfaces/Win32 reserves and existing
protection/fault signal semantics remain distinct.

Commit reserves backing capacity; demand-zero faults materialize a cleared
page only when touched. A shared zero page may be read-only and use a private
zeroed page on write. Private file/anonymous sharing uses PFN references and
write faults; reserve possible COW write commitment before promising it.
Permission, guard, kernel and invalid-address faults do not turn into demand
allocation. Never expose a frame before zeroing or successful verified
page-in. Coalesce faults on one page into one I/O and generation-check after
every sleep. User-copy helpers must pin/fault in pageable ranges before
entering locks that could be required by their pager.

Host tests: untouched committed regions allocate no data PFNs; first read is
zero; write allocates once; two private aliases diverge correctly; shared
aliases remain coherent; protection faults remain faults. Inject failure at
every allocation/page-table stage and race unmap/exit with faults. Free and
commit counts return to baseline after 100 lifecycle cycles. Mapping,
unmapping, scanning and failed-create cleanup must also pass R1, including
the currently unbounded `as_destroy` and `ua` walks.

### M3: global replacement and resident-set trimming (M04/S03)

Use global second-chance/access-history replacement as the proposed
LRU-style mechanism, with per-owner resident/hot/cold statistics and fairness
limits. Sample accessed/dirty bits with correct TLB invalidation; a sampled
clear bit is not proof that a mapping has no users. Reclaim order: unused
speculative cache → clean unpinned file/cache pages → cold private pages with
valid backing. Modified pages enter a bounded writeback queue; frames remain
owned until successful completion and alias invalidation. A clean file page
is discarded/reloaded from its file, not unnecessarily copied into swap.

Scan at most 64 pages or 1 ms CPU work per slice, whichever is first. A
pressure episode records pages scanned/reclaimed, dirty/in-flight/pinned
rejections, owner resident changes and stall/fault counts. Under an admitted
working set that fits after clean-cache reclaim, no anonymous page-out is
needed. Under a working set larger than RAM but within commit backing,
processes continue with bounded reclaim slices and accurate I/O statistics;
this is not a promise of zero disk-induced fault latency. A continuous
streaming copy cannot evict all recently accessed interactive/DOS pages.

Host tests use controlled accessed-bit traces for hot/cold owners, alias
invalidation, fault coalescing, pressure hysteresis and shared cache pages.
Verify progress and no double-free under concurrent write/exit/reclaim. The
algorithm's scan/decay constants are CiukiOS choices, not claimed 98 values.

Historical comparison, resolved by P33/P36 on 2026-10-19: demand paging,
clean discards and memory-manager borrowing from cache are confirmed; no
page-replacement sampling/trim cadence is supplied. Cache LRU/random recycle
counters do not establish the anonymous-page replacement algorithm. Keep
second chance and 64-page/1 ms slices as CiukiOS policy for f0-48 review.

### M4: dynamic backing file and idle prewrite (M03)

Add a backing-store object with fixed page slots, reserved/used/free states,
extent map, outstanding-I/O count, generation and sticky errors. Propose an
initial 16 MiB allocation on the writable fixed FAT system volume, growth in
16 MiB extents before commit admission, and a minimum disk-free safety reserve
of `max(16 MiB,5% of volume capacity)`. Actual activation/minimum/maximum need
the reviewed disk/RAM ledger; a read-only/no-space system reports reduced
commit capacity and remains usable. No swap on removable, optical or
read-only NTFS volumes. No swap-on/swap-off user ABI is implied by this draft.

Allocate and flush file extents on a sleepable manager before making their
slots available. Fault/pageout paths use prevalidated extents and reserved
request/buffer resources, with no ordinary FAT allocation, cache eviction,
namespace lock or recursive fault on the faulting process's locks. Define
lock order in f0-49; assert it in host tests. Pin pager worker code/stack and
its minimum I/O resources. The pagefile remains an active volume dependency
until all live slots and I/O are retired.

When filesystem write queues are idle and free RAM approaches the high
watermark, prewrite cold modified pages without reclaiming their frames
until needed. Cancel/deprioritize speculative page-out when demand I/O arrives;
mandatory reclaim and dirty aging still make progress. This adopts the role
of the 98 idle manager, with explicit CiukiOS scheduling limits. Once idle
for 30 s, free unused trailing 16 MiB extents down to the configured minimum;
never relocate a live slot during truncation. Growth/shrink must not hold
IRQ-off or a volume lock for an unbounded CPU span.

Historical comparison, resolved by P33 on 2026-10-19: Windows 98 dynamic
swap growth/shrink and idle prewrite are **confirmed in role**, not as
16 MiB increments or a 30 s shrink timer. The book gives no numeric
`MinPagingFileSize` default. Keep CiukiOS's initial 16 MiB, 16 MiB extents,
disk-free reserve and 30 s timer pending f0-49 ledger/replay evidence.
Retain P05's prewrite-after-pending-VFAT-writes ordering; the book does not
document `ConservativeSwapfileUsage`, so its Windows 98 default 0 remains
attributed to P05. Do not adopt the disable-prewrite option as a default.

A failed page-out leaves the resident page and its dirty version intact;
no commitment is admitted against failed capacity. A failed page-in returns
a contained unrecoverable backing error to the affected process/VM; it must
never fabricate zeros or panic unrelated tasks. Existing source data on
healthy volumes remains accessible. Swap contents are not trusted across
reboot; start with empty ownership, and zero all newly exposed anonymous
pages. This is not hibernation.

Host evidence covers demand beyond RAM, extent growth/shrink and exhaustion,
idle prewrite versus foreground reads, every I/O failure, exit/removal races,
and a deliberately recursive-allocation negative control. Physical evidence
records page-in/out, working sets, I/O waits and audio/input deadlines at
128/256/512 MiB; no general performance claim follows from a host pager model.

## Target scheduler and real-time contract

### R1: bounded kernel work and interrupts (R01/R02/M06)

Target ≤1 ms between safe points, with no software execution span >2 ms
while preemption is disabled outside IRQ-off paths. Amendment f1-90 (lead,
2026-10-21): the AC'97 interrupt wakes the audio refill worker directly, in
O(1), rather than through `device-work`; other waits keep the deferred path. Audit all lifecycle and
error paths as well as success: VM creation/teardown, page-table/heap/reclaim
walks, FAT chain walks, dirty scans, VGA harvest/exchange, user copies and
display/firmware handoffs. A fixed iteration count requires a conservative
cost model and physical maximum; “calls kpreempt_point somewhere” is not a
bound. No safe point is inserted inside an indivisible guest-emulation or
device-publication transaction. Such transactions must instead be bounded
or redesigned under a separate reviewed directive.

IRQ-disabled **software work** must be ≤100 µs outside identified firmware
execution. Keep shorter existing device budgets where applicable. Top halves
acknowledge/capture into preallocated state and wake deferred work; no cache
scan, filesystem access, formatting, buffer zeroing or mix operation there.
Deferred work takes at most 1 ms before yielding/requeuing and has a 2 ms
maximum service CPU span. Prevent repeated event notifications from turning
one bounded callback into an unbounded chain.

Record raw IRQ-off maximum, charged work, interrupt owner/site, firmware
attribution and unclassified gap counts separately. The T23's 2.1 ms SMI
heuristic does not relax the software budget, erase raw latency, or justify
changing BIOS/SMM policy. Host replay must show ≤100 µs modeled work; physical
qualification needs both that software bound and acceptable end-to-end
service despite measured firmware gaps. Unexpected raw overruns remain open.

Amendment f1-94 (lead, 2026-10-22): after the operating system has taken a
device over from the firmware through its documented handoff, clearing the
chipset SMI enable that belongs to that handoff is part of the handoff, not
a change of BIOS/SMM policy. Today this is exactly one bit: `LEGACY_USB_EN`
(ICH3-M 290716-001 9.8.3.14), cleared once after all three UHCI functions
are owned and their USB_LEGKEY trap/SMI enables are clear (11.1.16). Every
other SMI enable (GBL_SMI_EN, BIOS_EN, SLP_SMI_EN, APMC_EN, SWSMI_TMR_EN,
MCSMI_EN, TCO_EN, PERIODIC_EN) stays with the firmware; a firmware re-arm is
recorded and never fought. The SMI attribution records (`[kernel] smi`,
`smi_coincide`, `smi_regs`, [f1-94](../validation/f1-94-irqoff-blackout.md))
are evidence only: they never subtract firmware time from a software budget.
Reducing our own device traffic that the firmware turns into SMIs (the
ThinkPad EC poll, 1 Hz by default, `ecpoll=0` to stop it) is ordinary driver
policy and needs no further amendment.

Extend `test_kernel_latency.py`/production scheduler and VM lifecycle replays
with long cleanup, fragmented cached chains, all-dirty cache and maximum
task/page counts. Preserve f0-41's 16-page creation points and its negative
110 ms control. For every formerly unbounded site record iteration bound,
host-model worst span, physical maximum and remaining unqualified paths.

### R2: effective priorities, boosts and inversion (R04/R05)

Propose separating base class, base priority, dynamic boost/expiry and
inherited priority; effective priority is their maximum, with an explicit
run-queue reinsert when it changes. Use 32 internal levels as the reference
model: idle 0; ordinary base 7; focused native UI base 9; privileged high
13; kernel device service in the reserved high band. Public POSIX/Win32
priority values remain separately specified by their API contracts.

Propose foreground input boost +2 for 20 ms after actual input delivery to
the focused window's owner thread, capped at 15; repeated input refreshes
expiry rather than adding unbounded boosts. I/O completion gives its blocked
requester +1 for one 10 ms quantum, capped at 15. After a quantum, dynamic
I/O boost decays to base; foreground promotion ends on focus loss, and input
boost expires at its deadline. Spurious wakes/polling do not earn boosts.
A normal ready thread waiting 100 ms receives one bounded dispatch at 15,
then returns to base without changing its configured class. These are
testable CiukiOS proposals, not verified Win98 boost constants.

Priority donation follows actual mutex/request dependencies, including
transitive owners, and is withdrawn/recomputed on unlock, cancellation,
timeout and death. No donation across an arbitrary physical device wait
can make that device faster. Detect cycles and do not bypass file sharing or
durability ordering. Short lock ownership and C1 admission remain necessary;
boosting a 740 ms lock holder is not sufficient.

Retain the 10 ms ordinary quantum and the current 2 ms DOS/audio-sensitive
quantum until replacement evidence exists; do not increase them to the
historical Win95 20 ms value. Device service has explicit per-turn deadlines
and a proposed aggregate budget of at most 4 ms per 10 ms under the admitted
reference load. Budget exhaustion requeues ordinary work; an actual urgent
audio deadline is reported/served, and an unschedulable aggregate workload
fails admission/qualification rather than silently starving audio.
Amendment f1-90 (lead, 2026-10-21): the 4 ms per 10 ms budget is shared by
the device tasks with per-task accounting; only tasks above their max-min
fair share are demoted when it runs out. The audio refill worker is exempt
from the shared budget and has its own 2 ms per 10 ms budget, so device-class
CPU may reach 6 ms per 10 ms; refill overruns are recorded separately
(`audio_refill_overrun`). `device_attribution` keeps the previous all-tasks
demotion as the replay's negative control only.
Focused DOS preference must fit into the CPU time left after device service;
it may use at most 80% of that remainder, preserving desktop/background
progress. This is a proposed reconciliation of the existing 8/10 policy,
requiring measured game/desktop evidence before replacement.

Historical comparison, resolved by P34 on 2026-10-19: boost magnitudes,
decay and 98/ME quantum are **not in the Resource Kit**. Keep the CiukiOS
values above, including +2/20 ms input, +1/one quantum I/O, 100 ms aging and
10/2 ms quanta; f0-45 owns any evidence-based change. P18's Win95 20 ms
observation does not supply a missing 98/ME default.

Host tests cover input/focus changes, I/O completion, boost decay, two-level
inversion, waiter cancellation, device saturation and a CPU-hog VM. Under
admitted load, input-to-UI-ready dispatch ≤10 ms, normal ready wait ≤100 ms,
and all device deadlines/R1 bounds hold together. Report dispatch/wakeup
counts and CPU overhead, not just average frame rate. Physical acceptance
uses the combined C3 workload with continued input and game cadence.

Amendment f1-98 (lead, 2026-10-22): a task holding a drive reservation or
guard, a writer turn or a filesystem lock runs at least at the priority of
its highest waiter (transitive, cycle-refusing, released with the hold;
`[sched] inherit` record), as Windows 9x boosts the owner of a critical
section. The desktop process and the Win32 window server keep a 25 % share
of every millisecond (2.5 ms banked) ahead of the focused VM's preference,
behind device service and the starvation boost; a holder that a desktop
thread waits for borrows that share (`[sched] desktop_share` record). The
80 % cap stays.

### R3: timer and audio deadlines (R03/R06)

Keep PIT divisor 1193, nominal 1 kHz, and calibrated monotonic timestamps.
Timer queue entries carry absolute deadline, callback kind/owner generation,
armed/in-flight/cancelled state and bounded completion; use an ordered queue
or wheel to avoid an unbounded per-tick walk as process count grows. A timer
IRQ only moves due events to bounded work. Cancellation/destruction must
prevent an old callback from touching a reused owner; test wraparound,
rearm, concurrent cancel/expiry, tick loss and firmware pauses.

Historical comparison, resolved by P35 on 2026-10-19: the Resource Kit gives
no VTD/VPICD timing guarantee or PIT divisor. Keep divisor 1193/nominal
1 kHz and the R1/R3 bounds as CiukiOS policy requiring measurement; driver
naming and asynchronous input descriptions do not prove interrupt delivery
latency.

Retain 256-frame audio periods (5.333 ms at 48 kHz), P_DEVICE refill,
≤1 ms work before continuation and 2 ms maximum refill service. Refill wake
to execution target ≤2 ms, measured separately from service CPU time. Keep
the f4-32 nominal eight-period VM lead and its adaptive recovery unchanged
until a separately reviewed audio directive. At 48 kHz its current nominal
2352-frame total is 49 ms; 28 transport periods alone cost 149.333 ms.
At 44.1 kHz the same 2352 frames cost 53.333 ms: ≤50 ms cannot be asserted
for that rate without a separate policy change.

The combined physical replay must show no halt/underrun/deadline miss and no
`late-grow` caused by VM creation, cleanup or copying after the relevant fixes.
Record maximum refill gap and actual `latency_frames`; a larger silent lead
does not pass a nominal interactive-latency gate. A 134 ms no-service gap
and ≤50 ms FIFO output latency cannot both be guaranteed by extra buffering.
The proposed resolution is to remove software stretches and retain honest
adaptive recovery for exceptional gaps; any tighter requirement under
firmware pauses needs lead review of the audio timeline/hardware strategy.

## Target DOS integration contract

### D1: pageable VM objects and protected I/O (D01–D04)

Do not raise `DOSVM_RESIDENT_CAP`, change headroom or switch the existing
eager admission until M1–M4 and the physical ledger justify it. Later split
VM address quota, commitment, residency and pinned device/model pages.
XMS/EMS/DPMI handles point to memory objects, not necessarily permanently
resident PFNs; alternate aliases share page identity and correct dirty/access
history. Pin before virtual DMA/model/device work and unpin only after the
VM/device completion boundary. Trap/fault handling may block only at a
published safe continuation, never halfway through emulating an instruction.

Route private FAT16 outer-image reads and future host-volume bridge calls
through the same queued storage/cache framework. Preserve disk seek+transfer
atomicity or replace it with explicit-offset requests under review. Do not
flush/drop the private overlay as if it were ordinary cache debt: its
1024-row copy-on-write index is part of the VM's disk semantics and its
sector bytes are already stored in an outer file. Account those index pages
and the two fixed 64 KiB immutable-base read windows separately. Replace the
duplicate read windows with shared file-page references once C5 can preserve
their batching/lookup benefit; they must participate in global reclaim until
then. Making the index pageable requires its ownership, capacity and reset
semantics to be specified. No private-image/host-volume double mounting with
incoherent caches is permitted.

Keep virtual PIT/SB DMA advancement, pending IRQ delivery, VIF/interrupt
shadow and exact host-saved ISR returns distinct from host scheduling.
Preserve f3-45, including flat selectors/private stacks and late IRQRET.
Background/idle VMs sleep; foreground preference is bounded by R2. Page-in
or disk waits must not stop the desktop or physical audio timeline. A guest
with IF clear may delay its own interrupt; record that separately from
host-ready wait and do not force a guest ISR to hide host scheduling debt.

Host acceptance: maximum-quota admission and failure/exit return all
commit/resident/pin/swap/overlay counts; page faults through EMS/XMS/DPMI
aliases preserve data; terminate with I/O/IRQ pending without stale
completion. Replay DOS/4GW, DOOM's flat-stack ISR case, guest IF-off periods,
disk traffic and host audio together. Physical acceptance records DOOM/
Wolf3D/Quake creation, play and close alongside C3 for at least ten minutes,
with guest IRQ delivery/IF-clear time, host refill and input/storage maxima.
This does not add CD audio or change the approved DOS compatibility matrix.

## Interfaces and lock/lifetime changes to specify before coding

Names below are proposed internal interfaces, not declarations or ABI
changes in this delivery. Each implementation directive owns its final
header layout and error semantics.

| Current area | Data structure / interface addition | Ownership and lock boundary |
| --- | --- | --- |
| `fs/cache.h`, `cache.c` | Dirty/clean lists, byte credits, per-device age/frontier, immutable in-flight version; reserve/release credits, bounded writeback, reclaim/grow hooks | Credits before volume locks; cache lock only for metadata; stable block refs across transport; failed dirty data retained |
| `fs/blkdev.h`, drivers | `io_request`, `io_queue`, `io_pin_vector`; submit/cancel/wait/complete and ordered flush frontier | Controller queue owns issued request; exactly-once completion and media generation; no allocation in IRQ |
| `fs/vfs.c`, `fat.c`, file descriptions | File/range conflict reservations, staged FAT publication/dependency groups, mapping identity and byte-range locks | Namespace publication → ordered conflict reservations → short cache bookkeeping; never wait for credits under a reader-excluding volume lock |
| `core/pmm.c`, `mm.h`, memory owners | Resource ledger, page descriptors, pin tokens, pressure notification and reclaim results | PMM locks only protect PFN metadata; no filesystem entry or sleep under IRQ-off |
| `core/vmm.c`, `proc/uaddr.c`, fault/user-copy paths | Backing objects/areas, page-fault lookup/materialize, COW/refcounts, alias invalidation and resident sets | Object refs survive sleeps; validate generation before publication; lock order excludes pager recursion |
| New backing-store manager / fixed-volume adapter | Extent/slot map, commit capacity, preallocated fault requests, idle prewrite/shrink | Extent allocation outside fault path; no dirty-cache eviction to service pager allocation |
| `core/task.c`, `sync.c`, timer/IRQ paths | Base/effective priority, boosts/expiry/donation, deadline queue and bounded deferred work | Reinsert runnable owners on priority change; no callbacks or filesystem calls under run-queue/IRQ locks |
| `vm/dosvm*`, DPMI/XMS/EMS integration | VM commitment/residency/pins, generation-safe continuations, explicit-offset disk requests | Preserve indivisible monitor transactions and exact ISR contexts; device pins outlive VM exit when required |

Lock ranks, cancellation, timeout, allocation-failure and teardown paths must
be reviewed together with each interface. Any later public header or ABI
change requires `python3 scripts/gen_api_reference.py` and `make CAP= apps`
after that change, in addition to its host tests.

## Proposed implementation directives and dependencies

Identifiers are proposed reservations, not directives issued by this draft;
the lead controls scope/model, cross-review, integration and final numbering.
f1-82 already exists and continues under the owner's decision. Sizes include
host tests and documentation: M≈3–5, L≈6–10, XL≈11–20 implementer days;
hardware/debug time is additional and the estimates are not delivery dates.

| Order | Directive / size | Scope and acceptance | Prerequisites / host evidence |
| --- | --- | --- | --- |
| 1 | **f1-82**, existing, L | C1 dirty budgets/clean eviction and C3 reader replay; separate volume wait, cache wait and FAT barrier costs | Lead reconciles current transport unlocking and credit waits outside volume reservations; retain synchronous durability until staged publication exists. Complete host FS + kernel gates. |
| 2 | **f0-43**, M | M1 ledger, commitment categories, pins/pressure instrumentation; no cap increase | Source closure for memory services; host ledger/zone/pin rollback; lead collects physical 128/256/512 MiB ledger. |
| 3 | **f0-44**, M | R1/M06 bounded VM/UA teardown, allocation and transaction/chain audit | f0-43 counters; latency + maximum-VM failure lifecycle/audio replay; storage authority for chain walks expressly included. |
| 4 | **f0-45**, L | R2 priority/boost/donation/fairness with current DOS/audio constraints | 98 scheduler source closure; production scheduler/sync/latency + audio/DOS host replays. |
| 5 | **f0-46**, L | R1/R3 IRQ/deferred budgets, deadline timers, raw/firmware attribution | f0-44/45; IRQ/timer/driver lifetime and audio host suites; physical raw/work/refill record. |
| 6 | **f1-83**, L | C3 common async requests, controller fairness, page-vector DMA and synchronous adapters | f0-43/45/46, f1-82; ATA/USB/FS fault/removal/queue tests; no new controller activation scope. |
| 7 | **f1-84**, XL | C2/C5 staged FAT publication, finite barriers, file/range locks and coherent cache identity | f1-83; original IFSMgr/VFAT ordering source closure; full fsck/mtools/power-cut/bootlog matrix before reducing any barrier. |
| 8 | **f1-85**, M | C4 adaptive file read-ahead, generation cancellation, removable policy | f1-84 and measured f0-43 ledger; historical read-ahead/removable source closure; sequential/random/pressure FS replays. |
| 9 | **f0-47**, XL | M2 demand-zero/COW/backing objects and fault-safe user copies; C5 mapped-file integration | f0-43/44, f1-84; full 98 VMM service/context research; uaddr/Win32/protection/fault/alias/lifecycle tests, no ABI changes without regeneration/apps. |
| 10 | **f0-48**, L | M3 global replacement and C4 dynamic cache growth/shrink | f0-47, f1-85; deterministic accessed/dirty traces, pressure/pin/coherence tests and physical ledger/latency. |
| 11 | **f0-49**, XL | M4 backing extents, swap page-in/out, commit backing, idle prewrite/shrink | f0-48, f1-83/84; nonrecursive pager/fault-injection/reclaim tests and fixed-volume physical qualification. |
| 12 | **f3-46**, XL | D1 DOS pageable/alias objects, disk queue/bridge integration and combined resource gate | All preceding dependencies; DOS/4GW/ISR/XMS/EMS/DPMI/audio/FS host matrix and combined image-specific physical record. |

Sizes: three M, five L, four XL. Cross-subsystem steps are sequenced because
one writer owns the implementation worktree and paging must not be introduced
on top of an unbounded, recursively allocating storage path. This sequence
does not reorder the owner's F-steps or claim they have closed.

Future implementers run host gates one at a time at `nice -n 19`, including
`ASAN_OPTIONS=detect_leaks=0 nice -n 19 bash scripts/test/host_kernel_tests.sh`,
`ASAN_OPTIONS=detect_leaks=0 nice -n 19 bash scripts/test/host_fs_tests.sh`
for storage changes (all groups/fsck/mtools comparisons), and
`ASAN_OPTIONS=detect_leaks=0 nice -n 19 bash tests/host/audio/run.sh`
for deadline/VM integration. Add production host harness cases in those
existing groups rather than one-off committed diagnosis scripts. Kernel
compilation and canonical image/physical qualification are assigned by each
subsequent directive and the lead; no QEMU gate is resumed implicitly.

## Deliberate differences and lead decisions

| Historical feature or conflict | Proposed CiukiOS resolution / approval boundary |
| --- | --- |
| Cooperative Win16 scheduling and shared Win16/global critical sections | Not adopted: native 32-bit processes and the POSIX/Win32 personalities keep their existing isolation model. Compatibility scheduling is not a reason to serialize unrelated kernel I/O. |
| VxD binary loading, INT20 service ABI and arbitrary third-party ring-0 code | Not adopted: use typed CiukiOS kernel interfaces and qualified ownership/lifetime rules. Functional resource-management roles are adopted; binary driver compatibility is outside scope. |
| System-arena exhaustion / permissive historical protection | Not reproduced. Bound cache virtual and physical reservations and retain protection/fault containment. Owner requested complete management, not historical defects. |
| VREDIR/VSERVER redirector/server caching | Out of this local-resource contract. F6 networking remains in its established step; no SMB client/server parity, network cache coherency or NT redirector behavior is implied. |
| 20 ms historical Win95 slice versus 2 ms audio/VM service | Keep 10 ms ordinary / current 2 ms sensitive policy and prove response deadlines. Exact 98 slice research remains open; changing to 20 ms merely for numerical resemblance would conflict with measured requirements. |
| 100 µs IRQ-off software target versus T23 raw 2.1 ms pauses | Bound driver work, retain raw/attributed/unclassified metrics; no firmware takeover. Lead must judge physical end-to-end qualification; attribution alone cannot pass it. |
| Eight-period nominal latency versus a 134 ms service gap; ≤50 ms at 44.1 kHz | Fix scheduling/storage stalls; preserve current recovery and report actual latency. A new audio/timeline/rate policy needs a separate reviewed directive; no lead/owner audio decision is changed here. |
| 1 s dirty age / 12.5% budget versus unverified historical numeric policy | Resolved by P29 on 2026-10-19: Resource Kit names delay tunables without values or a dirty budget. Keep accepted CiukiOS policy driven by E82/f1-82; no conflicting numeric default established. |
| f1-82 cache-only fix versus same-volume FAT barriers | Instrument and retain f1-78 transport unlocking; credits/staged publication must not retain a reader-excluding reservation across waits. Keep integrity even if the first stage cannot meet C3; f1-84 supplies the ordering redesign. |
| Full pager versus existing eager VM admission and fixed caps | Preserve caps/headroom now. Change commitment/residency only after ledger, pager and alias evidence; do not infer free RAM from an early boot count. |
| Removable write-through proposal versus unresolved 98 write-behind default | Resolved by P30 on 2026-10-19: option documented, installed default unstated. Keep conservative write-through under the lead's decision; f1-85 owns any reviewed write-behind adoption and safe drain. |

Acceptance of this draft requires resolving the audit's source gaps and these
conflicts in the lead's cross-review. Completion of the later resource work
requires every applicable C/M/R/D criterion on the same integrated image;
neither this document nor a passing isolated host model constitutes that
completion.

## Lead cross-review (Claude, 2026-10-19)

Decision: the contract is accepted as the binding design for the owner's
requirement of 2026-10-19 (dev diary 2026-10-19-03 and 2026-10-19-04), with
these resolutions of the conflicts table and of the research closure:

1. Every number in the acceptance criteria is CiukiOS policy until a primary
   source states the Windows 98/ME value; an implementation directive may
   change a number only with the measured evidence that motivates it. The
   seven open research questions stay open in the audit; the lead asks the
   owner for the original Windows 98 DDK and Resource Kit media. No directive
   waits for that closure: mechanisms are implemented now, constants are
   revisited when the sources arrive.
2. Not adopted from Windows 98, as proposed: cooperative Win16 scheduling,
   VxD binary loading and the INT 20h service ABI, system-arena exhaustion
   and permissive protection, VREDIR/VSERVER caching.
3. Quanta: 10 ms ordinary and 2 ms sensitive stay (R2); the historical 20 ms
   slice is not adopted. IRQ-off software target 100 µs stays, firmware
   pauses are attributed, never hidden. Audio nominal lead (f4-32) and the
   timeline policy (f4-35) stay; a change needs its own reviewed directive.
4. Dirty budget 12.5 % / 1 s age (C1) is accepted as the f1-82 policy with
   its host replay; the same-volume reader fairness (C3) and staged FAT
   publication (C2/C5) follow in f1-83/f1-84 as sequenced.
5. Removable media: write-through pending source closure; no behaviour
   change until f1-85.
6. Order and parallelism: f1-82 (in progress) → f0-43 and f0-45 in parallel
   (disjoint files) → f0-44 → f0-46 → f1-83 → f1-84 → f1-85 → f0-47 →
   f0-48 → f0-49 → f3-46. Each step lands on `main` after host suites and a
   canonical build; physical acceptance per image on the T23 per the owner's
   current process (QEMU gates suspended).

Research addendum, f0-42b: resolved by P28–P38 on 2026-10-19 as recorded
in the audit's seven-row table. The supplied book closes the full Resource
Kit reading gap, not the missing DDK/supplement/literature or SE/ME gaps.
No accepted number or lead decision above is superseded by this research.
