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
- This contract owns the DOS compatibility-mode and sharing/locking matrix;
  it MUST be written here before the F4 bridge is implemented.
- Share modes (deny none / read / write / all) are checked at open against
  every existing handle of the node, with DOS compatibility-mode semantics
  provided by the bridge.
- Byte-range locks are per node, owned by a handle, released when the handle
  closes or its owner is torn down (F3, needed by INT 21h `5Ch`).

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

Any DMA error or 30-second timeout stops the master, records a sticky
per-device DMA quarantine, and returns EIO without replaying the failed
read/write. Later independent requests use PIO only when the bus master
and drive are idle. If stop cannot establish idle (or quiescence intervenes),
retain the existing whole-channel quarantine. Late IRQs cannot start DMA
or resurrect a generation. Quiescence stops DMA before proving idle and
retains all claims on uncertainty.
An IRQ before Start without an ATA abort error cannot prove completion of
the issued drive command; it returns EIO and quarantines the channel.
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
