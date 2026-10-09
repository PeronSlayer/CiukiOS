# f1-01 implementation and host validation

2026-10-10. Implementer worktree only. The lead owns review, integration,
QEMU/hardware qualification, diary entries, commits and publication. No files
outside the directive's source/test/script paths are changed. Generated files
are confined to `build/host/fs/`. No Git commands, QEMU, image builds or systemd
scopes are needed by this runner.

## Research and repository comparison

Research preceded the implementation, using the contracts' original sources:

- [Microsoft fatgen103](https://www.pcjs.org/documents/papers/microsoft/MS_FAT_OVERVIEW_103-2000-12-06.pdf),
  allocation geometry, packed FAT12 entries, reserved FAT entries/flags,
  LFN sequencing/checksums, and basis-name/numeric-tail generation.
- [Microsoft FAT format specification](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf),
  BPB, root, FSInfo, directory and UCS-2 naming layout.
- [Linux v6.12 MBR parser](https://github.com/torvalds/linux/blob/v6.12/block/partitions/msdos.c),
  EBR data starts relative to the current EBR, links relative to the container.
  Behavior reference only; no Linux implementation was copied.
- [dosfstools checker](https://github.com/dosfstools/dosfstools/blob/master/src/check.c),
  used as an independent on-disk checker, without repairs.
- [Microsoft CP437 mapping hosted by Unicode](https://www.unicode.org/Public/MAPPINGS/VENDORS/MICSFT/PC/CP437.TXT)
  and [Unicode 16.0 data](https://www.unicode.org/Public/16.0.0/ucd/UnicodeData.txt).
  `path.c` tables were generated from Python's CP437 codec and Unicode 16.0
  single-code-point uppercase mappings. Multi-code-point expansions do not fit
  UCS-2 case mapping. UCS-2 LFNs on imported volumes can be read; creation
  rejects a glyph that cannot be represented in the CP437 alias rather than
  silently replacing it. Legal LFN punctuation disallowed in an alias uses the
  specification's `_` basis conversion and numeric tail.

Read the three named contracts, every `src/kernel/include/ciuki/*.h`,
`core/kheap.c`, and `scripts/build_kernel.py`. Semble confirmed the allocator
and task/IRQ primitives. `kmalloc` has seven size classes and an eight-byte
header: requests above 2,040 bytes fail. Consequently the cache pool uses
`pmm_alloc`/`P2V`/`pmm_free`, not a large `kmalloc`. Small portability allocations
retain the heap limit. Cache pages and scan scratch are allocated at init; I/O,
mount scanning, namespace operations and handles allocate nothing afterward.
Caller-owned VFS/table/volume objects must have persistent storage, not live on
an 8 KiB kernel stack. Kernel compilation also emits `.su` stack-use records;
large alias-collision scratch is in the volume, and directory ancestry is
nonrecursive. Linking/running a complete kernel is not claimed.

F0 is UP with a nonpreemptible kernel and no sleeping mutex abstraction. The
port's cooperative task mutex changes its ownership flag in a short IRQ-save
section and yields while contended. Sector I/O, scans and flushes run with
interrupts enabled. Host builds use pthread mutexes. IRQ callers are forbidden.
Lock order is VFS namespace, cache, device. Direct FAT callers must serialize
operations (including mount/unmount) and must not mount overlapping extents.
`vfs_attach` enforces one shared cache and nonoverlapping attached extents.

PIT time supplies the cache's monotonic clock. F0 has no exported calendar
clock: `fs_set_calendar_clock` is an optional boot-time hook for an allocation-
free provider of DOS timestamps. Without it creation/modification uses
1980-01-01, rather than inventing a wall clock from PIT uptime. Tests install a
fixed 2026-10-10 clock. Explicit VFS timestamp get/set remains available.

## Implementation choices

- Frozen interfaces are the actual `src/kernel/fs/blkdev.h` and `vfs.h`.
  Synchronous block requests return negative `FS_E*`; capacity is a sector
  count. A non-null flush callback certifies qualification. Unknown cache state
  never permits writes. Enabled caching requires flush; verified disabled
  caching does not. A quarantined device receives no new request from cache.
- One default 8 MiB pool includes sector buffers, LRU/hash metadata and mount
  scan workspace (1/16 of the pool, up to 1 MiB). The cache is configurable from
  8 KiB to 32 MiB; insufficient scan workspace returns `ENOMEM`. Device error
  records remain sticky until an explicit new-media invalidation. Failed dirty
  blocks are not reused; they do not prevent other devices using other blocks.
  `cache_writeback_tick` is the five-second writer-thread integration hook.
- The ownership scan counts free clusters itself and never trusts FSInfo.
  It detects short/looping/cross-linked owned chains, scans directories without
  recursion, and mounts lost-allocation volumes read-only. Dirty/error flags
  remain read-only even if a scan finds no live-chain corruption; repair/check
  approval is deliberately a host operation in F1.
- File changes use replacement chains. All new FAT copies are durable before
  data; data before the owning short entry; old chains are released only after
  its durable switch; FSInfo is last. This also safely handles packed FAT12
  links. Overwrite/truncate can require spare space for a whole replacement
  chain and can therefore return `ENOSPC` on a nearly full volume.
- Directory growth allocates/zeros the new cluster durably before linking it
  into the existing directory. Before publishing past an end marker, newly
  exposed slots and a replacement terminator are cleared durably while the old
  terminator still hides them. Foreign stale bytes can never become owners.
- LFNs are made durable before the sole owning short entry. Deletion unlinks
  that short entry before releasing its chain. Cross-directory rename unlinks
  the source before publication and fixes directory `..` while unreachable.
  Namespace serialization and on-disk ancestry checks prevent alias-based
  directory cycles. Open files and cwd ancestors cannot be removed/renamed.
- VFS storage bounds: 26 drives, 64 nodes, 128 descriptions, 64 handles per
  table, 16 caller-owned tables; exhaustion returns a defined error. Independent
  opens share a node; duplicate/inherited handles share a description and
  position. Symmetric share denial is checked against existing descriptions.
  Byte-range locks and DOS compatibility mode retain their later contract phase.

## Contract problems and explicit limits for lead review

These are not silently waived acceptance conditions:

1. **Arbitrary torn sectors cannot be detected on standard FAT.** There is no
   sector checksum for file data or ordinary directory sectors. Mirror and
   structural checks catch the exercised torn metadata cases; they cannot
   distinguish every valid-looking old/new mixture. A universal detection
   guarantee requires an additional integrity format or a narrower failure
   model. Invalid BPB/FSInfo signatures refuse mount as the mount contract
   requires; they cannot simultaneously guarantee a readable read-only mount.
2. **The enumerated crash outcomes are incomplete.** Durable LFN-before-short
   publication and unlink-before-chain-release can leave orphan LFNs; updating
   FSInfo last necessarily allows stale free-count hints. A packed FAT12 entry
   interrupted between sectors can leave an unowned allocation fragment with
   an invalid next value, even in matching FAT copies. Tests explicitly classify
   these alongside lost allocation, dirty flags and mirror divergence, and fail
   on every unclassified fsck diagnostic. Both raw ownership scans must still
   show zero cross-links and zero malformed *owned* chains. The lead must decide
   whether to clarify the contract's “only allowed outcomes” wording; these
   passing tests do not by themselves satisfy its narrower literal list.
3. **Strict in-place same-directory rename has a capacity limit.** The short
   entry remains at the same index. A longer LFN requires enough immediately
   preceding deleted slots; otherwise rename returns `ENOSPC`, even if there is
   room elsewhere. Arbitrary relocation needs permission for the same
   unlink-first approach used across directories, or a recovery protocol.
4. **Imported FAT12 directory split tails:** growing an existing directory whose
   live tail FAT entry spans sectors returns `EOPNOTSUPP`. Updating that live
   link cannot be atomic with a one-sector device contract. New directories
   avoid these tail clusters. File chains, including split entries, are handled
   through replacement chains. General directory relocation/recovery is a
   remaining design issue, not an untested claim of support.
5. **FAT32 active-only mode:** valid volumes with BPB mirroring disabled are
   readable using their selected active FAT, but remain read-only. The specified
   “write every FAT / divergence is read-only” policy does not define writable
   handling of intentionally inactive copies.

The implementation is reviewable and host-tested, but these limits must be
resolved or explicitly accepted by the lead before claiming the full F1 gate.

## Reproduction and evidence

Run from this worktree:

```sh
scripts/test/host_fs_tests.sh > build/host/fs/results.log 2>&1
```

The script builds production code with clang C17, `-Wall -Wextra -Werror`, ASan
and UBSan. LSan is disabled because it fails under this sandbox's ptrace
restriction; leak-check qualification is not claimed. Kernel object checks
extract the exact `CFLAGS` AST literal without importing/executing the kernel
builder or invoking Git. They also reject compiler runtime helper dependencies.

`mkfs.fat -F 12/16/32`, `mcopy` and `mmd` generate the fixtures. Tool versions,
geometry, seed, SHA-256, and initial listings/checks are in `fixtures.json`.
Sizes: FAT12 12,582,912 bytes; FAT16 16,777,216; FAT32 41,943,040. Sector size
512; sectors/cluster 8, 2, 1 respectively. These are filesystem test data, not
boot images. A single `work.img` is reused and removed for clean workloads.
Fault replay uses per-sector in-memory undo, never whole crash image copies.

Four deterministic persistence modes cover immediate writes, total loss of
pending writes, reverse-order suffix persistence and reverse-order alternating
persistence. Cuts cover each submitted sector, each sector persisted during a
flush, and each barrier completion. Every cut discards the cache, reopens a
fresh mount, scans **both FAT copies independently** via a raw image oracle,
and runs `fsck.fat -n` without repairs. Diagnostics are retained in
`crash-fsck.log`; `classify_crashes.py` rejects unknown diagnostics. This is
exhaustive over the exercised workloads and four schedules, not over every
possible disk reorder permutation or every workload.

Read tests include malformed/orphan LFNs, aliases, non-ASCII CP437, case,
relative paths/cwd, attribute masks, invalid UTF-8/UCS-2 and FAT12 boundary
entries. Clean writes include 0 bytes, one cluster, 4 MiB, overwrite, shrink,
extend/zero-fill, directory growth, numeric-tail collisions, exact 13/26-unit
and 255-unit LFNs, rename/delete, sharing, duplicate/inherited handles,
concurrent shared-position reads, metadata and 64-bit/4 GiB limits. Cache tests
check age, LRU pressure, multiple devices, barriers and sticky delayed errors.
Fault tests include BPB, flags, mirror divergence, loops, torn FAT sectors,
read/write/flush failures, unknown durability, ENOSPC and stale directory slots.
Partition tests cover relative logical starts, signature, loop, overflow,
illegal overlap including EBR sectors, and protective GPT.

Every clean workload requires `fsck.fat -n` exit zero. An independent mtools
recursive listing and `mtype` reads must match the driver's case-preserved
names, sizes and FNV-1a hashes. The runner checks both logical and allocated
artifact bytes against 200,000,000. See `REPORT.md` for the final exact output.
