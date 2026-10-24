# VFS and storage contract

Contract 4 of 7 required by decision D11. Author: Claude (lead). Reviewer:
Codex. Status: approved after the Claude–Codex cross-review, 2026-10-09.

## Decision

Storage is layered: block devices, an MBR partition layer, one block cache,
filesystem drivers, and a VFS that owns names, open-file nodes, handles,
sharing and locks. FAT32 with long file names is the native system
filesystem from F1, read first and written only after its read gate passes.
All offsets are 64-bit. Metadata writes follow a fixed order so that a crash
can leave lost clusters but never cross-linked chains. A volume with
detected corruption or repeated write errors becomes read-only. DOS VMs use
the same VFS through the bridge defined in `dos-dpmi-contract.md`.

## Sources

- Microsoft, [FAT: General Overview of On-Disk Format (fatgen103)](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf):
  FAT type by cluster count, FAT32 BPB and FSInfo, FAT entry masks (28 bits),
  end-of-chain and bad-cluster values, the `FAT[1]` `ClnShutBitMask` and
  `HrdErrBitMask` bits (FAT32 `0x08000000`/`0x04000000`, FAT16
  `0x8000`/`0x4000`; a set bit means "clean" and "no I/O error"
  respectively), directory entries, long-name entries and their checksum,
  short-name generation.
- T13, [ATA/ATAPI-6 draft d1410r3a](https://www.read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/ATA-d1410r3a.pdf):
  `FLUSH CACHE` (`E7h`) and write-cache features.
- Linux, [`fs/fat`](https://github.com/torvalds/linux/tree/master/fs/fat)
  (GPL-2.0): a reference implementation of FAT/VFAT behaviour, including
  name handling; read for behaviour, not copied.
- dosfstools, [`fsck.fat`](https://github.com/dosfstools/dosfstools): the
  host-side checker used by the tests below.
- [ECMA-119 (ISO 9660)](https://ecma-international.org/publications-and-standards/standards/ecma-119/)
  for the CD filesystem (F4).
- Linux, [ntfs3 documentation](https://docs.kernel.org/filesystems/ntfs3.html)
  (GPL-2.0 driver) and the Linux-NTFS project's
  [NTFS documentation](https://flatcap.github.io/linux-ntfs/ntfs/) for the
  read-only NTFS driver (F4).
- The existing driver `src/vm/session_disk_ata.c` and its rule against BIOS
  fallback after an issued command (`src/vm/session_disk_ata.h`).
- Repository facts: the 0.8 DOS kernel keeps its own dirty FAT cache and
  issues INT 13h directly (`src/boot/floppy_stage1.asm`); LFN logic lives in
  `src/lfn/lfn.c`; FAT32 layout and image rules are in `boot-memory.md`.

## Layers

| Layer | Phase | Responsibility |
| --- | --- | --- |
| Block device | F1 (ATA PIO/DMA), F4 (ATAPI) | Sector I/O, geometry, error reporting, media change; one owner per controller (`device-firmware-ownership.md`) |
| Partition | F1 | MBR primary and extended partitions; GPT is out of scope |
| Block cache | F1 | One cache for all devices; write-back with ordering barriers |
| Filesystem drivers | F1 FAT12/16/32+LFN; F4 ISO 9660, NTFS read-only | On-disk formats; no driver keeps a private cache of blocks |
| VFS | F1 | Names, drive letters, open-file nodes, handles, sharing modes, byte-range locks, change notification hooks |

## Namespace

- Drive letters as in Windows 9x: `C:` is the system partition; further
  partitions, then removable and CD devices, take the next letters in
  enumeration order; assignments are recorded in the boot log.
- Paths are case-insensitive and case-preserving; `\` is the separator and
  `/` is accepted; maximum path length 259 characters plus terminator (the
  Windows `MAX_PATH` limit); components follow FAT long-name rules.
- The VFS uses UTF-8 internally. FAT long names are UCS-2 on disk; short
  names use code page 437 in F1. Unmappable characters are rejected, not
  silently replaced.
- Short names for new long names are generated with the fatgen103 algorithm
  (`~N` tails), checked for collisions in the target directory.

## Open-file model

- One in-memory node per open file per volume, shared by every handle, so
  size and timestamps stay coherent between native processes and DOS VMs.
- An open creates a reference-counted **open description** that owns the
  access mode, share mode and 64-bit position. Handles in a process (or DOS
  VM) table point to open descriptions; duplicated and inherited handles
  share the description and therefore its position (DOS `45h`/`46h` and
  EXEC inheritance). FAT files above `0xFFFFFFFF` bytes fail with `EFBIG`.
- This contract owns the DOS compatibility-mode and sharing/locking matrix.
  The proposed f3-48 amendment below anticipates that bridge into F3 and must
  be cross-reviewed before implementation.
- Share modes (deny none / read / write / all) are checked at open against
  every existing handle of the node, with DOS compatibility-mode semantics
  provided by the bridge.
- Byte-range locks are per node, owned by a handle, released when the handle
  closes or its owner is torn down (F3, needed by INT 21h `5Ch`).

**[F3-48 proposal: DOS claims]** Explicit modes use the symmetric matrix
`(new_access & old_deny) || (old_access & new_deny)`: a nonzero result refuses
the open. Access READ/WRITE bits are 1/2; deny-none/read/write/all are 0/1/2/3.
DOS compatibility is a separate owner-sensitive state, not mask 0. Implement
the DOS 2–6.22 table [RBIL #01403](https://fd.lod.bz/rbil/interrup/dos_kernel/213d.html#table-01403),
including same-owner compatibility reopens, read-only exceptions and cases
requiring INT24 instead of direct error 5. Native and DOS claims share node
identity and check both directions; drive/name aliases never create another
claim registry. The full ownership, inheritance, range-lock close rules and
conformance requirements are [DF2/DF4](dos-dpmi-contract.md#df4-sharing-and-record-locks).
This amendment is a contract proposal, not a claim that current VFS already
implements compatibility mode or range locks.

## Block cache and write ordering

**[F1, f1-79 amendment, 2026-10-17]** The short namespace mutex protects
tables, shared descriptions, node identity, cwd pins, observers and mount
publication. Each letter has a separate volume mutex. Lock order is namespace
→ volumes in ascending letter order → cache → device. Before borrowing
identities, filesystem operations reserve their complete drive set under namespace;
contenders sleep with namespace released. Native path preflight follows the
synthetic `/mnt/<letter>` transitions and cwd ancestry without disk access,
including intermediate components before `..`; two-path operations reserve
the union before resolving either path.

Filesystem calls release namespace, keeping the reserved volume mutexes.
On return they drop those mutexes before reacquiring namespace → volumes.
The reservation stays published during this handoff, preventing a same-volume
open, mutation, final close or removal from observing partial publication.
Global node/description/handle slots are claimed before dropping namespace.
This replaces the former requirement to keep namespace across commit and
detach: shutdown prevents new writeback, drains the active writer (including
delayed-error publication), then reserves every drive before preflight.
Removable removal reserves every partition of its physical device before any flush. The DOS VM
gate still spans seek plus transfer on its private disk description.

Attaching a prepared volume is namespace-only publication and refuses a letter
reserved by another owner. Optical expiration ends its old reservation before
preparing a new mount; NTFS preparation/unmount use the slot's current letter.
Legacy relative paths retain a default-drive snapshot across I/O. Native
`getcwd` reserves its volume before copying names, excluding partial rename
publication as well as expired mount identities.

Media queries take namespace alone and copy the published mount identity,
immutable volume label/identifier and current blocker list. They never reserve
a volume or call filesystem/transport callbacks. Optical preparation occurs
before publication; detach removes the published pointer before reuse. Waits
of at least 100 ms emit the verbose `storage/vfs_wait` record.

Research checked against the former `vfs.c`, native `_locked` helpers,
`mount.c` and `media_dev.c` before implementation: Linux's
[filesystem locking](https://docs.kernel.org/filesystems/locking.html) separates
directory/inode exclusion and description offsets from global bookkeeping;
[directory locking](https://docs.kernel.org/filesystems/directory-locking.html)
requires a consistent order for multi-object operations. Its
[sequence-counter documentation](https://docs.kernel.org/locking/seqlock.html)
warns against lockless traversal of pointers whose lifetime can end during a
read: Ciuki therefore retains short namespace exclusion for mount/blocker
copies rather than introducing a pointer seqlock. The
[Microsoft FAT specification](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf)
defines cluster chains, directory ownership and FSInfo hints; this change
preserves the existing FAT algorithms and durability barriers. These sources
inform the locking decision; the drive-reservation protocol is Ciuki's own.

**[F2, f2-11 amendment]** Durable shutdown MUST preflight every attached
volume under the shared VFS namespace lock for native volume-backed open
descriptions and legacy VFS handles before stopping storage, committing or
detaching any volume. A refusal MUST return `-EBUSY`, leave storage running
and every volume attached with its mount generation unchanged, and permit a
later `storage_sync()` after the final close to succeed, provided no I/O error
intervenes. Duplicated/inherited fds retain their shared description until the
last close, following the [POSIX open-description lifetime](https://pubs.opengroup.org/onlinepubs/009604499/functions/close.html).
Cwd pins and structural/synthetic nodes MUST NOT count as open descriptions
on a volume. Per-drive reservations MUST remain held from preflight through
commit and durable detach (f1-79), preventing concurrent opens from invalidating
the preflight. Successful detach MUST invalidate native mount identities and
reset legacy per-drive cwd to `/`; native cwd expiration follows
`posix-subset.md`. Optional native hooks MUST remain weak for standalone F1
storage. `storage_sync()` first drains/closes the boot-log sink; an EBUSY
refusal keeps storage usable but does not reactivate that sink. Existing
sticky write/flush errors remain errors and MUST NOT be converted into a
successful clean shutdown.

- Default size 8 MiB (`boot-memory.md` budget), adjustable at boot.
- Dirty blocks become eligible for the `device`-priority writer after
  1 second or at the dirty budget (F1-82 below), on explicit `flush`, before unmount, and
  before shutdown or reboot.
- A **durability barrier** waits for all earlier writes to complete and
  then for a qualified device flush (`FLUSH CACHE`), or for verified
  disabled write caching on that device. Until the ATA driver implements and
  qualifies `FLUSH CACHE` (the 0.8 backend has none), write-back of FAT
  metadata is not enabled on that device. Errors from delayed writes are
  kept and reported by the next `flush`, `commit` or unmount.
- Ordering for FAT writes, enforced with durability barriers between steps:
  1. allocation: write the new chain to every FAT copy;
  2. write the file data;
  3. update the directory entry (first cluster, size, timestamps);
  4. update FSInfo hints last (hints are never trusted on mount).
  Deletion: mark the directory entry free first, then free the chain.
  Rename within a directory rewrites the entries in place. Across
  directories, without a recovery journal, the source entry is durably
  removed before the destination entry is published, under ordered
  namespace locks; an interruption can orphan the chain (lost clusters) but
  never leaves two entries owning it.
- These guarantees assume atomic, complete sector writes. At most a lost
  cluster chain may result from a crash; cross-linked chains are a driver
  defect. Torn sectors or diverging FAT copies are detected on mount and
  lead to read-only handling.

**[F0, f0-32 amendment, 2026-10-16]** Ordinary writes supersede the allocation
order above: update existing sectors in place, initialize new data and zero
seek gaps, barrier, publish a detached allocation chain to every FAT copy,
barrier, extend the previous EOF link, barrier, publish directory size and
timestamps once, barrier, then update FSInfo hints. Select new clusters under
the volume lock without publishing allocation before their data is durable.
Only explicit truncation retains the existing full-file COW ownership switch;
ordinary overwrites and appends MUST NOT copy the existing prefix. Overwrite
contents may be partially updated after a cut; published length remains old or
new. After a complete sole-ownership scan, an EOC/link disagreement at the
published EOF may be repaired and excess regular-file tails detached before
reclaim. Disagreement inside the visible extent, crosslinks and invalid chains
retain the write gate. FAT12 split live links retain safe refusal; new ordinary
write allocations avoid them. Native regular-file syscalls transfer at most
64 KiB per call, returning short and releasing the namespace lock before the
caller's next run. Research and host evidence:
[f0-32 validation](../validation/f0-32-fat-write-in-place.md).

**[F0, f0-36 amendment, 2026-10-16]** The three reserved boot logs
`/SYSTEM/LOGS/BOOT.LOG`, `BOOT1.LOG` and `BOOT2.LOG` are an exception to
ordinary-file excess-tail trimming. After every ownership pass succeeds,
recover their unpublished text from the active, solely owned chain before
finalizing its unused reservation. Bound recovery by `BOOTLOG_LIMIT` (320 KiB),
preserve the published length, accept printable bytes (including high bytes),
TAB/CR/LF and trailing zero padding, skip entirely zero sectors (including
the protected-prefix/runtime-ring gap), and stop before a malformed sector.
The recovered EOF is the last text byte in the last valid sector; padding
does not become logical data. Publish that size durably before detaching or
freeing unused clusters. When extending past an interrupted EOF detach,
first mirror the ownership-proved active link durably. Log repairs report
`action=extended`, `recovered_bytes` and the resulting `size`; empty unused
reservations report `action=finalized`. Ordinary files retain f0-32 policy,
and live disagreements/crosslinks still refuse every repair write.

Research checked against the current implementation: Microsoft's
[FAT specification v1.03, directory size and FAT chains](https://www.cs.fsu.edu/~cop4610t/assignments/project3/spec/fatspec.pdf)
stores byte length independently of allocation; upstream
[dosfstools v4.2 `check_file()`](https://github.com/dosfstools/dosfstools/blob/v4.2/src/check.c)
trims ordinary excess allocation, while
[Linux v6.12 `fat_free()`](https://github.com/torvalds/linux/blob/v6.12/fs/fat/file.c)
orders size publication before detachment. Text recovery is a Ciuki-specific
policy for its zero-initialized reservations, not generic FAT semantics.
The f0-25 implementation and its validation record actually finalized logs
at published EOF too; there was no existing FAT log-sector validity routine
to retain. The raw black box has separate sequence/CRC validation, unchanged.
Plain log text has no checksum: syntactically valid torn data cannot be
distinguished from valid text and is not claimed to be detected.

The five-second publisher still serializes with namespace/FAT mutations.
A namespace holder can make `bootlog_drain()` return `EBUSY` before either
data draining or metadata publication, even when the metadata deadline is
due. Recovery uses the durable reserved chain rather than depending on that
publisher's progress; it needs no new lock bypass, memory budget or disk
format. Bytes still in RAM or an unflushed device cache are not recoverable.
Physical write counters count padded/repeated 32 KiB writes, not unique
logical bytes: twelve writes do not imply a 384 KiB recoverable file.

**[F1, f1-24 amendment]** In addition to the interrupted states above, an
atomic-sector crash during directory publication may leave nonowning orphan
long-name entries: the [Microsoft FAT specification, version 1.03, pp.
26–28](https://www.cs.fsu.edu/~cop4610t/assignments/project3/spec/fatspec.pdf)
requires contiguous LFN slots immediately before their short entry, with
ordinal/checksum validation and a zero LFN first-cluster field, and defines
unpaired slots as orphans. Ciuki's `fat_create()` always preserves the exact
spelling in an LFN, including `F109CUT.BIN` (one slot); `slots()` durably
clears the future short slot and terminator before `publish()` makes the LFN
durable, then publishes the sole owning short entry. Entries can span sectors;
separate barriers also permit an orphan within one sector. In the supplied
image `33a68c43a8a9488c275cb983b0aaabef05e7c5f3fb31a7b6b3478ff488c66797`
at `62bd77f`, the marker occupies byte 160 of disk sector 4120 (volume
sector 2072), the workload LFN byte 192, its short entry byte 224, and the
new terminator byte 256: cut write 1 persists slot preparation and write 2
persists the LFN; write 3 publishing the short entry is suspended. The
device cache is disabled, so these barriers need no traced flush
(`barrier=0`); dirty flags were made durable before ARM. The short entry,
allocation, 8192-byte data, replacement and rename have not become durable.
This ordering preserves ownership and requires no driver change. For
`mount-crash-reboot`, the declared workload outcomes are an empty
`F109CUT.BIN` with no orphan diagnostic, or an absent `F109CUT.BIN` with
exactly its orphan-LFN diagnostic paired with `Auto-deleting.`; the marker
MUST remain empty, the dirty diagnostic MUST occur on the boot-1 cut export,
and cross-links, lost
chains and every other discrepancy MUST fail. [dosfstools 4.2
`lfn_check_orphaned()`](https://github.com/dosfstools/dosfstools/blob/v4.2/src/lfn.c#L483)
proposes deleting only those slots; [`fsck.fat -n`](https://github.com/dosfstools/dosfstools/blob/v4.2/src/fsck.fat.c#L163)
does not apply the deletion or dirty-bit removal and returns 1 for pending
changes. Both boots MUST classify all checker output and independently
verify the corresponding absent/empty file state through mtools; boot 2
MUST reuse the unrepaired overlay and preserve the directory-sector digest
and classified absent/empty workload state. Per the owner's decision of
2026-10-11 (f1-26), boot 2 MUST recover a dirty-only volume with `lost=0`,
`scan_corrupt=0` and no FAT-copy divergence, open the usual read/write gate,
and report `recovered=1`, `reason=dirty_recovered`, mode `rw` and zero write
refusals. Its export after durable unmount MUST have no dirty diagnostic;
the declared orphan-LFN diagnostic remains allowed and MUST NOT be repaired
by the kernel. Both checker reports MUST agree on the workload state, with
only the expected removal of the dirty diagnostic between boots.

### F1-82 budgeted writeback and reader/writer concurrency (2026-10-19)

Research before implementation: Linux documents separate
[background dirty thresholds and writer thresholds](https://docs.kernel.org/admin-guide/sysctl/vm.html#dirty-background-bytes).
Its upstream [`balance_dirty_pages`](https://github.com/torvalds/linux/blob/v6.12/mm/page-writeback.c)
wakes background writeback and sleeps the task generating dirty data. Ciuki
uses that separation, with a fixed per-device background budget of
`min(cache_bytes / 8, 1 MiB)`, a high watermark of twice that budget, and a
one-second oldest-dirty deadline. This is a Ciuki policy, not Linux's adaptive
bandwidth algorithm. The cache size and volume FIFO reservations do not change.

The original implementation already released the cache mutex for transport
(F1-78), but selected dirty LRU victims ahead of available clean ones and
used whole-device barriers for periodic writeback. FAT append also calls
durability barriers, so background policy alone cannot remove its synchronous
single-sector command overhead. Bounded LBA-sorted write batches serve both
explicit barriers and background work; adjacent sectors share one command.
Each batch reserves at most 64 blocks (at most 32 KiB), using a preallocated
buffer charged to the cache budget. Tiny caches retain their single-sector
fallback and eight-slot minimum. Busy reservations exclude writes/eviction
of participating blocks while cache bookkeeping is unlocked for transport.
Clean hits and unrelated misses remain available during writes and flushes.

An online writer sleeps on cache changes with a 10 ms safety poll and drains
immediately while work remains due. Reader allocation prefers any eligible
clean slot; if only healthy dirty victims remain it requests writeback and
waits up to 20 ms, then counts a synchronous eviction as a last resort.
The writing caller above the high watermark sleeps until its device falls
below the background budget. This sleep releases cache bookkeeping, but
retains the caller's FIFO volume reservation: F1-79 does not permit another
operation to observe partially published FAT metadata. Slow/failing devices
can therefore still delay same-volume readers; the replay is evidence for
its stated device model, not a universal latency bound. Standalone caches
without an installed writer retain synchronous eviction rather than waiting
for a service that does not exist.

After 20 ms without a reduction in its device's dirty bytes, a throttled
caller assists by writing/flushing bounded batches of its own debt until it
falls below budget. This is necessary because the shared storage task also
drains the boot log: its namespace try-lock does not prevent it from waiting
for a volume reservation in `vfs_seek`/`vfs_log_write`. Waiting indefinitely
for that same task from `cache_write` would deadlock. The caller uses the same
busy/epoch reservations and sticky-error path as background writeback, and
its full wait/assistance time is included in writer-wait statistics. This
follows the generating-task writeback semantics of Linux's `dirty_bytes`
reference while retaining Ciuki's existing FAT publication locks.

The writer must inspect sticky errors before reserving a volume for error
publication. Reserving healthy volumes after every batch would deadlock a
writer throttled under that volume's reservation. Failure publication still
reserves/revalidates the affected volume, revokes `FAT_RO_WRITE_ERROR`, and
is included in the existing shutdown writer drain. Explicit barriers retain
all previous metadata ordering and qualified device flushes. Delayed flush
failures remain sticky even after sectors have become clean in RAM.

Protected `[storage] writeback` records every five seconds while dirty data
exists include current dirty bytes, both budgets, bytes in the last tick,
maximum tick/flush milliseconds, `cache_sync_evictions`, writer waits and
wait duration. Each completed `vfs_wait` of at least 100 ms remains recorded.
Record dirty bytes and counters are cache-wide totals; both budgets apply
individually to each device.

Allocation audit reference: [Microsoft FAT specification, sections 4–6](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf).
Zero FAT entries identify free clusters; FSInfo's free count and next-free
cluster are advisory and must be validated. The host model uses a 512 MiB
FAT32 volume, two FATs and 4 KiB clusters. A 64 KiB append selects 16 clusters
in three bounded passes from validated `next_free` (48 allocation probes),
updates both FAT copies, publishes one directory sector, then FSInfo.
No scan from cluster 2, change to disk format, or removal of a durability
barrier is justified. Timing, physical counts and replay results are recorded
in `build/codex/f1-82-report.md`; the host suite exercises production FAT/VFS.

## Mount, errors and corruption

- Mount checks: BPB values and signature, FAT count, cluster count matching
  the declared type, root cluster inside the volume, FSInfo signatures, and
  the media byte in `FAT[0]` and the reserved-entry and flag encoding of
  `FAT[1]`. A failure refuses the mount and logs why.
- **[F1, f1-26 amendment; owner decision 2026-10-11]** The
  [Microsoft FAT specification, section 4.2, p. 19](https://www.scs.stanford.edu/~zyedidia/docs/_other/fat.pdf)
  defines `FAT[1]` masks: FAT16 `ClnShutBitMask=0x8000`,
  `HrdErrBitMask=0x4000`; FAT32 `0x08000000`, `0x04000000`.
  A set clean bit means clean shutdown; a set hard-error bit means no
  recorded I/O error. Clearing the dirty indication therefore means
  `FAT[1] |= ClnShutBitMask`, preserving the hardware-error bit and all
  other bits, including FAT32's reserved upper nibble. The existing
  `fat_mount()` compares complete FAT sectors and runs the bounded
  ownership scan before `storage_enable_write()` opens the write gate.
  A dirty-only volume MUST recover automatically when that scan reports
  `lost=0`, no corruption and no FAT-copy divergence, and durability is
  qualified. The initial requested read-only mount MUST issue zero writes
  and flushes; recovery runs when the usual read gate permits write
  enablement. Set the clean bit in copy 1, barrier, copy 2, barrier (or
  the single copy and its barrier). These are sector writes through the
  shared cache, with FLUSH CACHE or verified disabled device caching.
  Only after all recovery barriers succeed may the driver remove the
  dirty read-only reason and report `recovered=1`; activation records
  `reason=dirty_recovered` in its `[storage]` line. Before writable use,
  clear the clean bit again (`FAT[1] &= ~ClnShutBitMask`) for the new
  session, with the same per-copy barriers. A successful durable unmount
  sets it with that same order. A cut between copies leaves a detectable
  mismatch: the next mount's existing divergence scan MUST retain
  read-only mode rather than reconcile copies automatically. Lost
  clusters, corruption, divergent copies, `HrdErrBitMask=0` (reason
  `error-flag`), unqualified durability or any clearing write/barrier
  failure MUST retain read-only handling. The hardware-error flag MUST
  NEVER be cleared automatically; repairs stay a host-side `fsck.fat`
  step. FAT12 has neither flag. `FAT[0]` must match `BPB_Media`.
- Chain walks are bounded by the volume's cluster count, so a loop is
  detected instead of hanging.
- A read error returns `EIO` to the caller. A failed issued ATA command ends
  that request with `EIO` and puts the controller ownership in quarantine
  (`device-firmware-ownership.md`); there is no automatic retry, through the
  BIOS or otherwise, until a separately qualified recovery path exists. A
  metadata write failure turns the volume read-only and reports it.
- Removable media (F4): a media change invalidates cached blocks and nodes
  for that device; open handles then fail with `EIO`.

### F1-70 mount memory and scan budget (2026-10-15)

The directive's clean-mount path supersedes the unconditional ownership scan
above: after BPB/reserved-entry validation and an actual FAT census, agreeing
clean/error-free copies may skip ownership traversal. Dirty, differing and
explicit-check paths retain the full ownership proof. FSInfo hints cannot
establish the used extent. Both FATs are read in runs of at most 64 KiB;
shared-cache dirty hits remain authoritative for fixed/live mounts.

Ownership uses one bit per cluster, with at most 1 MiB of individually
allocated NORMAL pages per checker. Allocation failure uses the existing
cache workspace in passes; all passes must validate before reclamation.
Directory traversal uses an explicit stack covering every path representable
by the existing 259-character path contract. Deeper trees retain readable
read-only access with `directory_depth_limit`. Dirty validation/recovery
passes stop at the allocated high-water mark measured in both FATs. Log
tails detach durably before sector-coalesced orphan reclamation. Allocation
refusals record required/available workspace bytes, and recovery records
include elapsed time. See [research and qualification](../validation/f1-70-fat-mount-budget.md).

### F1-74 removable read-only mount and deferred ownership (2026-10-16)

This supersedes attach-time scanning and automatic writable activation for
removable FAT only. Research checked before implementation:
[Microsoft FAT specification §§4.2/4.3/5](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf)
defines clean/error flags and FSInfo signatures, unknown free-count sentinel
and range-checked advisory hints; [Linux v6.12 FAT mount](https://raw.githubusercontent.com/torvalds/linux/v6.12/fs/fat/inode.c)
reads FSInfo and reserved metadata without an ownership walk. Hints cannot
prove sole ownership or authorize allocation.

Repository comparison: `storage_add_removable_bounded()` used `fat_prepare()`
with FAT read-ahead windows, a complete census, then automatic write upgrade.
For the T23's 1,953,289-cluster FAT this consumed 12,293 sectors before the
failed read. Introduce a separate removable preparation path: validate BPB,
backup BPB, FSInfo and reserved entries/flags of each FAT using single-sector
reads, retain dirty/copy/error reasons and publish read-only with scan deferred.
Use an in-range FSInfo free count for advisory free-space reporting; unknown
or invalid hints report zero until validation, with next-free defaulting to 2.
No FAT windows, ownership bitmap, writes or flushes are needed for publication.

The existing explicit `storage_enable_write()` upgrade (including the f0-25
hardware workflow) runs the F1-70 bounded-memory checker with full ownership
validation even for clean removable media before any metadata write. It
recomputes free space, detects crosslinks, loops and FAT disagreements, and
applies the existing repair/durability gates. Failure leaves read-only access;
unavailable durability does not trigger a needless full scan. Scan time and
completion state are separate from mount time. Fixed volume mounts and the
existing full-scan `fat_prepare()` interface keep their policy.
The safe-remove write probe explicitly requests that same writable upgrade
before creating its test file, so read-only publication is not mistaken for
a failed durability gate.

### fx-17 removable media writable at insertion (2026-10-23)

This supersedes, for removable FAT only, F1-74's read-only publication
with writes gated on an explicit ownership-scan upgrade, and F2-74's
"volume repair at the next insertion" for a yanked device. T23 trial of
`e831e685`: `[usb-storage] ready … mode=rw_capable` then
`volume drive=E … mode=ro dirty=0`. Nothing on the user path ever asked
for the F1-74 upgrade, so every stick stayed read-only; a move from it
copied the file and then failed with EROFS.

Research: Windows 98 Resource Kit ch. 10 (File Systems; local copy
`build/rskt/wrkc10_1.htm`): ScanDisk runs after an improper shutdown at
the next start-up for the system's disks, not when a medium is inserted;
ch. 26 (`wrkc26_4.htm`) makes write-behind caching for removable disks an
option, off by default. [Microsoft FAT specification §§4.2/5](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf):
`ClnShutBitMask` clear means the volume "was not dismounted properly …
it would be a good idea to run a Chkdsk/Scandisk"; `FSI_Free_Count`
0xFFFFFFFF means unknown and "must be computed". [Linux v6.12
`fs/fat/inode.c`](https://github.com/torvalds/linux/blob/v6.12/fs/fat/inode.c)
`fat_set_state()`: a read-write mount marks the volume dirty, a dirty
volume is mounted read-write with the warning "Volume was not properly
unmounted … Please run fsck", and its state is not changed ("do not
change state if fs was dirty"); an unknown free count is counted when
needed.

Decision (`storage_activate_removable()` → `fat_enable_write_removable()`,
called by the USB storage service after publication and by
`floppy_mount()`):

- Publication is unchanged (single-sector BPB/FSInfo/reserved-entry
  reads, zero writes). Activation then makes the volume read-write without
  an ownership scan when the medium is writable: block device writable
  (MODE SENSE WP clear), durability qualified, and no reason beyond
  requested/dirty/error-flag. Write-protected media, unqualified
  durability, divergent reserved FAT entries, a corrupt backup BPB, an
  active-FAT-only FAT32 and DMF disks stay read-only with their reason.
- Allocation only takes clusters whose active-FAT entry is zero, and this
  driver's publication order leaves at worst lost clusters after a cut
  (growth: data, detached chain, link, entry; delete: entry before
  reclamation). Writing a dirty medium is therefore safe; its dirty or
  hardware-error indication is preserved (`dirty_preserved`: no clearing
  at insertion, none at a clean unmount) for an explicit check, as Linux
  does. A clean medium has its clean flag cleared for the session (two
  FAT[1] writes with barriers) and set again by Safely Remove.
- Free count: FAT32 trusts an in-range FSInfo hint (Windows and Linux
  both do). FAT12/16, and FAT32 whose hint is unknown, are counted at
  activation when the active FAT is at most 512 KiB; a larger FAT32 with
  an unknown hint stays read-only (`reason=free_count_unknown`) until the
  explicit full upgrade, because counting megabytes of FAT under the
  drive reservation would stall its readers on USB 1.1.
- The explicit `storage_enable_write()`/`fat_scan()` path (f0-25
  workflow, hardware check) keeps the F1-70 full ownership proof and
  repair for any volume still read-only.
- Record: `[storage] removable drive=E mode=rw dirty=0 reason=writable
  free=… census_ms=… result=0`, before the `[usb-storage] volume` line.

USB command failures: a READ(10)/WRITE(10) the device ends with CHECK
CONDITION and valid sense is a failed request, not a lost device (SBC-3
sense keys; Linux sd completes the request with an error). The stick
stays online and published (`[usb-storage] command_error … state=online`);
DATA PROTECT (sense key 7) on a write marks the device write-protected and
returns EROFS. Transport faults, medium change/absence and readiness
failures still quarantine and unpublish. A FAT metadata write failure
keeps the existing rule: the volume becomes read-only for the session
with the failing file named, and stays mounted and readable.

### fx-17b unknown free count, write-through sticks, floppy drives (2026-10-23)

Lead decisions after fx-17. They supersede fx-17's "large FAT32 with an
unknown hint stays read-only" and "sticks without SYNCHRONIZE CACHE stay
read-only".

**Unknown FSInfo free count.** [FAT specification](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf)
§5: `FSI_Free_Count` 0xFFFFFFFF means unknown and "must be computed";
`FSI_Nxt_Free` is only a hint for where to start looking for a free
cluster. FreeBSD treats an unknown next-free hint the same way
([msdosfs commit, 2004](https://lists.freebsd.org/pipermail/cvs-src/2004-March/019652.html)),
and Linux counts lazily ([Linux v6.12 `fat_statfs`](https://github.com/torvalds/linux/blob/v6.12/fs/fat/inode.c)).
No primary Microsoft document on Windows 98's own recalculation was
found. The rule above is the specification's.

Decision:
- A FAT32 removable volume whose hint is unknown and whose FAT is larger
  than 512 KiB is writable at once (`fat_enable_write_removable`).
- `storage_census_service()` on the storage writer task counts the active
  FAT in steps of one 128-sector read-ahead window under the drive
  reservation, then sleeps three times the step's duration. The count
  therefore uses at most a quarter of the medium's time, does not run on
  the display or audio tasks, and the CPU work per step is one pass over
  64 KiB.
- One byte per FAT sector holds its free entries, plus a dirty bit:
  - every published active-FAT sector (transaction publication or direct
    write) sets the bit after the cache holds the new bytes;
  - a step clears the bit before it reads;
  - the count is final only when a step finds no dirty sector while
    holding the reservation.
  Allocations during the count are therefore never double-counted or lost.
- Until then:
  - allocation scans from the next-free hint, with the free count only an
    upper bound (ENOSPC comes from the scan);
  - FSInfo keeps 0xFFFFFFFF;
  - `fat_free_estimate()` reports counted-free plus not-yet-counted
    clusters to DOS INT 21h/36h, Win32 and `MEDIA_GET_SPACE` with
    `CIUKI_MEDIA_SPACE_COUNTING`, which Files shows as "Free Space:
    calculating...".
- Removal (safe or surprise) cancels the count and frees its arrays.
- A volume left read-only (write-protected and similar) still gets its free
  space counted (`fat_census_start`).
- Record: `[storage] free_count clusters=… free=… elapsed_ms=…
  source=background`.

**Sticks without SYNCHRONIZE CACHE.** SBC-3: SYNCHRONIZE CACHE is
optional. Linux sd treats an unsupported cache and an unreadable caching
page as "Assuming drive cache: write through" and never flushes such a
device. Windows 98 never flushes removable media and keeps write-behind
off for them by default (Resource Kit ch. 26).

Decision: an ILLEGAL REQUEST answer to the bind's SYNCHRONIZE CACHE makes
the device `BLKDEV_CACHE_DISABLED` (`write_through`). Every WRITE(10) then
counts as durable when its CSW reports success, the host cache is already
write-through for removable FAT, and no flush is ever sent.

Records: `[usb-storage] flush … result=unsupported … cache=write_through`
and `ready … mode=rw_write_through`.

Trade-off: a stick that buffers internally and reports success early can
lose the last writes on power loss or on removal without Safely Remove.
That is the same exposure as on Windows 98 and Linux. Safely Remove still
commits FAT metadata and the clean flag through ordinary writes. Any other
flush failure keeps the volume read-only.

**Floppy drives A:/B: in the native namespace.**
- `/mnt/a` and `/mnt/b` exist while a diskette is mounted. `/mnt` lists A:,
  B:, then D: to Z:; `/mnt/c` never exists. The posixpath reservation
  encoding stores drive d as kind d+2, so 0 and 1 no longer collide with
  the /mnt and /dev kinds.
- `MEDIA_GET_STATE` reports `CIUKI_MEDIA_FLOPPY` for every drive the FDC
  found, setting the removable bit only while a diskette is mounted.
  `MEDIA_FLOPPY_LOAD` reads the diskette (Files' Retry); ENXIO means no
  disk.
- Windows 98 has no floppy insertion event and reads the diskette when the
  drive is opened. The existing 1 s DIR poll still detects a changed or
  removed diskette while one is mounted.
- Native floppies get the same writable activation as USB media; FAT12 has
  no clean flag, so no write happens at insertion. Write protection
  surfaces on the first write as EROFS (`write_protected`).

## Filesystem driver rules

- FAT12/16/32 share one driver. FAT12/16 keep a fixed root directory; FAT32
  uses `BPB_RootClus`. Long-name entries are written and read with checksum
  validation; orphaned long-name entries are ignored on read.
- ISO 9660 (F4): Level 1–3 names, Joliet when present, Rock Ridge not
  required; read-only.
- NTFS (F4): read-only; uncompressed resident and non-resident data,
  directories through `$I30` indexes; compressed, encrypted or sparse
  features that are not implemented fail with `EOPNOTSUPP` instead of
  returning wrong data. Write support needs a separate decision (D6).

## Interface for the DOS bridge

The VFS offers the operations INT 21h needs, so the DOS personality never
touches shared volumes through sectors: open/create with DOS attributes,
read/write/seek, close, find-first/find-next with attribute masks and
wildcards over short and long names, get/set attributes and DOS timestamps,
rename, delete, mkdir/rmdir, current directory per VM and drive, volume
free space, share modes and byte-range locks, and commit (`68h`). Raw sector
access to a shared mounted volume is refused (`dos-dpmi-contract.md`).

## Acceptance tests

- T0 host: the FAT driver and VFS core compile for the host and run against
  images made by `mkfs.fat` (FAT12, FAT16, FAT32): create, write, read,
  rename, delete, long names with collisions, directory growth beyond one
  cluster, files of 0 bytes, of one cluster and of several MiB. After each
  run `fsck.fat -n` reports no errors and `mtools` lists the same names and
  sizes.
- T0 crash injection: replay a write workload and cut it after every block
  write. Under atomic sector writes no cut may produce cross-links; lost
  chains, the dirty flag and interrupted FAT-copy synchronisation are
  permitted, and FAT-copy divergence MUST lead to a read-only mount.
  Torn-sector handling is tested separately.
- F1 QEMU suite: the same workload on the canonical image through the kernel
  ATA driver; host `fsck.fat -n` on the overlay afterwards.
- F1 hardware: write a known file set on the T23 and E500, power-cycle, read
  it back on the laptop and on the host; checksums match.
- F1 mount paths: a consistent dirty-only image recovers after the read
  gate and exports clean after durable unmount; dirty plus lost clusters,
  corruption or FAT divergence and hardware-error flags stay read-only.
  A corrupted BPB refuses mount. Recovery cuts between FAT copies MUST
  be detected as divergence on the next mount, with logged reasons.

## F1-44: firmware-configured ATA bus-master DMA

Sources checked against `drivers/ata.c` before implementation:

- Intel, [Programming Interface for Bus Master IDE Controller 1.0](https://pdos.csail.mit.edu/6.1810/2004/readings/hardware/IDE-BusMaster.pdf),
  sections 1–3 and 5: PRD layout, ordering, completion and BAR4.
- Intel, [PCI IDE Controller Specification 1.0](https://www.isdaman.com/alsos/hardware/hdc/pciide.pdf),
  section 2: class and compatibility/native programming interface.
- T13, [ATA/ATAPI-6 d1410r3a](https://www.read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/ATA-d1410r3a.pdf),
  sections 6.6, 8.15.24/44/49, 8.46.11, 9.9 and annex B: selected
  UDMA mode, cable validity, SET FEATURES and reset effects.
- Upstream [QEMU IDE core](https://github.com/qemu/qemu/blob/master/hw/ide/core.c),
  [QEMU PCI IDE](https://github.com/qemu/qemu/blob/master/hw/ide/pci.c) and
  [SeaBIOS ATA](https://github.com/coreboot/seabios/blob/master/src/hw/ata.c):
  QEMU reports selected UDMA5 and an 80-wire cable; BM capability flags start
  clear and SeaBIOS does not set them. Those flags are software bookkeeping,
  not hardware admission bits; preserve them without requiring them set.

The existing channel mutex, synchronous block API, storage task, PIO and
ATAPI paths remain. DMA adds one bounded IRQ-completed ATA transaction.
This amendment permits future PIO requests after a DMA failure with proven
idle hardware; it does not relax the existing PIO/IDENTIFY/FLUSH fault policy.

PCI class/subclass `01/01`, programming interface bit 7 and an assigned,
16-byte-aligned I/O BAR4 qualify the bus master. Bits 0/2 indicate primary/
secondary native mode; bits 1/3 indicate switchability. This step supports
channels already in compatibility mode (legacy task file and IRQ14/15).
It never switches interface mode or invents native PCI IRQ routing. More
than one candidate for the same channel is ambiguous and leaves DMA off.
Claim each channel's eight BAR4 ports before accessing them. Require I/O
decode and verify PCI command bit 2 using a 16-bit write that preserves
other command bits and does not write PCI status W1C bits. Simplex masters
use only the primary channel; secondary stays PIO. No vendor timing writes.
An active firmware bus master is stopped and the channel quarantined before
discovery; it cannot silently enter PIO while an old DMA transfer is live.

IDENTIFY word 49 must advertise DMA and word 53 must validate word 88.
Word 88 must contain exactly one selected, supported UDMA mode, with no
simultaneously selected multiword mode. Reaffirm that exact mode through
SET FEATURES `EFh`, features `03h`, count `40h | mode`. Never select above
the firmware's choice, or derive a choice from support bits alone. For modes
above 2, require valid word 93 (`15:14 = 01b`) and cable bit 13. If the cable
evidence conflicts, stay PIO rather than retiming a chipset to a lower mode.
Discovery retains its existing SRST. A bounded, nIEN IDENTIFY before reset
captures the firmware selection, since devices may lose UDMA under SRST
(6.6.1); after reset revalidate support and cable before restoring that exact
selection. Without a measured selection DMA stays off. ATAPI
always uses PIO. SET FEATURES failure disables DMA; uncertain busy state
still quarantines the channel.

Each PRD is eight bytes: even physical address, even byte count, reserved
bits zero, bit 15 of the final flags word is EOT. Count zero represents
65536 bytes. Neither a region nor the dword-aligned table may cross a
64 KiB boundary. A request uses at most 128 KiB, thus at most three PRDs
for a contiguous range. One page holds the table; a pinned contiguous
128 KiB bounce buffer per channel handles odd or noncontiguous requests.
Direct mappings must be contiguous cacheable kernel RAM, writable on reads,
and exclude PMM reservations (including MMIO and permanent kernel pages).
Buffers and tables stay pinned until reboot, including uncertain failures.

Publish descriptors, direction (bit 3 set for disk reads) and table pointer;
clear bus-master error/IRQ with W1C while preserving capability bits. Issue
READ/WRITE DMA (`C8h/CAh`, EXT `25h/35h`), then start. The IRQ handler stops
the master, snapshots bus-master and ATA status, acknowledges W1C, and
wakes the existing generation-bound wait queue; it copies no data. Success
requires that Start was issued, the matching IRQ, BM interrupt without BM error, and drive ready
without BSY/DRQ/ERR/DF. Read bounce data is copied only after success.

f1-92 (`docs/validation/f1-92-ata-early-interrupt.md`) refines the edge
rules. After Start, an edge whose bus-master status has neither Interrupt
nor Error, or has Interrupt with Active while the drive still shows BSY or
DRQ, is not a completion: the handler leaves the master running and the
waiter keeps the command's deadline (a drive that ends without a latched
Interrupt gets a bounded 10 ms grace, then fails as `bm_no_interrupt`). An
edge between the DMA command and Start without ERR/DF from a present drive,
with the master neither Active nor in Error, cannot be this command's end
(ATA-6 9.7): it is counted (`early_irq`), consumed, and the master is started.
An issued command is never ended by its caller's cancellation or request
budget; interruption is honoured only before a command is issued.

Any DMA error or 30-second timeout stops the master and records a sticky
per-device DMA quarantine. When the bus master and drive are proven idle,
the same request is repeated once through PIO (same buffer and LBAs) before
any error reaches the caller; later requests use PIO. Only a failed PIO
repeat returns EIO with the whole-channel quarantine. If stop cannot
establish idle (or quiescence intervenes), retain the existing whole-channel
quarantine without a repeat. Late IRQs cannot resurrect a generation.
Quiescence stops DMA before proving idle and retains all claims on
uncertainty. An edge before Start with ERR is the drive's abort (DMA
quarantine and PIO repeat); with DF, a missing drive or an active/error
master it still quarantines the channel.
If Stop fails on native hardware, revoke PCI bus mastering for the whole
function before returning a direct buffer. Failure to verify that revocation
halts the kernel rather than allowing further DMA into reclaimed memory.

Verbose `[ataN] dma mode=udmaM` records identify unit, BAR, cable and buffer
size. DMA quarantine records include ATA/BM status and elapsed time.
`[launch-io]` retains cache request counters and adds successful DMA read
bytes, requests, maximum size, bounce requests and milliseconds, attributed
to the existing loading thread/provisional child. Host tests validate PRDs,
mode/PCI admission, register sequencing, IRQ races, timeout/fallback and
production FAT/cache throughput through a synthetic master. QEMU and T23
throughput (target 15 MB/s on the 1.3 MB ELF) remain lead-owned evidence.
The existing `ata-fault-blkdebug` gate explicitly selects PIO for its injected
tail-sector fault and records `case=fault_transport transport=pio`; its
preceding hash reads still use detected DMA. This preserves the established
PIO whole-channel quarantine test. DMA fault/fallback is tested at the host
register boundary; native DMA fault injection remains lead qualification.

## F0-40 delayed-error isolation

`storage.writer_error` denotes failure of the shared writer service itself
(for example, its startup allocation), not a device's delayed write/flush
error. Those errors remain sticky in the existing per-device cache state;
writeback revokes only affected FAT volumes and emits a protected
`failure stage=writeback drive=... disk=... result=... scope=volume
fallback=readonly` record on first revocation. Healthy volumes can still
enable writing and persist BOOT.CFG. All-volume shutdown continues checking
each volume and fails on the affected device, retaining dirty state and
never declaring a failed volume clean. No error is cleared or retried by
this isolation policy. The concurrent shutdown test and two-device
write/flush injection are in the [F0-40 audit](../validation/f0-40-reliability-audit.md).

## Open questions

- Whether the 5-second flush window is acceptable for the owner's use
  (Windows 9x used delayed writes too); measured against power-loss tests.
- Whether drive-letter assignment should be configurable in F1 or fixed.
- Whether the T23 and E500 disks support the firmware-configured ATA DMA
  path reliably with their chipsets (F1-44 hardware qualification pending).

## Interfaces required from other contracts

- `boot-memory.md`: FAT32 layout and the cache budget.
- `execution-abi.md`: handle and syscall conventions, `device` priority for
  the writer thread.
- `device-firmware-ownership.md`: ATA/ATAPI controller ownership, IRQ and
  DMA rules, BIOS policy for disks.
- `dos-dpmi-contract.md`: bridge semantics for PSP/JFT/SFT, FCBs and DOS
  compatibility share mode.


## Asynchronous requests (f1-83)

Research checked before implementation, 2026-10-19:
[Microsoft IOS services (P06)](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S2F60.HTM),
[load layers (P07)](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S2F5A.HTM),
[DRP](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S302E.HTM),
[port registration](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S2F87.HTM),
[Microsoft Q196550 (P12)](https://www.betaarchive.com/wiki/index.php/Microsoft_KB_Archive/196550),
[Intel BM IDE 1.0 §§1–3 (P22)](https://pdos.csail.mit.edu/6.1810/2004/readings/hardware/IDE-BusMaster.pdf),
[T13 ATA/ATAPI-6](https://www.read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/ATA-d1410r3a.pdf),
[USB BOT 1.0 §§3.4, 5.3, 6.3](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf),
and [T10 SBC-3](https://www.t10.org/ftp/t10/document.05/05-344r0.pdf).
Q196550 was accessible through indexed article text after direct retrieval
failed. It requires locked request/register/buffer storage and explicit
completion ownership. IOS supplies packet/device lifetime and calldown
services; it does not specify our queue constants. BOT forbids another CBW
before CSW/reset. PRDs permit scattered physical regions, each confined to
64 KiB, with even addresses/counts and zero count encoding 64 KiB.

Repository comparison: blkdev callbacks and ATA channel/USB host mutexes
serialize whole synchronous transfers; cache drops its mutex for transport,
but VFS retains its volume reservation through FAT work. The implementation
adds bounded requests and controller arbitration above the existing register
engines. Transport completion/stop proof remains authoritative: an issued
error is never retried, and no pin retires in a top half. FAT barriers and
f1-82 dirty policy remain synchronous and unchanged. Queue completion runs
in task context; IRQs only acknowledge, capture status and wake the existing
transport waiter. CiukiOS numerical policies are documented separately from
historical Windows behavior.

The common pool contains 32 resident request records, allocated once and
charged (including page rounding and completion events) to ledger owner
`io_request`. Preparation pins the queue lifetime; only terminal requests
can be released. Destroy refuses prepared, queued, issued and unretired
completed requests. Resident controller queues have a sleeping kernel
executor; host and synthetic-controller fixtures drive the identical
one-command executor from waiters. Submission is separate from waiting.
A generation fence is checked before and after transport. Queued cancellation
issues no command; cancellation after issue waits for the existing stop proof
and cannot turn a completed write into an assertion that nothing was written.

Scheduling policy is CiukiOS policy: demand reads carry a 20 ms service
objective, speculative reads precede background writes, and FIFO order breaks
class ties. Work aged 100 ms earns promotion for one command; a split bulk
request must earn promotion again after its last service, so an aged large
write cannot consume all remaining commands ahead of new demand reads.
Overlapping writes and per-device flush/dependency frontiers retain ordering;
dependency work inherits demand rank. A flush never jumps its predecessors.
The initial command cap is 65536 bytes; a measured command exceeding 10 ms
reduces subsequent commands proportionally, rounded down to whole sectors.
The lower transports retain their existing command/flush/packet deadlines.
Queue admission expires after 30 s (data) or 60 s (flush); exceeding a service
objective never authorizes replay of an issued command. USB's synchronous
facade retains its 128-sector admission limit. A common larger request can
still be split into commands within that cap.

Merging requires adjacent LBAs and contiguous virtual bytes, identical
operation/device/generation/class/frontier, and the same explicitly supplied
live pin owner. Independently owned buffers never merge. A command records
the DMA regions and their pin tokens: IDE first tries a pinned contiguous
translation, then individual pinned page fragments, finally its permanently
pinned bounce run on mapping/pin refusal. PRDs retain even addresses/counts,
64 KiB boundaries and a final EOT flag. Direct tokens retire only after
completion or existing hardware-stop/revocation proof, in thread context.
The USB storage adapter pins its shared three-page HCD arena for the host
lifetime; packet engines copy through that arena, which can remain referenced
by HID after a disk detaches. Media removal cancels its queued work and stops
its executor without waiting under the USB host reservation.

Mount adapters keep their write gates, tracing and counters. Executing task
context carries priority/dependency frontier through their synchronous
calldowns; offset-only partition views can resolve directly to their parent
before admission. The 5 s `[storage] queue` record includes class depths,
oldest queued/issued age, command/merge/split/cancellation counts, timeouts,
backpressure and current command cap. Record publication stays outside request execution/waiting, avoiding UART
backpressure on the data path. A generation-safe deferred timer reports submit-only
requests while an executor is stuck in a lower command: its callback tries
the queue lock without blocking and retries after 1 ms on contention. It
allocates nothing and uses the f0-46 nonblocking work-consumer record path.
Worker exit cancels the timer and retains queue lifetime until any in-flight
callback finishes.

VFS metadata tickets now retire before `VFS_IO` drops namespace for transport.
A separate mutation guard retains the existing FAT serialization and lifetime
boundaries through publication/observers; it does not share or remove a FAT
barrier. An already-open unrelated legacy file reader can pass an identified
writer: it pins its description/position, takes an immutable geometry/entry
snapshot and performs its data I/O without a volume ticket. Same-file access,
close/dup/table teardown, other mutations and removal remain excluded as
required. Read diagnostic workspaces are private; revocation is published
under reservation only if both mount and media generations still match.
Cold opens and metadata operations retain their existing admission rules.

`tests/host/fs/test_fairness.c` runs the 10 MiB / 1 MiB/s source versus
20 MB/s destination, with zero modeled seek/read-setup cost, 1 ms write-command setup and 2 ms flushes, a
16 KiB reader every 20 ms, and the unchanged 8 MiB cache. Arrival latency
includes scheduler lateness against the scheduled arrival. The former
volume-ticket reader is an explicit fixture-only negative control. Variants
cover cold/hot data, fragmented reader and copy chains (a valid filler file occupies alternating
clusters), every cache slot initially dirty,
concurrent fsync, an independent read error, and queued cancellation. The
mixed-device variant creates and reads an actual FAT16 source file through
production VFS/cache/BOT/UHCI over the existing peripheral model, with a
separate paced producer. All positive variants assert zero missed reads,
maximum arrival latency <=20 ms, source throughput within 10%, final file
bytes/length/chain termination and equal FAT copies after commit. The normal
host suite separately retains full FAT12/16/32 crash/fsck/mtools qualification;
each variant also runs the former ticket policy as its negative control.
After commit, a temporary sparse FAT32 host fixture with specification BPBs
is checked by fsck.fat and COPY.BIN is independently extracted/compared with
mtools, then both temporary files are removed. This host timing model is not
physical timing evidence.

## Publication groups

F1-84 research (2026-10-19): read the primary [DDK IFSMgr ring-0 I/O
interface](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S312A.HTM)
(P10), [Microsoft FAT specification](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf)
(P23, sections 4, 5 and 7), and Mitchell's [Inside the Windows 95 File
System](https://pdfcoffee.com/inside-the-windows95-file-system-3-pdf-free.html)
(P26, VFAT and VCACHE chapters). P10 distinguishes context-owned record
locks and file handles. P23 specifies allocation and validates LFN ordinals
and checksums; FSInfo is advisory. Mitchell describes client-keyed cache
blocks, held/dirty blocks and memory-manager cooperation. None establishes
a complete asynchronous power-loss publication protocol for Windows 98.
The P29/P30 closure in `win98-class-resource-management.md` confirms commit
semantics and an optional removable write-behind setting, without a universal
installed default. Decision: preserve synchronous FAT barriers and the owner's
conservative removable write-through policy. No asynchronous default switch,
on-disk format change or journal is justified by these sources.

Groups carry increasing numbers within a mounted volume and device generation.
Ordinary growth initializes data, constructs a detached chain, publishes its
link, publishes directory ownership/size, then updates FSInfo. Delete removes
the owning entry before reclamation; cross-directory rename removes the source
before publishing the destination. Existing COW/directory allocation can build
a detached chain before initializing its data, with both durable before an
owning entry/link is published. Clean flags keep their per-copy barriers.
Each group records start, stable completion or failure; transport requests use
the existing f1-83 frontier field. A barrier freezes only that device's write
admission and captures the accepted version. It drains a finite set of dirty
versions and issued writes, then requires a qualified flush or verified disabled
hardware cache. New writes wait without namespace/cache locks and cannot extend
this capture. Reads and other devices continue. This conservative admission
protocol deliberately retains the synchronous throughput limitation.

`cache_write` acknowledges buffered bytes; `cache_barrier_record` acknowledges
stable bytes and returns accepted/durable versions, group, media generation,
cache epoch, counts and incomplete/error state. A background write is not a
successful durable barrier record. `fat_commit` (and native fsync), explicit
sync, durable removal and shutdown use these barriers. Errors remain sticky
per device. Current ordinary FAT writes already include the durable publication
barriers, including on removable media. Surprise removal records incomplete
durability, cancels physical queued work and expires aliases; it cannot flush
absent media or retire DMA pins. f1-83/f0-43 retain issued transport/pin ownership
until stop/completion proof. Replacement media require a new cache epoch.

## Locks

An LFN and its validated 8.3 alias resolve to the same owning short-entry
location and node. Explicit share masks deny read, write, both or neither;
checks are bidirectional. DOS compatibility modes retain their existing
read-only exception and critical/share-error distinction. Owner identity is
the pair `(personality, context id)`; numeric PID/PSP equality across native,
Win32 and DOS grants no privilege. `vfs_table_set_owner` registers a context
before it opens files; unregistered native tables have distinct live-table
contexts. The DOS bridge can adopt this interface without a DOS
API change in this directive. Existing untyped owner release is confined to
the native owner namespace; new clients use `vfs_release_lock_owner`.

Range locks are mandatory, half-open intervals. Read, write and truncate
check overlaps, including through the other personality's ordinary file path.
Only the owning context and description can unlock an exact claim; same-file
independent opens do not bypass it. Duplicate/inherited handles share one
description and position, with the caller's context retained for range claims.
Closing/replacing a handle releases that context's claims on the description;
final description release removes all remaining claims, and table destruction
performs those closes. Typed owner-death release cannot clear another
personality's equal-numbered context. Sharing lasts as long as an inherited
open description. The existing DOS bridge owns its no-inherit open flag policy.

## Mapping identity

The bounded identity table assigns a never-reused file serial to a validated
owning entry, shared by legacy nodes and native descriptions. Generation is
the mount generation paired with the block-device media generation. A page
key is `(volume generation, stable file serial, file-page index)`. Referenced
file or page slots cannot be recycled. A new directory occupant receives a
new serial; rename moves a retained identity, while unlink detaches its name.
The native description pins the common identity through final close. Page
references independently retain identity after close and block safe detach.
Raw/external writes and removal expire identities and page aliases. Writes
and truncate invalidate page metadata before changing bytes, including failed
or partial operations. The current hook conservatively invalidates every
page of the affected file, including a partial EOF page. This prepares the
f0-47 integration; it allocates no mapping data pages and enables no native
file-backed mmap feature.

Before enabling file-backed mappings, f0-47 must bind data pages to this table
and enforce the following protocol:

- MAP_SHARED views and ordinary I/O see the same bytes across processes.
  Subsector cache views share backing or flush/invalidate synchronously under
  the volume's mutation reservation; two dirty copies are forbidden. A dirty
  shared mapping must be reconciled before an ordinary read/write proceeds.
- MAP_PRIVATE writes use private COW and never enter the file's dirty queue.
  File changes invalidate shared aliases; private COW pages retain their
  private contents, subject to the truncate/EOF access policy.
- Populate the last page with file bytes and zero its tail. Tail writes beyond
  EOF do not extend the file or become persistent. Access to a whole page past
  current EOF fails; truncate revokes affected PTEs/TLBs and in-flight fills
  before releasing storage. Extension zeros newly exposed bytes.
- fsync captures dirty shared pages plus the required FAT dependency frontier,
  writes them through the one cache protocol, and propagates sticky errors.
  Close releases a descriptor; mappings and their file identity survive until
  their final unmap. Closing a descriptor alone is not a durability operation.
- Rename preserves mapping keys. An unlinked mapped file retains its chain
  until the final descriptor/mapping reference; replacement cannot acquire
  that old key. Existing chain-lifetime integration must be extended to mapping
  references before enabling those mappings.
- Raw writers must quiesce filesystem users, durably drain dirty state and
  invalidate block and file-page aliases before proceeding. Detected external
  writes expire existing identities; callers cannot keep using stale aliases.
  Surprise removal cancels queued work and invalidates keys. Issued DMA pins
  stay with transport until stop/completion proof; insertion gets a new epoch.

These are implementation requirements for f0-47, not claims of mapped-data
coherence tests in f1-84. Here the tests cover the metadata identity, reference
lifetime and invalidation protocol through real FAT/VFS operations.

F1-84 crash-diagnostic closure: the expanded append matrix can stop after a
live tail is durable and before the larger directory size, leaving an owned
excess chain. A shorter same-index rename can leave an old orphan LFN fragment
or a new LFN checksum that does not match the still-old short name. These are
existing recoverable publication outcomes, not permission for short owned
chains or cross-links. Checked the primary dosfstools 4.2
[`check_file`](https://github.com/dosfstools/dosfstools/blob/v4.2/src/check.c)
and [LFN parser](https://github.com/dosfstools/dosfstools/blob/v4.2/src/lfn.c):
the former distinguishes dropping excess clusters at unchanged EOF from
shortening a file whose chain is too short; the latter discards unmatched LFN
sets. The host classifier now recognizes only the exact new workload's
multiline diagnostics, unchanged EOF and rounded cluster bound. Negative tests
reject short-chain/data truncation, malformed/missing diagnostic context,
unexpected names and any corrupt/cross-linked owner. Both independent FAT-copy
ownership scans and production reboot validation remain mandatory; fsck runs
with `-n` and never repairs these test inputs.

## FAT cluster cursor and write generations (f3-51c)

Research (2026-10-20). Microsoft's MIT-licensed [MS-DOS 4.0 source](https://github.com/microsoft/MS-DOS/tree/main/v4.0/src)
keeps the position per open file in its SFT entry: `SF.INC` defines
`sf_firclus` (first cluster), `sf_cluspos` ("position of last cluster
accessed") and `sf_lstclus` ("last cluster accessed"); `ROM.ASM` `FNDCLUS`
skips clusters from `sf_lstclus` when the target is not before `sf_cluspos`
and restarts from `sf_firclus` otherwise; `DISK2.ASM` `SETCLUS` stores the pair
after each transfer and `DISK3.ASM` `KILLFIL` (truncate to zero) clears both.
The [HelpPC SFT table](https://www.cs.yale.edu/flint/feng/research/BIOS/Resources/assembly/sft.html)
documents the same per-handle "relative/absolute cluster of the last read"
fields for DOS 3.x. Windows 98's 32-bit VFAT source is not public and the
Windows 98 Resource Kit (extracted to `build/rskt/` in the main checkout)
does not describe its per-handle state; the DOS SFT model is the documented
one. Before f3-51c,
`fat_read` resolved every call from the first cluster, so sequential reads
were quadratic in the chain (dev diary 2026-10-20-04).

**Cursor.** `struct fat_cursor` is the last resolved (cluster index, cluster)
pair of one open file, stamped with the entry's first cluster and chain
generation. VFS descriptions (`vfs_description.cursor`), native descriptions
(`file_description.fat_cursor`) and the ELF image loader keep one;
`fat_read_cursor`, `fat_read_stream` and `fat_write_cursor` resolve a target
from the nearest valid pair at or before it (the description's cursor, the
entry's write cursor, else the first cluster) and leave the cursor on the
cluster holding the last byte transferred. Plain `fat_read`/`fat_write` keep
the former behaviour. A cursor is a hint: it is followed only while the entry
names the same first cluster and chain generation, its cluster is valid and
its index is within the volume; bounds and corruption checks are unchanged.
Any error (f1-83 cancellation or deadline, I/O, corruption) empties it. Only
the description's own serialized I/O touches it (`io_active` on the
ticket-free reader path, the handle reservation otherwise); it adds no lock,
wait or allocation.

**Chain generation.** `fat_entry.chain_generation` is a volume-unique stamp
(`fat_volume.chain_stamp`, issued under the volume reservation). A decoded or
created entry receives a fresh stamp, so a copy refreshed from disk (for
example after DOS raw writes, `dos_file_volume.c`) never adopts an older
cursor. Every FAT mutation of a file's chain restamps the caller's entry
before its publication group runs, so a failed group also retires cursors:
the append `FAT_P_LINK` group (`write_span`), COW relocation and truncate
(`replace`, published by the directory group), the reserved-tail cut
(`fat_publish_reserved`, log and pagefile trim), and the `FAT_P_RECLAIM`
release of `fat_remove` and a replaced rename target. An in-place overwrite
does not mutate the chain and keeps cursors valid. A remount starts the stamp
again, but every description and identity of the former mount already fails
with EIO before it could follow a cursor.

**Write generations.** The VFS identity hook records, under `identity_lock`,
the latest change sequence of each changed file location `(volume, mount
generation, directory cluster, entry index)`: write, truncate, rename source
and destination, unlink. The ledger holds 64 locations; evicting one raises a
floor, and an unlisted location reports the floor. Volume expiry (raw
change, removal, detach) drops the volume's rows and raises the floor. Hence
any change after a reading yields a different value; unrelated evictions may
change it too (conservative). `vfs_handle_version` returns the open file's
content version: mount and media generation, drive and drive generation,
location, first cluster, size, modification date/time and that write
generation; read-only providers return `-FS_EOPNOTSUPP`. The DOS game launch
digest cache (`dos-dpmi-contract.md`) is its consumer.

Evidence: `tests/host/fs/test_cluster_cursor.c` in `host_fs_tests.sh`.
On a FAT32/4 KiB-cluster volume holding a 27,336,704-byte mtools-written file
(6,674 clusters), 418 sequential 64 KiB reads cost 2,801,342 FAT lookups
without a cursor and 13,295 with it (1.99 per cluster; half the file 698,413
versus 6,619), with the same SHA-256. A negative control that ignores the
generation reads another cluster's bytes after a truncate reallocates the same
first cluster with a shifted chain.

### fx-16 amendment: the leading contiguous run (2026-10-23)

`FNDCLUS` keeps one position, so a seek before it walks from the first
cluster. DOOM reads its WAD with `lseek` + `read` per lump in the order the
game asks for them (id Software
[linuxdoom-1.10 `w_wad.c`](https://github.com/id-Software/DOOM/blob/master/linuxdoom-1.10/w_wad.c)
`W_ReadLump`), so about half of its reads walked an 11-12 MB chain
(~3,000 links at 4 KiB clusters, about 1 us each on the T23 per the decode
note in `get_copy`), synchronously inside the DOS VM's file service. In
T23 trial e831e685 the unified-DOS IFS calls (INT FEh) took 736 ms of one
5 s full-screen window while the SB16 IRQ waited up to 69 ms
(`validation/fx-16-doom-stutter-crackle.md`). Linux caches contiguous runs
per inode for the same reason
([`fs/fat/cache.c`](https://github.com/torvalds/linux/blob/master/fs/fat/cache.c),
`fat_cache_lookup`, `nr_contig`).

`struct fat_cursor` gains `run0`: chain indices `0..run0-1` are clusters
`first..first+run0-1`. The read path (`read_cluster`, used by
`read_span`) resolves an index inside that run in O(1); otherwise it walks
from the nearest valid pair at or before the target, the run's last pair
included, and extends `run0` while a walk that starts at the run's end
finds `next == c + 1`. Learning costs no more lookups than the walks the
reads already make (at most one pass over the chain in total). `run0`
belongs to the chain generation, not to the position: `cursor_set` keeps
it while the entry's first cluster and chain generation are unchanged, and
every chain mutation's restamp retires it with the cursor. Writes and
`fat_read` without a cursor are unchanged.

Evidence (`test_cluster_cursor.c`, `doom_lump_reads`): 400 lump reads of
1-48 KiB at scattered offsets of the 6,674-cluster file cost 895,017 FAT
lookups before (2,204 per backward seek, about 2.2 ms on the T23) and 9,579
after (6.2 per backward seek, 18 at most once the run is known); the
earlier sequential, invalidation, write-generation and error cases are
unchanged.

## Cache sizing and read-ahead

F1-85 research (2026-10-19), before implementation: Microsoft's
[Q108079](https://jeffpar.github.io/kbarchive/kb/108/Q108079/) documents
KiB-valued MinFileCache/MaxFileCache for WfW/95, without establishing Windows
98 defaults. Russinovich and Cogswell's original
[VCMon investigation](https://jacobfilipp.com/DrDobbs/articles/DDJ/1996/9605/9605c/9605c.htm)
connects resizing to hits, misses, free memory and page faults. Mitchell's
[Inside the Windows 95 File System, chapters 10–11](https://pdfcoffee.com/inside-the-windows95-file-system-3-pdf-free.html)
describes page commitment and relinquishing free, then unheld clean LRU
pages. Its reverse-engineered Windows 95 constants are not Windows 98 policy.
The P01 [original TechNet endpoint](https://technet.microsoft.com/en-us/library/cc768196.aspx)
and [archived excerpt](https://www.oocities.org/~budallen/98performance.html)
could not be retrieved in this worktree's research session. The reviewed
f0-42b closure (P28/P31/P36, Windows 98 Resource Kit ch. 26/28/10) supplies
the established dynamic growth/shrink and read-ahead roles; the licensed
media/extracted book are absent from this checkout.

Decision: implement C4's 2/8/32 MiB bounds, 100 ms/two-window feedback,
256 KiB steps and 16–64 KiB stream windows as **CiukiOS policy**. No
ReadAheadThreshold compatibility setting is exposed: the closure did not
establish its units, default, range or generation rule. Retain C1's formula
and durability protocol and M1's reserve/hysteresis. F1-84 page identities
retain references, not extra data frames; cache residency is counted once.

Canonical boot chooses `min(8 MiB, ceiling, free_NORMAL - high)` rounded to
pages, refusing storage admission if the 2 MiB floor cannot fit above M1's
reserve. The ceiling is `min(32 MiB, usable_NORMAL/8)`. `cache_init` retains
its 8 KiB–32 MiB standalone contract; adaptive mode is explicit and only
canonical storage enables it. Data, descriptor, scan-workspace and writeback
pages all count. Growth uses NORMAL pages and transfers their ledger owners;
shrink returns DATA/META pages. Fixed caller-owned cache/open-description
structures remain in their existing kernel ownership. File/page identity
references add no data allocation and are never counted as additional frames.

The storage worker samples every 10 ms, under its existing shutdown
reservation. A completed 100 ms window with demand misses **and demand reuse**
qualifies as useful. Two consecutive qualifying windows permit at most 64
additional physical pages (data plus descriptors); late samples do not invent
missing windows. Growth requires free NORMAL greater than high plus the
maximum step, rechecks the live ledger before allocations, and stops on
pressure, dirty debt above C1's budget, or resolved speculative waste of at
least 16 KiB exceeding useful speculative bytes. C1's existing
`min(1 MiB,C/8)` formula is preserved as C changes. A partial allocation or
one-page descriptor remainder need not use the entire permitted step.

Pressure cancels plans and visits at most 64 data-page headers per call,
rotating past blocked pages. Reclaim checks pins, fills/waiters, dirty data
and errors; it queues healthy dirty debt and never issues transport. It
releases at most 64 total pages, including now-empty descriptors, and stops
starting page preflights after 500 us, leaving margin to the 1 ms CPU target.
It retains the inline data page and fixed workspaces, reuses descriptor holes
on growth, and never moves a surviving descriptor. Snapshot counters expose
the unique unreclaimable-data/data-page fraction and overlapping pin, dirty
and error reasons, plus per-slice visits, pages and maximum measured CPU.
The 1 ms physical target still needs hardware timing qualification.

Each physical FAT open description owns a shared-cache stream record;
duplicated/inherited handles share it. Two adjacent completed reads start a
16 KiB byte-valued window; consumption doubles it to at most 64 KiB. FAT
plans a contiguous range through validated cluster links, volume bounds and
whole sectors before EOF. Existing logical image and read-only providers keep
their provider paths. Demand callers queue plans without issuing speculative
data I/O. The worker uses C3's `BLK_SPECULATIVE` read class, below demand and
above background writes as required by the existing C3 scheduler. The
preallocated, budgeted 32 KiB writeback bounce buffer serializes each command;
a 64 KiB window can use two commands. No second data cache or new arena exists.

Resident and in-flight speculative bytes share the `C/8` cap. Dirty/clean
shared hits split speculative runs; only clean, unpinned victims are used.
Seek, final close, same-file publication, raw identity expiry, competing
streams and pressure cancel generations and reset sequential evidence.
Cancelled tags remain budgeted until demand, eviction or close retires them;
consuming cancelled data or overwriting unused prefetch counts as waste.
Sequential reads on the same description can join an issued prefetch.
Close drops unused speculative hash identities and waits for issued transport
retirement with namespace released, retaining its publication guard, before
removing the stream;
queued cancellation issues no command. Generation/epoch checks prevent stale
completion publication. An already issued physical command retains its
existing bounded transport/stop protocol; it cannot be physically preempted.
Host random-read evidence establishes absence of queued speculative delay,
not an unmeasured hardware latency bound.

### F5-06 Amendment 1 consumers of common locks

Win32 native descriptions now retain the F1-84 common identity and register
claims as `(VFS_OWNER_WIN32, PID, description)`. The common table admits 64-bit
half-open ranges and shared locks. Shared claims permit all reads and prohibit
all overlapping writes; exclusive claims exempt only the owning PID and
open description. A shared claim may overlap its own exclusive claim; exact
unlock releases the exclusive claim first. Ordinary native/DOS I/O checks the
same table. Native syscall numbers, records and errno conventions are unchanged.
The original 32-bit DOS lock entry point keeps its overflow check.

The native share observer also checks Win32 read/write/delete access and deny
masks. Delete sharing lasts through inherited description references. Win32
pending deletion retains the live file/identity for existing opens, rejects new
opens and removes the name/chain on final close. A native rename changes the
ancestry used by subsequent Win32 handle writes, truncate, section writeback
and delete-on-close; System protection cannot be bypassed by retaining a handle.

### fx-23 amendment: background I/O and the storage writer (2026-10-23)

Trial evidence (T23, image f441c5bd, private records): during the fx-4b
check of `UNIFIED.IMG` (27.7 MB, 26 s) the storage writer (P_DEVICE) used
10–23 % of the CPU while the idle-class verifier itself barely registered;
after the DOS launch the writer used 24–40 % with 1,200–1,500 device-budget
overruns per 2 s and about 18,000 context switches per second, the ATA queue
was empty, the boot log made no progress for 55 s, and Files waited 19.4 s
for its window.

Reference behaviour:

- Windows 98 runs disk maintenance (ScanDisk, Disk Defragmenter,
  Compression Agent) through Task Scheduler "while the computer is idle" and
  can stop it when the computer is in use (Resource Kit ch. 10, "Using Task
  Scheduler"; ch. 26, task configuration "how to run the program if the
  computer is idle, is in use").
- Windows NT-class systems lower I/O and memory priority, not only CPU,
  for background work: "it is not sufficient to adjust the CPU scheduling
  priority; even an idle CPU priority thread can easily interfere with system
  responsiveness when it uses the disk and memory"
  ([SetThreadPriority, THREAD_MODE_BACKGROUND_BEGIN](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreadpriority)).
- Linux's idle I/O class "will only get disk time when no other program has
  asked for disk I/O" ([ionice(1)](https://man7.org/linux/man-pages/man1/ionice.1.html)).
- The Windows lazy writer runs on a timer ("spawns a process every second
  called a lazy writer", [File Caching](https://learn.microsoft.com/en-us/windows/win32/fileio/file-caching)),
  not on every cache event.

CiukiOS rules from this amendment:

1. **Background requests.** `struct vfs_request.background` marks idle-class
   maintenance I/O (the DOS image check, the fx-17b free count). Its reads
   rank after every other queued request (`blkreq.c rank`, the 100 ms age
   promotion still bounds the wait), `fat_read_stream` queues no read-ahead
   for it (the storage writer never reads at device priority on its
   behalf), and the blocks it fills stay at the cold end of the LRU while its
   hits are not promoted (drop-behind): a scan larger than the cache reuses
   its own slots and leaves the foreground working set resident.
2. **Writer wake.** The storage writer sleeps on `cache.writer_kick`, not on
   the shared `cache.changed` event. It is kicked when write-back is requested
   or becomes due now, and when a stream queues read-ahead whose window is not
   already resident. Fills, unpins, reservation releases and completed writes
   no longer wake it; future deadlines, the boot log and the census are found
   by its 10 ms poll.
3. **Free count yields.** A background free-count step runs under a
   background request and is deferred by 20 ms while any foreground caller is
   queued for that drive (`census_yields`).
4. **Diagnostics.** The writer reports `[storage] writer_busy passes=…
   kicks=… window_ms=…` when it ran more than 600 passes in a 2 s window
   (the poll alone is about 200), and `[storage] vfs_hold_long drive=… ms=…
   task=… op=… site=… waiters=…` once for each drive reservation still held
   after 2 s (the release-time `vfs_hold` record is budgeted and never appears
   for a hold that outlives the observation).

Host evidence: `tests/host/dos/test_fx23_contention.py` (production
cache/FAT/VFS: a Files listing every 2.5 MiB of a 6 MiB check through a
2 MiB cache reads 0 sectors, 2,668 before; 0 writer read-ahead commands for
the check, 178 before; 5,000 foreground fills wake a sleeping writer once,
2,901 times before), `tests/host/fs/test_blkreq.c` (`background_readers`) and
`tests/host/usb/storage_test.c` (the census step yields to a waiting caller).

#### fx-23 follow-up: a refused publication step is not starved (2026-10-24)

The ~50 s CREATE of the f441c5bd trial was a FAT transaction step
(`transaction_guard_wait`) waiting for C: to have no reservation, no queued
ticket and no in-flight reader at one instant (`guard_busy`), while ordinary
ticketed C: operations kept overlapping. Its gate meanwhile refused every
open-file read on C:. Rule from this follow-up:

- When a step is refused, the drive's `gate_ticket` records the next ticket.
  Tickets issued from then on are not admitted until the step has run. A
  caller already holding drives and the owner of the drive's writer turn are
  exempt, because the step may need that turn back.
- `guard_busy` counts only the tickets issued before the gate. The step takes
  the drive with an express reservation (no ticket, so its release advances no
  queue) and then opens the gate. Ticket admission also requires the drive not
  to be reserved.
- A step never queues behind its own gate: it opens the gate before taking
  a ticket (`guard_lock`, non-express entry) and before waiting for the
  writer turn (`transaction_guard_resume`). The turn's holder may be queued
  behind a gated ticket.

Host evidence: `tests/host/dos/contention_test.c` section 4. With three
back-to-back C: operation streams the stamp write waited the whole 4 s run
before, and 523 ms after. `tests/host/fs/test_t23_holds.c` covers the
turn-holder exemption.
