# Boot and memory contract

Contract 2 of 7 required by decision D11. Author: Claude (lead). Reviewer:
Codex. Status: approved after the Claude–Codex cross-review, 2026-10-09.

## Memory objects (M2, f0-47)

Research before implementation (2026-10-19): [Mitchell, chapter 10,
pp. 213–240](https://pdfcoffee.com/inside-the-windows95-file-system-3-pdf-free.html)
describes commitment associating pages with a pager, first-touch virgin-in,
zero-initializing pagers and separate copy-on-write mapped-file behavior.
[Microsoft DDK, Pageable VxDs](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S31A5.HTM)
and [Deadlocks](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S31A7.HTM)
require stable resident metadata and prohibit depending on IRQ exclusion to
prevent paging preemption. [Intel SDM, volume 3A, §§4.6–4.10](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
defines present/write/user/accessed/dirty bits, #PF error bits and invalidation.
The original King link (P21) was unavailable; a follow-up read of the indexed
[original scan, p. 121](https://vtda.org/books/Computing/OperatingSystems/Inside_Windows_95_Adrian_King_1994.pdf)
confirms that locked kernel pages are distinct from RAM contested by application,
cache and DMA allocations. No Windows 95 numerical budget is imported. P02's full Windows 98 VMM service reference remains unavailable;
the accessible inherited DDK context rules above are the evidence used here.
The Resource Kit closure in f0-42 confirms demand paging but supplies no
additional commitment/COW service contract. These mechanisms implement M2,
not an assertion of identical undocumented Windows 98 internals.

Decision: virtual areas retain object identity, page offset, generation,
permissions and commitment independently of PTEs. Anonymous commitment reserves
RAM capacity before publication; pagefile capacity remains zero. Read faults
share a read-only zero frame; stores install a cleared private frame. Private
aliases reserve their full potential write commitment before sharing PFNs;
shared aliases retain one object. Frames are initialized before any user PTE
publication. Fault preparation precedes user-copy assembly and lock-protected
I/O. Existing externally owned surfaces and Win32 section backing remain
separate adapters; native file mmap stays unsupported, with the C5 file/page
identity reserved in the object representation. M3 below adds replacement;
disk paging remains deferred to f0-49. Metadata and release walks service the scheduler in
bounded slices; no test-only production behavior is introduced.

Each PFN descriptor is 24 bytes. Since f0-48 the boot reservation covers
E820's highest RAM endpoint rounded up to a 4 MiB direct-map granule,
clamped to the existing 768 MiB ceiling, without a hotplug reserve. Holes
retain indices but never become allocatable. The array occupies the first
contiguous unreserved NORMAL run after the kernel image; it costs
768/1536/3072 KiB at 128/256/512 MiB (4608 KiB at the ceiling), charged to
`MM_PFN_DESCRIPTORS`. A three-table bootstrap window makes this NORMAL
reservation accessible before `vmm_init` installs the complete direct map.
The `[mm] PFN descriptors` boot record gives entries, bytes and location.
An unavailable reservation leaves allocation disabled rather than exposing
untracked RAM. Its entry density is 6 KiB per represented MiB. Object slots use 64-entry
small-heap blocks, indexed by 32 buckets, with resident-set index links;
large reservations never require an oversized small-heap allocation.
Object ownership serializes synchronous fault preparation across aliases;
waiters revalidate area/object generations after every sleep. No filesystem
page-in implementation is added by the file placeholder. Section COW credits
now reserve commitment without preallocating private data frames.

## Global replacement and resident activity (M3, f0-48)

Research, 2026-10-19: P21's original King URL did not fetch. The indexed
[original scan, p. 53](https://www.bitsavers.org/pdf/microsoft/windows_95/King_-_Inside_Windows_95_1994.pdf)
explicitly describes A/D bits and access-history/LRU replacement; its example
interval is not a Windows 98 policy value. [P25, Russinovich/Cogswell](https://jacobfilipp.com/DrDobbs/articles/DDJ/1996/9605/9605c/9605c.htm)
describes hit/miss, available-memory and fault feedback for cache resizing.
The locally supplied Microsoft Windows 98 Resource Kit (1998), P33/P36,
chapter 26, sections Optimizing File System Performance and Tracking
Performance with System Monitor, confirms cache shrink under paging, idle
modified-page prewrite and oldest-first cache borrowing. It supplies no
replacement cadence. The primary media excerpts read are `wrkc26_3.htm`,
`wrkc26_4.htm`, `wrkc26_9.htm` (table 26.7) and `wrkc28_4.htm`; their provenance
and public source register are in the [audit](../validation/f0-42-resource-management-audit.md).
[Intel SDM volume 3A, paging-structure invalidation](https://cdrdv2-public.intel.com/835781/325462-sdm-vol-1-2abcd-3abcd-4.pdf)
requires invalidation after clearing A/D; otherwise later references need
not set those bits again. These findings support the mechanism below,
without importing NT working-set guarantees or undocumented 98 constants.

`core/reclaim.c` is a NORMAL-priority bounded worker woken by M1's pressure
transition. It preserves the low/high hysteresis and stops strictly above
high. Each slice dispatches at most 64 preflights, including absent PDEs,
PTE holes, free PFNs and cursor advances. A task-CPU clock stops dispatch
at 500 us, reserving 500 us for the final bounded step and accounting.
An uncalibrated clock permits only one preflight per slice.
Each pressure slice is separated by a 10 ms sleep; idle workers wait on the
pressure condition with a one-second safety timeout. These are **CiukiOS
policy constants**, requiring physical CPU/latency qualification.

Order is a full unused/speculative-cache pass, then a clean unpinned cache
pass through the C4 one-frame hook, then an incremental global alias sample,
PFN activity decay and private resident-set selection. The cache hook does
no transport I/O, retains dirty/busy/pinned/error pages and the C4 floor,
reuses C4's stable descriptor cursor, and reclaims empty metadata in separate
constant-time unlink steps. Hash removal continues after at most 32 link
comparisons per preflight, including adverse collision chains; an intervening
writer forces the whole-frame eligibility check to run again. Stable pins
use the PFN pin count directly; cache preflights defer during pin-descriptor
publication/retirement, and exclude a new transition through final free.
This keeps scattered pin-vector walks out of a reclaim preflight.
It counts each data frame once irrespective of block hits. Unregister drains
an already-issued preflight before cache destruction. C4's ordinary growth
and stream cancellation remain with its existing controller; pressure shrink
is delegated to the global worker when linked.

Address spaces form an intrusive registry; teardown repairs its sampling
cursor before releasing tables. One sample clears PTE A/D and invalidates
the active UP translation with INVLPG. Inactive user spaces have no global
PTEs and reload CR3 before execution. All aliases contribute to one PFN's
activity. A touch earns two global decay passes; a private resident node
then receives two selection second chances before becoming eligible.
Software faults also mark access; dirty epochs remain sticky. Ledger hot
and cold counts partition each physical owner's resident count. Process
allocation identities limit queue admission to eight pages per owner per
slice; recently sampled hot pages are always spared. DOS pools remain with
their existing owner/pins and are not made pageable by this change.
f3-46 (2026-10-20) makes the default DOS pool pageable through its own
provider (`vm/dosvm_pool.c`, see the DOS/DPMI contract's "F3-46 pageable
pool"), and restricts sampling: A/D bits are cleared and folded only for
unpinned frames of `MM_USER_PRIVATE`, `MM_USER_SHARED` and `MM_VM_POOL`.
Device, kernel and VM-private translations (the DOS VGA window) and pinned
frames (the DOS VBE buffer) keep their bits for their owners' dirty harvest.

The 32-entry modified-page interface carries non-reused tickets, PFN and
object generation. Only a registered valid backing provider can enter it;
f0-49 supplies the production private provider (see M4 below). The provider freezes mutable aliases,
holds a frame/object reference and completes bounded alias revocation before
the queue pin/reference is released. Partial completion keeps ownership;
failed or redirtied versions preserve dirty state; stale/duplicate completion
cannot free a reused frame. File reload and backing I/O belong to f0-49.
A cleared A bit alone never permits page reuse.

Pressure episodes retain scanned/reclaimed and dirty/in-flight/pinned counts,
faults/stalls, and per-owner resident deltas. One protected summary per
second covers ongoing pressure and coalesces rapid completed episodes; exact
owner deltas and physical resident/hot/cold counters accompany the 30-second ledger. Reporting runs
in cpustat context, never in the allocator or IRQ. Host traces and slice
CPU measurements are in `build/codex/f0-48-report.md`; no physical or QEMU
qualification is claimed here.

## Backing store and idle prewrite (M4, f0-49)

Research, 2026-10-19 (sources registered in the
[audit](../validation/f0-42-resource-management-audit.md)): P33 (Windows 98
Resource Kit ch. 26/28) confirms dynamic swap growth/shrink, demand paging
and idle prewrite of modified memory, without growth increments or a shrink
timer. P05 ([KB Q223294](https://www.betaarchive.com/wiki/index.php/Microsoft_KB_Archive/223294);
direct fetch returns 403 again, the indexed text was read for the audit)
places the asynchronous prewrite in VFAT's idle callback after pending VFAT
writes drain; `ConservativeSwapfileUsage` defaults to 0 on Windows 98 (1
disables the prewrite). P10 ([IFSMgr_Ring0_FileIO](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S312A.HTM),
re-read) marks swap I/O with a privileged swapper flag, bypasses the cache
(`R0_NO_CACHE`) and requires the file system to avoid deadlocks on that
path. The [Microsoft FAT specification](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf)
defines the cluster chain the extent map is derived from. Decision: CiukiOS
keeps the 98 roles (dynamic file, idle prewrite after pending writes, cache
bypass, no swap ABI) with the CiukiOS numbers below; prewrite is enabled by
default as in 98 (`ConservativeSwapfileUsage=0` semantics, no option).

Repository before f0-49: M2 objects had `MM_BACKING_SWAP` and descriptor
`backing_slot` fields but refused any non-RAM page-in; M3's 32-entry
modified queue had no provider; the ledger carried zero `swap_*` fields;
f1-83's pool had no reservations; FAT growth (`fat_truncate`) copies the
whole chain, so it could not grow a live pagefile.

f0-49c research, 2026-10-21: the indexed Microsoft Resource Kit
[chapter excerpt](https://www.oocities.org/~budallen/98performance.html)
explicitly says Windows 98 retains the boot-drive swap file at boot to
reduce startup time; its size can reflect the previous session. The DDK
[IFSMgr_Ring0_FileIO](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S312A.HTM)
defines open/create action reporting and privileged uncached swap I/O;
[Q223294](https://www.betaarchive.com/wiki/index.php/Microsoft_KB_Archive/223294)
(indexed text, direct retrieval denied) confirms idle prewrite ordering.
The [Microsoft FAT specification, sections 6 and 6.7](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf)
defines file length and cluster ownership and requires initialization of
new directories. CiukiOS decision: keep the existing hidden/system `PAGEFILE.SWP`, capture
the VFS open/create action atomically and log the durable length and extent
count. `fat_reserve` uses the existing detached-chain → tail-link → directory
publication order without a data phase. Only the exclusive pagefile adapter
uses it; normal file extension still initializes bytes. Ownership starts
empty on every boot and no slot is readable before this boot writes it.

Inspection found no truncate/delete in the previous backend open or shutdown
path: `VFS_CREATE` already means open-or-create. The early `extents=0` stats
line precedes asynchronous store opening and does not report on-disk size.
Reboots of one host filesystem fixture test retention, and the same test
also passes with the pre-WIP `c5fa7583` FAT/VFS/pagefile implementations:
two extents, identical maps, no writes on reopening, both clean and unclean
shutdown. The new open record distinguishes a genuinely new file from
retained storage. Establishing the cause of the cited physical logs still
needs the lead's consecutive boots
of the same image; replacing the image or discarding an overlay also discards
its runtime file.

On normal boot the manager is gated through desktop ELF loading and its
first successful complete frame. Before that frame, admission is RAM-only
and returns immediately when RAM cannot back the request. A successful full
presentation releases the gate after presenter resources are released;
desktop launch failure, console stand-in or desktop death also releases it.
Diagnostic/test boots without a normal desktop release the gate at pager
initialization. After release, admission can wait for opening and growth
within one shared 10 s deadline. Failed opening is attempted once, leaving
RAM-only admission rather than a runnable manager retry loop. Existing
extents are mapped without allocation; the manager never rewrites their
contents at boot.

| Policy (CiukiOS, not 98 values) | Value |
| --- | --- |
| File | `C:/SYSTEM/PAGEFILE.SWP`, hidden+system, writable fixed FAT system volume only |
| Initial size / minimum | 16 MiB (one extent) |
| Growth / maximum | 16 MiB extents, before admission needs them (headroom < one extent); 64 extents = 1 GiB metadata bound |
| Disk-free reserve | growth only while free ≥ max(16 MiB, 5 % of volume) + 16 MiB |
| Shrink | trailing extents with no live slot, after 30 s without page I/O or growth, never below commitment + one extent of headroom |
| Bounded storage steps | growth 256 KiB of metadata-only reservation, shrink 1 MiB, map 512 FAT hops per volume reservation |
| Page-out batch | ≤ 8 contiguous slots per request, 8 pinned bounce pages (one f0-43 token) |
| Reserved f1-83 records | 4 page-in, 1 page-out; ordinary requests never consume them |
| Prewrite | no pressure, NORMAL free ≤ 1.5 × high, no dirty cache bytes and empty controller queue, ≥ 100 ms after demand I/O, one slice per 50 ms, ≤ 8 speculative queue entries, > 8 free slots kept for mandatory page-out |
| Errors | failed slot retired; 4 consecutive failed batches stop page-out (sticky); read failure = contained `-EIO` |

**Object.** `core/pager.c` owns up to 64 extent records (one metadata page
each, `MM_PAGER`): a 2-bit state per slot (free, reserved, used, failed), a
free bitmap and ≤ 32 contiguous LBA runs mapped from the FAT chain. A slot
outside a run (a page straddling discontiguous <4 KiB clusters, or runs
beyond 32) is failed capacity. Global counters give capacity (published
slots minus failed), used/reserved/free, outstanding I/O, store state
(off/active/quiesced/failed) and a sticky error. Slots are never relocated;
handles are 1-based `extent * 4096 + slot + 1`. Swap contents are never
trusted: every boot publishes existing extents empty, reads only slots
written in this boot, and newly exposed anonymous pages stay demand-zero.

**Commit.** `ledger_commit` admits pageable commitment against
RAM + published swap capacity; `commit_pagefile` reports promises beyond
RAM. A failed or quiesced store publishes only its live slots, so no new
commitment is admitted against failed capacity. A refused commitment calls
`pager_commit_wait` (sleepable, no object/pager level held), which asks the
manager for the deficit and waits ≤ 10 s; a refused growth is retried only
after 30 s. Win32 and section commits (f5-04) use the same path.

**Page-out.** The reclaim worker queues cold private pages through the uaddr
provider (sole owner, not busy, not pinned or VirtualLocked, and either a
valid copy or a reservable slot). The pager worker takes up to eight entries
(mandatory first), the provider revokes every alias (folding D into the
dirty epoch) and copies the page into the pinned bounce, slots are reserved
as contiguous runs, and one reserved request writes each run. Completion
revokes aliases again: unchanged pages detach (`SLOT_SWAPPED`), the frame
is freed by the queue release; a redirtied page keeps its frame and a stale
copy; a failed write retires the slot and leaves the page and its dirty
version intact. A page with a clean copy is discarded without I/O. Pager
callbacks never yield, so unmap/mprotect never see a pager transaction; a
busy object defers the page instead.

**Page-in.** A fault allocates its frame *before* marking the object busy
(waiting ≤ 5 s for reclaim while the pager is online, with back-to-back
reclaim slices for 100 ms), then holds the object busy across one reserved
demand read, which coalesces other faults on that page. The copy stays
associated while clean. Any failure returns `-EIO` to the faulting process
(signal) or syscall; the slot stays swapped; data is never fabricated.

**Prewrite.** Under the prewrite conditions above, the reclaim scanner's
separate cursor samples each candidate's own aliases (second chance) and
queues cold pages without a valid copy as speculative entries; frames stay
mapped. A demand page-in cancels a queued speculative request (f1-83
cancellation) and suppresses prewrite for 100 ms; mandatory page-out is
never cancelled and keeps f1-83 aging/promotion.

**Lock and resource order** (outermost first; a task may acquire a level
only while every level it holds is lower; recorded and refused otherwise;
trylock probes create no wait edge):

| Level | Held by | May do | Never |
| --- | --- | --- | --- |
| 1 address space | range/mutation pins | wait for 2 (commit growth) | — |
| 2 manager | extent growth/shrink/open | VFS namespace → volume → cache → device, allocate | wait for 3–7 |
| 3 object | fault, page-in, alias transaction | 4, 6, 7 | a second object level |
| 4 allocation | reclaimable frame allocation | wait for reclaim/page-out | under 5–7 |
| 5 page-out | pager worker | probe objects (no wait), 6, 7 | allocate, reclaim, manager, fault, commit wait |
| 6 slots | slot map (cooperative atomic) | ledger update | sleep, allocate |
| 7 I/O | reserved request submit/wait | controller queue | namespace, FAT, cache |

`pager_order_*` keeps a per-task mask (`struct task::pager_order`);
`frame_new`, `reclaim_slice`, `resolve_page`, the manager and the commit wait
assert it. Host tests include the negative control (allocation, reclaim,
manager, recursive fault and commit wait inside page-out I/O: five
violations, each refused). No new panic path; violations are rate-limited
`[pager] order violation` records.

**Volume dependency.** The pager holds the file with an exclusive
(`VFS_DENY_ALL`) handle: readers, delete and detach of the volume are
refused. `storage_shutdown` first quiesces the store (no page-out, prewrite
or growth; in-flight I/O drains ≤ 30 s or the shutdown is refused with
`EBUSY`), then closes the handle; live slots remain readable through their
runs. A failed shutdown resumes the store after re-mapping the file and
verifying identical runs. Safe removal of the pagefile volume is refused;
surprise loss fails the store (page-ins `-EIO`, capacity = live slots).
Page I/O routes through the partition view to the physical controller
queue with the volume's generation checks; it never flushes (swap is not
durable state) and never passes through the block cache.

Records: `[pager] attach/grow/shrink/shutdown/resume/stats` (stats at most
every 30 s from cpustat), `[storage] pagefile`, `[mem] ledger … swap_*`.
Host evidence: `tests/host/pager_test.c` (kernel suite) and
`tests/host/fs/test_pagefile.c` (filesystem suite); report
`build/codex/f0-49-report.md`. No physical qualification is claimed.

## Resource ledger (M1, f0-43)

`core/ledger.c` keeps page counters under the same `irq_save` exclusion as
the PMM bitmap. Physical owner categories form a partition: firmware/LOW
reservations, kernel image, PFN descriptors, general kernel allocations,
heap, stacks, page tables, DMA/device buffers, cache data/metadata/workspace, native private or
shared backing, and VM low/pool backing. `reserved` is a subset of physical
ownership, not another term in the physical sum. Overlapping RAM descriptors
count once; a restrictive descriptor removes the page from usable totals.
Allocation begins as general kernel ownership; the allocating object claims
its category once and returns it through `mm_free` with that category. A
mapping never claims a second copy of its backing. Unclassified permanent
kernel buffers remain in the explicit general-kernel category.

Snapshots copy only ledger counters with interrupts excluded. Reconciliation
reads the static PMM bitmap in 32-PFN sections and restarts if its accounting
sequence changes; it never walks process/cache/VM objects. Each zone must
satisfy `usable = free + sum(resident owners)`, and ledger and PMM free/total
counts must agree. Boot, the H-page Memory ledger item and the existing
`f0:allocator` probe run the check. A mismatch logs a protected `[mem] ledger
mismatch` record and increments a persistent counter, without panicking.
The allocator probe includes the result in its verdict.

Virtual reservation and RAM commitment counters are separate from resident
pages. Native committed object slots contribute a promise until final release,
including backing retained across PROT_NONE protection; shared surface backing contributes once for the
object, with each present alias counted separately. The VM admission promise
is charged before construction and rolled back through ordinary teardown.
Pagefile commitments and swap capacity/use remain zero. M1 does not change admission values, memory caps or cache growth limits;
M3 above adds pressure reclamation without page-out I/O.

NORMAL low is `max(1024, usable_NORMAL/32)` pages, with integer division;
high is twice low. Pressure enters strictly below low and clears strictly
above high. DMA free headroom remains independent. The reporting task emits
at most one protected crossing summary per second, retaining crossing and
coalescing counts, plus a protected snapshot every 30 seconds. Snapshot IDs
associate all lines of a snapshot even if another producer interleaves.
Lifecycle profiles cover boot end, five seconds after the first desktop
frame, audio start, the Files copy-start record, maximum-size VM creation and
VM teardown. Audio/refill and log hooks copy the counters and capture tick into three
fixed pending snapshots; the reporter prints them in ordinary task context.
A short copy therefore retains its copy-start snapshot even if it ends
before printing. Repeated events preserve the first unprinted snapshot.
These are observation points, not a claim of physical qualification.

Pin tokens copy the object generation, PFN extent, address mask, byte
alignment, segment boundary, direction and a non-reused completion ticket
into a fixed registry. ATA PRD/bounce buffers and audio descriptor/ring pages
hold tokens for their existing lifetimes; uncertain hardware stop retains
the tokens and pages. The eager DOS pool uses immutable, resident PTE vectors
(at most seven tables per VM) to describe scattered PFNs without adding a
per-PFN array. These vectors must contain distinct, exclusively owned pool
PFNs and remain unchanged until unpin; the table itself is held too. VM
worker termination is proved before the pool tokens are released and the
address space destroyed. Counters never traverse these vectors on snapshot.
The 64 registry entries accommodate the first clients (at most 28 VM-vector
tokens, four ATA tokens and nine AC'97 tokens). Pool-vector retirement is
synchronous after the existing worker-stop proof; DMA extent retirement may
outlive its owner. `mm_pin_retire` transfers the entire extent to the token,
and only completion or proven stop calls `mm_unpin` and releases its pages.
An old generation/ticket cannot complete a replacement token; repeat unpins
are refused and counted. Directly freeing a pinned PFN is an invariant error.

The existing IRQ exclusion protects accounting; explicit IRQ and refill
context depths drive `ledger_assert_pinned_context`. Reclaimable allocation
and page faults in those contexts increment `bad_context` and emit a protected
`[mem] pinned_context violation task=%s irq=%u site=%08x` record at most once
per second (including an initial violation at tick zero). `irq` is the nested
IRQ context depth; `task` is the current task name, or `boot` before a task
exists. The allocation proceeds and the existing fault dispatcher continues.
Only the explicit build option `CIUKI_LEDGER_STRICT` makes the guard fatal;
the production build does not define it. Host tests run both modes.
Refill marking belongs to its task, so preemption does not mark an unrelated
task critical. Allocation entry points pass their caller's return address;
vector 14 passes the faulting EIP. Following the compiler's
[return-address documentation](https://gcc.gnu.org/onlinedocs/gcc/Return-Address.html),
use `__builtin_return_address(0)` at the allocation entry point, rather than
walking an older stack frame in the guard. The task name, tick, IRQ depth and
rate limiter are captured under IRQ exclusion; logging follows its release.
PMM scans reserve/roll back at most 32 PFNs per interrupt-disabled section,
never sleep and never call filesystem reclaim, including when entered with
IF clear. Physical timing and all lifecycle profiles still require the lead's
canonical-image/hardware evidence; no QEMU evidence was collected for M1.

## Decision

The canonical CiukiOS disk is an MBR-partitioned disk with one FAT32 system
partition, from F0 on. A new real-mode loader in the gap after the MBR reads
the kernel from FAT32, collects firmware data into a versioned
`ciuki_boot_info` structure, sets the initial video mode and enters
protected mode with paging off. Ciuki VMM copies the boot information first,
builds its own page tables (kernel at `0xC0000000`), and becomes the only
owner of physical memory. Only E820 type-1 RAM is ever allocated; the first
MiB is never handed to the general allocator.

## Sources

M1 research (2026-10-19): the Microsoft DDK [BASE samples](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S3188.HTM)
distinguish physical page allocation and demand-page information. Microsoft
[Q180579](https://www.betaarchive.com/wiki/index.php/Microsoft_KB_Archive/180579)
specifies constrained allocation in PFNs, not byte addresses. The DDK's
[deadlock rules](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S31A7.HTM)
forbid blocking in critical/event contexts. Intel's [IDE bus-master specification](https://pdos.csail.mit.edu/6.1810/2004/readings/hardware/IDE-BusMaster.pdf),
sections 1.1–1.2 and 3, requires stable PRDs, boundary constraints and
completion/stop ownership. Creative's [Sound Blaster programming guide](https://pdos.csail.mit.edu/6.828/2018/readings/hardware/SoundBlaster.pdf),
chapter 3, describes a stationary cyclic DMA buffer and explicit termination.
These support separate ownership, mapping and pin accounting; they do not
establish Windows 98 numerical watermarks. M1 adopts the contract's CiukiOS
low/high arithmetic and preserves existing allocation/admission policies.
The full Windows 98 DDK/pager research gaps in f0-42 remain open.

- Microsoft, [FAT: General Overview of On-Disk Format (fatgen103)](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf):
  the FAT type is decided only by the cluster count (FAT32 needs at least
  65,525 clusters); FAT32 BPB fields `BPB_FSInfo`, `BPB_BkBootSec` (6
  recommended), `BPB_RootClus`, 32 reserved sectors typical; FSInfo
  signatures; FAT[1] clean-shutdown and hard-error bits.
- ACPI 6.5, [System Address Map Interfaces](https://uefi.org/specs/ACPI/6.5/15_System_Address_Map_Interfaces.html):
  INT 15h E820 protocol, 20/24-byte descriptors, extended attributes, address
  range types; usable memory is type 1 only.
- VESA, [VBE Core 3.0](https://pdos.csail.mit.edu/6.828/2012/readings/hardware/vbe3.pdf):
  `4F00h` controller info, `4F01h` mode info (ModeAttributes bit 7 = LFB
  available, `PhysBasePtr`, `LinBytesPerScanLine`), `4F02h` with bit 14 to
  select the linear model, `4F03h` read-back; VBE does not provide
  acceleration.
- Ralf Brown's Interrupt List (reference for firmware calls without a single
  vendor specification): [INT 13h AH=41h EDD check](https://www.ctyme.com/intr/rb-0706.htm),
  [INT 15h AX=2401h A20](https://www.ctyme.com/intr/rb-1336.htm),
  [INT 15h AX=E801h](https://www.ctyme.com/intr/rb-1739.htm),
  [INT 10h AX=4F15h BL=01h read EDID](https://www.ctyme.com/intr/rb-0308.htm),
  [INT 1Ah AX=B101h PCI BIOS](https://www.ctyme.com/intr/rb-2371.htm).
- Intel, [64 and IA-32 Architectures SDM, Vol. 3A](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html):
  Vol. 3A "Processor Management and Initialization" (mode switching) and
  "Paging" (32-bit paging), CR0.WP,
  PSE/PGE.
- [ELF specification (System V ABI, i386 supplement)](https://refspecs.linuxfoundation.org/elf/elf.pdf):
  `ET_EXEC`, `EM_386`, `PT_LOAD`, `p_paddr`/`p_vaddr`.
- SeaBIOS 1.16.3 [keyboard services](https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/src/kbd.c)
  and [BIOS clock](https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/src/clock.c)
  (F1-37 research, 2026-10-11): INT16 AH=01 polls without consuming,
  AH=00 consumes, and the BDA timer wraps daily at 18.2 Hz; comparison with
  `menu_getc`, `deadline_start` and `deadline_expired` supports reusing the
  existing keyboard/COM1 poll and restarting both tick and stalled-clock bounds.
- Repository facts: the 0.8 disk is an unpartitioned FAT16 volume of 262,144
  sectors whose boot sector reserves 73 sectors for Stage1
  (`src/boot/full_boot.asm`, `bpb_reserved_secs = 1 + 72`); Stage1 loads
  `CIUKIDOS.SYS` to `0x0900` (`full_stage1_loader.asm`). The 0.8 release
  history recorded T23 installs that failed with CHS and succeeded with EDD
  and with direct ATA writes (dev diary 2026-05-04), and a T23 VBE mode whose
  scanline query was malformed (commit `548920a`).

## Disk layout (mandatory from F0)

| LBA range | Content |
| --- | --- |
| 0 | MBR: boot code, disk signature, partition table |
| 1 – 1023 | `CIUKLDR` loader image (at most 511.5 KiB; fixed header with size and CRC32) |
| 1024 – 2047 | reserved, zero |
| 2048 – end | partition 1: type `0Ch` (FAT32 LBA), active, the system volume |

The FAT32 system volume:

- 512-byte sectors, 4 KiB clusters, 2 FATs, **160 reserved sectors**
  (`CIUKI_FAT_RESERVED_SECTORS` in `src/boot/ciukldr/disk.inc`, `FAT_RESERVED`
  in `scripts/build_image.py`): boot sector 0, FSInfo at sector 1, backup boot
  sector at 6 (its FSInfo copy at 7), the blackbox descriptor `CBB1` at sector
  8 and the 128-sector blackbox ring at sectors 32–159; root directory at
  cluster 2. (The earlier "32 reserved sectors" text was stale; corrected by
  f9-00 on 2026-10-21.)
- At least 65,525 data clusters, so a partition of at least about 270 MiB.
  The canonical image is 512 MiB (sparse on the host; compressed in releases).
  The 0.8 128 MiB size cannot be FAT32 with 4 KiB clusters.
- The partition boot sector (VBR) carries a valid FAT32 BPB. From F1 it MAY
  also carry boot code that loads `CIUKLDR` for chain-loading boot managers;
  F0 relies on the MBR path only.
- System files use 8.3-compatible names so the loader needs no LFN support:
  `\SYSTEM\VMM.ELF` (kernel), `\SYSTEM\BOOT.CFG` (plain-text boot options).
- Images are produced with `mkfs.fat -F 32` and `mtools` from the host build;
  the build MUST verify the volume with `fsck.fat -n` and check that no loader
  byte overlaps LBA 2048 or later.
- The installer (F1) writes the same layout to a physical disk, keeping any
  existing partitions only when the owner explicitly selects it.

## Loader (`CIUKLDR`, real mode, NASM)

The MBR relocates itself, verifies the loader header and CRC, and loads it
with INT 13h `AH=42h` after the `AH=41h` EDD check. CHS is used only when
EDD is absent, and then only within the first 1024 cylinders. Every canonical
image contains the same loader, including the probe selector; with no test
request the system boots normally. The loader MUST, in this order:

1. Set its stack below `0x7C00`, record the boot drive (`DL`) and the
   partition LBA.
2. Collect platform data, without failing when a source is absent: PCI BIOS
   presence and mechanism (`B101h`), the SMBIOS entry point
   (`F0000h–FFFFFh`; type 1 identifies QEMU, T23 and E500), EDD parameters for
   the boot drive (`AH=48h`), APM installation check (`5300h`, record only),
   the ACPI RSDP address (EBDA first KiB, then `E0000h–FFFFFh`, record only).
   Then, before any direct i8042 access: when the SMBIOS type 1
   manufacturer string reads `QEMU`, verify the fw_cfg `QEMU` signature at
   ports `510h`/`511h` and read and validate the test request
   `opt/it.alcybercloud.ciukios/test` through the file directory (physical
   machines never get fw_cfg port accesses). A valid `platform=e500` request
   sets `input_policy = 1` and flag bit 7; a valid `safe=1` request sets
   flag bit 0 (safe mode) as `BOOT.CFG` or the menu would. Unless bit 7 is
   already set, run the exact E500 PCI platform match before A20; success
   sets `input_policy = 1` and bit 14 (`CBI_F_INPUT_DETECTED`). Normal defaults
   to native unless the E500 platform match or an explicit request selects
   firmware; the kernel falls back to native when firmware fails (F0-37).
   Clear the complete boot-info block at loader entry, before discovery,
   requests, A20 and the menu, on both cold and warm boots. Preserve the
   incoming boot drive across clearing.
3. Enable A20: test the wrap-around first; try `INT 15h AX=2401h`, then the
   keyboard-controller output port (skipped on firmware-first input profiles
   unless separately qualified), then port `92h` (preserving bit 0). All
   controller waits are bounded. Stop with an error if the wrap-around test
   still fails. A20 stays enabled for the life of the system (guest A20 is
   virtual, `dos-dpmi-contract.md`).
4. Read the memory map with E820 (24-byte buffer, continuation in `EBX`,
   maximum 128 entries) and **normalize it before any load**: reject
   overflowing, zero-length, malformed or truncated maps; treat a 20-byte
   record as `ext = 1`; drop entries whose `ext` bit 0 is clear; resolve
   overlaps to the more restrictive type; round type-1 ranges inward to
   4 KiB. Missing or invalid E820 stops boot with an error. `E801h` and
   `INT 15h AH=88h` are recorded for diagnosis only, never used for
   allocation.
5. Read `BOOT.CFG`, then show a boot menu for 3 seconds (keyboard through
   `INT 16h`, and COM1 at 38400 8N1 with bounded polling when the port
   answers): Normal, Safe mode, Serial log on/off, and the probe selector
   defined in `f0-acceptance.md`. A QEMU test request was already collected
   in step 2 and is not read again; physical machines never get fw_cfg port
   accesses. BOOT.CFG may also provide a selector line, automatically accepted
   after this override window; N/S cancel the configured request (positive
   safe-mode sources still persist). A validated fw_cfg request outranks cfg
   and bypasses the menu. The loader emits L:SELECT_SOURCE=cfg|menu|fw_cfg.
   The validated UART base and divisor are passed to the kernel.
   S cancels the configured request, adds the safe flag, confirms it with
   `Safe mode; P for a probe, or wait` and `L:SAFE=1`, and restarts
   the bounded three-second menu window so P can enter a selector (without
   a `safe=1` suffix), L can toggle logging, and N or expiry boots with the
   safe flag preserved and `L:SELECT_SOURCE=menu`.
6. Select and set the video mode (below).
7. Load `VMM.ELF`: require `ELFCLASS32`, `ELFDATA2LSB`, `ET_EXEC`, `EM_386`.
   For each `PT_LOAD` segment, the whole extent `[p_paddr, p_paddr + p_memsz)`
   (BSS included) MUST lie inside `[0x00100000, 0x01000000)` and inside
   normalized allocatable RAM, without overlapping the loader, the boot
   information or another segment. Copy `p_filesz` bytes, zero the rest.
   Copies above 1 MiB use unreal mode (flat data segment), never
   `INT 15h AH=87h`.
8. `e_entry` is a **virtual** address. Find the executable `PT_LOAD` segment
   that contains it and compute the physical entry
   `p_paddr + (e_entry − p_vaddr)`; reject the image if no segment contains
   it. Fill `ciuki_boot_info`, mask both PICs, load a flat GDT, set `CR0.PE`,
   jump to the 32-bit stub and enter the kernel at the physical entry with
   `EAX = 0x4B554943` (`"CIUK"`) and `EBX` = physical address of the
   structure. Paging is off, interrupts disabled, the direction flag clear.
   The kernel's bootstrap stub uses physical addresses until it enables
   paging, then jumps to the high half.

Every loader failure prints a short code on screen and on COM1 and waits for
a key; it never boots a partially loaded kernel.

### Video mode selection

- Read controller info (`4F00h`, VBE 2.0+ signature) and every mode in its
  list (`4F01h`).
- Pitch and colour masks: with VBE 3.0 use `LinBytesPerScanLine` and the
  `Lin*MaskSize/FieldPosition` fields; with VBE 2.x use `BytesPerScanLine`
  and the ordinary mask fields.
- A mode is eligible only if: graphics, supported by hardware, LFB available
  (attribute bit 7), direct colour 32 or 24 bpp (16 and 8 bpp only as later
  fall-backs), pitch ≥ width × bytes per pixel, `pitch × height ≤
  TotalMemory × 65536`, a nonzero `PhysBasePtr` with no 32-bit overflow of
  `PhysBasePtr + pitch × height`, and valid, nonoverlapping colour masks.
  Modes failing these checks are logged and skipped (the T23
  malformed-scanline case).
- Preference: the mode named in `BOOT.CFG`; else 1024×768, then 800×600,
  then 640×480, each 32 then 24 bpp. Safe mode prefers 640×480.
- Set the mode with bit 14, read it back with `4F03h`, and re-read its mode
  info; a mismatch rejects the mode and tries the next one.
- If no mode qualifies, stay in text mode 3 and set the text flag; the
  kernel then uses the VGA text console and the serial log (D9).
- Read EDID block 0 with `4F15h BL=01h` after the mode is set; failure or a
  bad checksum is normal and recorded.

## `ciuki_boot_info` version 1 (frozen)

Little-endian, packed, 16-byte aligned, placed by the loader between
`0x00000500` and `0x00090000`. Firmware addresses inside it are opaque values
that the kernel validates before any use.
The kernel MUST check `magic`, `version == 1`, `size == 0x10F0`,
`e820_count ≤ 128`, `test_request_len ≤ 64` and every extent for overflow
**before** copying the structure into kernel memory, and before it
allocates or overwrites any memory below 1 MiB. Unassigned flag bits and
reserved fields MUST be zero and are rejected otherwise. An unsupported
version is rejected; a later version is a new layout with its own size.

| Offset | Field | Type | Meaning |
| --- | --- | --- | --- |
| `0x000` | `magic` | u32 | `0x31494243` (`"CBI1"`) |
| `0x004` | `version` | u16 | `1` |
| `0x006` | `size` | u16 | `0x10F0` |
| `0x008` | `flags` | u32 | bit 0 safe mode; 1 serial log selected; 2 EDID valid; 3 VBE controller info valid; 4 text mode (no LFB); 5 SMBIOS reports QEMU; 6 test request present; 7 input policy forced by validated QEMU fw_cfg `platform=e500` (requires bits 5 and 6, `input_policy = 1` and the matching selector; zero otherwise); bits 8–10 A20 method (1 already on, 2 `INT 15h`, 3 keyboard controller, 4 port `92h`); 11 boot log on screen (menu `L`, amendment 2026-10-12); 12 high-volume device/VM diagnostics (accepted fw_cfg selector, amendment 2026-10-15); 13 hardware check page (menu `H`); 14 input policy selected by the loader's E500 platform match (`CBI_F_INPUT_DETECTED`, F0-37) |
| `0x00C` | `boot_drive` | u8 | BIOS drive number |
| `0x00D` | `memmap_source` | u8 | `1` = E820 (the only accepted value) |
| `0x00E` | `input_policy` | u8 | `0` native i8042 (default), `1` firmware-first from the E500 platform match or explicit request; effective kernel backend may fall back (F0-37) |
| `0x00F` | `uart_state` | u8 | loader UART result: 0 unknown (older loader), 1 OK, 2 BDA COM1 not 3F8h, 3 scratch failure, 4 LSR FFh, 5 divisor read-back failure, 6 disabled by THRE timeout, 7 disabled by character budget (F0-38) |
| `0x010` | `partition_lba` | u32 | FAT32 partition start |
| `0x014` | `loader_start`, `loader_end` | u32 × 2 | physical extent, reclaimable after copy |
| `0x01C` | `kernel_start`, `kernel_end` | u32 × 2 | physical extent of all loaded segments |
| `0x024` | `kernel_entry_phys` | u32 | translated entry (step 8) |
| `0x028` | `e820_count` | u32 | entries used in `e820[]` |
| `0x02C` | `e801_low_kib`, `e801_high_64k`, `int88_kib` | u32 × 3 | diagnostic only |
| `0x038` | `uart_base`, `uart_divisor` | u32 × 2 | validated COM port and divisor, `0` when absent |
| `0x040` | `pci_bios` | u32 | bit 0 present; bits 8–15 mechanism byte (`AL`); bits 16–23 last bus |
| `0x044` | `apm` | u32 | BCD version in bits 0–15, bits 16–31 zero; `0` when absent |
| `0x048` | `acpi_rsdp_phys`, `smbios_entry_phys` | u32 × 2 | opaque, `0` when absent |
| `0x050` | `fb_phys`, `fb_pitch` | u32 × 2 | active framebuffer (`0` in text mode) |
| `0x058` | `fb_width`, `fb_height` | u16 × 2 | pixels |
| `0x05C` | `fb_bpp` | u8 | bits per pixel |
| `0x05D` | `fb_red_size`, `fb_red_pos`, `fb_green_size`, `fb_green_pos`, `fb_blue_size`, `fb_blue_pos`, `fb_rsvd_size` | u8 × 7 | colour masks from step 6 |
| `0x064` | `vbe_mode` | u16 | active mode number (`0` in text mode) |
| `0x066` | `test_request_len` | u16 | `0`–`64` |
| `0x068` | `test_request` | char × 64 | ordinary selector grammar of `f0-acceptance.md` plus the cfg-only sweep/cursor grammar above, not NUL-terminated |
| `0x0A8` | `options` | char × 128 | `BOOT.CFG` options, NUL-terminated |
| `0x128` | `edid` | u8 × 128 | valid only with flag bit 2 |
| `0x1A8` | `edd` | u8 × 66 | `AH=48h` result for the boot drive |
| `0x1EA` | `reserved1` | u8 × 6 | `0` |
| `0x1F0` | `vbe_ctrl` | u8 × 512 | `4F00h` copy |
| `0x3F0` | `vbe_mode_info` | u8 × 256 | `4F01h` copy of the active mode |
| `0x4F0` | `e820` | 128 × 24 bytes | normalized map: base u64, length u64, type u32, ext u32 |

### UART diagnosis and recovery (F0-38)

F0-38 UART diagnosis and recovery: `uart_state` describes only the loader;
the kernel preserves it even after recovery. Loader timeout/budget disable
clears `uart_base`, `uart_divisor` and `CBI_F_SERIAL_LOG`. When the state is
not 1, the kernel independently probes 3F8h at divisor 3 (38400 8N1) using
reversible SCR patterns, LSR presence, both divisor bytes and a bounded THRE
check. Successful recovery publishes the effective port/divisor and enables
the serial flag; a failed kernel probe clears those runtime fields. The
original loader fields and kernel probe result are logged at boot, and the
H-page absence detail includes both result codes. Unknown state 0 keeps old
v1/v2 loaders compatible. Research and the kernel result-code table are in
[F0-38 UART validation](../validation/f0-38-loader-uart.md).

### BOOT.CFG selector and cursor (F1-28)

The file remains <=127 ASCII bytes without NUL; ordinary `safe`, `serial` and
`mode` tokens retain their existing semantics. At most one `probe=` line is
allowed. It occupies its entire line (CRLF or LF), without trailing spaces:

```text
probe=(f[012]:<probe|all|sweep>|all:sweep) run=<8hex> [step=<0..63> [state=<8hex>]]
```

`all` remains F0/F1 only. `step` and `state` are internal sweep cursor suffixes,
invalid on ordinary probes. Sweeps enter only via BOOT.CFG, including QEMU
(overlays); menu and fw_cfg retain their ordinary selector grammar. No cfg
selector accepts `platform`, selector `safe=1`, or `server` suffixes.
The request after `probe=` stays <=64 bytes. The optional state word preserves
six-bit passed/failed/not_run counters in bits 0..5/6..11/12..17 and a pending
completion bit 18. Reserved bits must be zero. Before a step, the durable
cursor advances by one and provisionally counts it as not_run. Completion
replaces that provisional count; a reset without completion retains not_run.
This extra state is necessary for truthful totals after reset/panic, which
cannot be recovered from an in-memory accumulator or a step number alone.
No boot-info ABI bits or fields are added.

## Physical memory ownership

- Usable pages: type 1 entries of the normalized map in `ciuki_boot_info`
  (loader step 4). Types 2, 3 (ACPI reclaim), 4 (NVS) and unknown types are
  never allocated in F0–F4.
- Reserved in addition: the kernel image, `ciuki_boot_info` until copied, the
  loader until copied, kernel page tables, any framebuffer or MMIO range that
  overlaps RAM, and every page below 1 MiB.
- The first MiB: page 0 (IVT and BDA), the EBDA (segment from BDA `40Eh`,
  checked against `INT 12h`) and `A0000h–FFFFFh` are preserved for the BIOS
  call path (`device-firmware-ownership.md`). Physical `10000h–1FFFFh` is the
  fixed BIOS-call scratch area. The remaining conventional pages form the
  LOW zone, used only for ISA DMA bounce buffers and BIOS-call buffers.
- Zones: LOW (< 1 MiB), DMA (< 16 MiB, contiguous allocations, no 64 KiB
  boundary crossing for ISA), NORMAL. A bitmap allocator (one bit per page)
  with per-zone free counts; contiguous requests search the DMA zone only.
- DOS VMs never use physical low memory for their conventional memory: their
  first MiB is ordinary pages mapped at linear 0 in their own address space.
- Exhaustion returns an error to the caller; the kernel never panics on a
  failed allocation outside early boot (F0 criterion).

## Virtual layout

| Range | Use |
| --- | --- |
| `00000000–BFFFFFFF` | per address space: native process or DOS VM (`execution-abi.md`, `dos-dpmi-contract.md`) |
| `C0000000–EFFFFFFF` | direct map of physical RAM from 0 (up to 768 MiB); kernel image at `C0100000` |
| `F0000000–FF7FFFFF` | kernel virtual allocations: framebuffer and MMIO mappings, kernel stacks with guard pages, vmalloc |
| `FF800000–FFBFFFFF` | reserved, unmapped |
| `FFC00000–FFFFFFFF` | recursive page-directory window |

- Kernel page-directory entries 768–1022 point to page tables allocated at
  boot and shared by every address space, so kernel mappings never need to
  be copied after boot. Entry 1023 is per address space: it points to that
  address space's own directory (recursive window), is supervisor-only, and
  is excluded from mapping-equality checks.
- Kernel PDEs 768–1022 are created with the accessed flag already set
  (f6-21b, 2026-10-22). The processor sets the accessed flag of every
  paging-structure entry it uses in the directory that CR3 names, and the
  dirty flag only in the leaf entry
  ([Intel SDM Vol. 3A 4.8, as quoted on LKML](https://lkml.iu.edu/hypermail/linux/kernel/2103.1/08629.html)).
  A directory copied before a kernel task first touched a 4 MiB region would
  otherwise differ from the kernel directory in that bit only. On QEMU
  (image 8af310c2) the isolation probe's second process zeroed its data page
  at 28 MiB, the first page of PDE 775, after both clones, and reported
  `shared_kernel_errors=1`. Linux presets the same flag in kernel table
  entries (`_KERNPG_TABLE`,
  [pgtable_types.h v6.12](https://github.com/torvalds/linux/blob/v6.12/arch/x86/include/asm/pgtable_types.h)).
  Kernel PDEs are therefore bit-identical in every directory for the whole
  boot; the working-set sampler clears accessed bits only in user PTEs.
  Host regression: `tests/host/kernel_pde_test.c`.
- 4 MiB PSE pages MAY be used, when CPUID reports PSE, only for direct-map
  ranges whose pages all share the same permissions and cache attributes.
  The kernel image (read-only text and data), guard pages and MMIO always
  use 4 KiB pages. PGE MAY mark kernel pages global.
- Kernel pages are supervisor-only; `CR0.WP = 1`; kernel text and read-only
  data are mapped read-only.
- RAM above 768 MiB is ignored and logged in F0–F4 (the targets have at most
  1 GiB; T23 and E500 maximum RAM is an open question).
- MMIO and framebuffer mappings use cache-disable or write-combining only as
  allowed in `device-firmware-ownership.md`.

## Early kernel sequence

1. The entry stub (physical addresses) builds a temporary page directory that
   identity-maps the first 16 MiB and maps it again at `0xC0000000`. This
   covers every loaded segment (all inside the 16 MiB load window), the boot
   information, the loader stack and the stub. It then enables paging and
   jumps to the high-half address.
2. Validate the handoff address, the header and all bounds of
   `ciuki_boot_info` before copying it; then initialise early serial and the
   crash screen (text or LFB).
3. Build the physical allocator from the E820 copy and the reservations
   above; log usable and reserved totals per zone.
4. Build the final kernel page tables, remove the identity map, reclaim the
   loader pages.

## Memory budget at 128 MiB (targets to be measured in F0/F1)

| Item | Target |
| --- | --- |
| Kernel image (F0 / F4) | ≤ 512 KiB / ≤ 2 MiB |
| Kernel page tables and allocator bitmap | ≤ 1.5 MiB |
| Kernel heap at boot | ≤ 4 MiB |
| Block cache (F1, adjustable) | 8 MiB default |
| Desktop back buffer at 1024×768×32 (F2) | 3 MiB |
| One DOS VM (F3, `dos-dpmi-contract.md`) | default resident cap 48 MiB (f3-47 Amendment 2): ≤ 24 MiB XMS/EMS/DPMI pool, ≤ 16 MiB private disk cache, ≤ 8 MiB low memory, tables and device state |
| Uncommitted host headroom kept by VM admission | 32 MiB |
| Available for processes and VMs, before VM reservations and headroom | ≥ 100 MiB |

These are targets, not measurements. The F0 boot log MUST print the measured
values, and any later change to a target needs a measurement on QEMU at
128 MiB and on one physical laptop (AGENTS.md).

## Acceptance tests

- T1 static: the built image has an MBR with one active `0Ch` partition at
  LBA 2048, `fsck.fat -n` passes, FAT32 cluster count ≥ 65,525, `BkBootSec`
  6, the loader fits before LBA 1024 and its CRC matches; a host unit test
  checks the `ciuki_boot_info` offsets and size above.
- F0 probe `bootinfo` (`f0-acceptance.md`): the kernel validates the real
  structure and rejects synthetic copies with a bad version, size, count or
  overflowing extents; it prints the E820 entries, usable and reserved totals
  per zone, kernel extent and video mode. QEMU at 128 MiB and 256 MiB, T23,
  E500.
- F0 probe `allocator`: exhaustion fails cleanly and 100 mixed
  allocation/free cycles restore the free counts; no reserved page is ever
  returned.
- `boot` subcase "video fallback": QEMU with `-vga none` reaches `READY`
  with the text flag set; evidence is the serial log (no VGA text output is
  assumed).
- Loader error paths (bad CRC, missing `VMM.ELF`, segment outside the load
  window, entry outside every executable segment, invalid E820) stop with
  their codes on COM1 (QEMU only).

## Open questions

- Maximum installable RAM on the T23 and E500, and whether any E820 entry on
  either laptop is above 768 MiB. Evidence: E820 dumps from both machines
  with maximum RAM fitted.
- Whether both laptops boot from an MBR whose partition starts at LBA 2048
  (older BIOSes sometimes expect CHS-aligned partitions). Evidence: a boot of
  the F0 image on each laptop.
- Whether the T23 and E500 firmware expose COM1 for the loader menu and log.
  Covered by `f0-acceptance.md`.
- CD boot (El Torito no-emulation, ISO9660) is deferred to F4; its loader
  variant is specified then.

## Interfaces required from other contracts

- `device-firmware-ownership.md`: the BIOS-call path that uses the preserved
  first-MiB areas and the scratch area; MMIO cache attributes.
- `execution-abi.md`: the per-address-space layout below `0xC0000000` and
  kernel stack sizes.
- `dos-dpmi-contract.md`: the VM layout at linear 0 and the A20/HMA policy.
- `f0-acceptance.md`: probe names and the probe selector in the boot menu.

## Amendment 2026-10-12: quiet normal boot

- Flag bit 11 (`CBI_F_VERBOSE`) is defined: the menu `L` key sets it (and
  still toggles bit 1). `CBI_F_DEFINED_MASK` becomes `0x00000FFF`; layout,
  size and version are unchanged.
- With no test request and no safe flag at menu time, the loader clears the
  text screen and writes only `S for safe mode, L for log` on
  row 24 (`INT 10h AH=13h`); `SELECT_READY`, the menu text, provenance and
  `L:VBE_SKIP` lines go to COM1 only. `S`, `L`, `P` and every fatal error
  switch screen output back on. Test-request and safe boots print as before.
- The loader image is fixed at 24576 bytes (assembly-time check). The 1 KiB
  video ranking table moved out of the file into scratch directly after it;
  `loader_end` covers it (`0x26400`), so the reported loader extent still
  contains every byte the loader writes.
- On such a boot the kernel draws the Ciuki photograph splash instead of
  clearing the framebuffer, draws no console line until the desktop takes
  the display, and draws the history on a panic, double fault, desktop
  failure or a 120 s watchdog. See `docs/boot.md`.


### Amendment 2026-10-15: diagnostic verbosity

Bit 12 (`CBI_F_DIAG_VERBOSE`) selects UHCI schedule dumps, HID poll/pipe
traces, VM performance samples and recurring accelerator summaries. An accepted
fw_cfg test selector sets it. Ordinary hardware boots, BOOT.CFG selectors and
the screen-log `L` choice leave it clear. `CBI_F_DEFINED_MASK` is now
`0x00001FFF`; the handoff version, size and offsets remain unchanged. Existing
selector strings and suite predicates remain valid. See
[F0-22 research and validation](../validation/f0-22-hardware-log-cost-and-timer-gaps.md).

### f1-23 amendment: bootinfo version 2 display extension

The v1 prefix and offsets above remain frozen. A v2 handoff has version=2,
size=0x1344 and a tagged DISP block at 0x10F0, described in the
[f1-23 acceptance amendment](f1-acceptance.md#amendment-f1-23--display-inventory-and-runtime-vbe-modes-2026-10-12).
The validator accepts exactly v1/0x10F0 or v2/0x1344. The kernel zeroes its
private copy and copies only the declared validated size, so a v1 allocation
never requires extension bytes. Unknown versions/tags/sizes are refused.
OEM strings and the bounded mode inventory are copied values, never live
firmware pointers. This amendment supersedes the earlier version-1-only check.

## f0-26: explicit hardware check selection

Flag bit 13 (`CBI_F_HW_CHECK`) selects the H boot-menu diagnostic page.
The defined mask is `0x00003FFF`; structure version, size and offsets stay
unchanged. H cancels a configured probe selector, preserves positive safe-mode
sources, enables the text console and enters the bounded hardware checks
instead of the desktop. See [hardware-check.md](hardware-check.md).

The 512-byte ELF segment bounce buffer moves to loader BSS after the existing
selector scratch. Each read initializes `copy_count` bytes before the copy
loop consumes exactly those bytes. The loader stays 48 sectors with either
the default or release version string; its page-rounded reservation is
unchanged. The menu model and both assembled extents are host-tested.

## f0-37 review amendment: detected E500 input policy

Flag bit 14 (`CBI_F_INPUT_DETECTED`) records the loader's exact E500 platform
match, independently of bit 7's validated fw_cfg override. The defined mask
is `0x00007FFF`; structure version, size and offsets stay unchanged. Normal
defaults to native unless the E500 platform match or an explicit request
selects firmware; the kernel falls back to native when firmware fails.
Safe mode also selects firmware first. Policy 1 without forced, detected or
safe flags is logged as an unknown source and treated as native. See the
[input activation amendment](hardware-check.md#f0-37-input-activation-and-ebda-2026-10-16)
for the PCI BIOS sources and fallback order.

## F0-40 loader progress trail

The fixed 48-sector loader now exposes short menu/video/ELF/handoff tokens
on both screen and available UART. Cooperative deadlines cover loader loops
and returning BIOS calls, with a separate finite poll budget for a stopped
BDA clock. Optional video falls back to text; a timed-out partial ELF stops
visibly and never waits for a key or transfers control to the kernel.

An optional private `CLDRPRG1` trail occupies the file's last 32 bytes;
boot_info v1/v2 and the public ABI stay unchanged. The kernel validates and
replays its stage masks through protected `klog` before loader-memory reclaim.
The sector scratch buffer moves after `cfg_buffer` in reported loader BSS;
all 512 bytes are initialized by a successful BIOS read before use. Fixed
file size and the page-rounded reservation are preserved. The format,
deadlines, assembled tests and limits (including nonreturning BIOS calls
and pre-kernel disk persistence) are in the
[F0-40 audit](../validation/f0-40-reliability-audit.md#loader-trail-and-limits).

## f0-54: loader byte budget

The ELF program-header scratch (`ph_table`, at most 32 × 32 bytes) shares
the raw E820 buffer (`raw_map`, 128 × 24 bytes), which only `memory_collect`
uses before A20, the FAT mount and the menu; later RAM checks read the
normalized `boot_info.e820` array. `elf_load` fills every program-header
byte it reads. Ordering invariant (accepted by the lead, 2026-10-21):
`raw_map` is dead once `memory_collect` returns, and `elf_load` stays after
it; code that reads or writes `raw_map` after `memory_collect`, or a
program-header read before it, must first give `ph_table` its own buffer
again. An assembly-time check keeps `MAX_PH * 32` within `raw_map`.
The file stays 48 sectors; `loader_end` (`0x26841`) and the
page-rounded reservation are unchanged. `scripts/loader_budget.py` reports
the bytes per section and the free tail, and the size test requires at
least 512 free bytes. See the
[f0-54 record](../validation/f0-54-loader-budget.md).
