# Directive f1-01: FAT12/16/32 with long names, block cache and VFS core

- **Step:** F1. **Contracts:** `docs/design/vfs-storage-contract.md`
  (layers, namespace, open-file model, cache and write ordering, mount and
  errors), `f1-acceptance.md` (probes `fat-read`, `fat-write`, `cache`,
  `mount-crash`, `partition`; T0 rules), `boot-memory.md` (image layout).
- **Implementer:** Codex, `gpt-6-astra`, effort `xhigh` (filesystem write
  paths and crash safety).
- **Worktree:** `wt/f1-fat-vfs`. Files: `src/kernel/fs/` (new: `blkdev.h`,
  `cache.c/h`, `partition.c/h`, `fat.c/h`, `vfs.c/h`, `path.c/h`),
  `tests/host/fs/` (host tests and fixtures generator),
  `scripts/test/host_fs_tests.sh`. Do not touch the ATA driver (another
  directive), probes, loader, runner or contracts.

## Design constraints

- **Host-first.** Everything compiles for the host (clang, ASan/UBSan)
  against a `blkdev` interface implemented by a file-backed fake with
  fault injection (`fail read/write at sector N`, `cut after write K`,
  `reorder between barriers`, `torn sector`), and for the kernel with the
  kernel's allocator and locks behind a small portability header
  (`fs_port.h`: alloc/free, lock/unlock, panic, time). No kernel-only code
  paths in the FAT logic.
- **`blkdev` interface** (freeze it; the ATA directive implements it):
  `read(dev, lba, count, buf)`, `write(...)`, `flush(dev)`, `capacity`,
  `sector_size` (512), `write_cache_state` (enabled/disabled/unknown),
  `quarantined` flag; errors `EIO`, `EINVAL`, `EQUARANTINED`.
- **Cache:** one write-back cache for all devices, default 8 MiB, LRU,
  durability barrier = all earlier writes complete + `flush` (or verified
  disabled write cache); delayed errors kept for flush/unmount; writer
  thread hook (`cache_writeback_tick`) for a 5-second dirty limit.
- **FAT:** one driver for FAT12/16/32; LFN read and write with checksums;
  short-name generation per fatgen103 with collision checks; UTF-8 in the
  API, UCS-2 on disk, CP437 short names, unmappable names rejected; mount
  checks and read-only fallback exactly as the VFS contract lists; chain
  walks bounded by the cluster count; FAT[1] clean/error flags with the
  FAT16/FAT32 masks; write ordering (FATs → data → directory → FSInfo),
  cross-directory rename that unlinks the source first; 64-bit positions
  with `EFBIG` above 4 GiB − 1.
- **VFS:** nodes shared per open file, reference-counted open descriptions
  (position shared by duplicated handles), handle tables, share modes
  (deny none/read/write/all), paths case-insensitive and case-preserving
  with `\` and `/`, drive letters (`C:`) mapped to mounted volumes, 259-char
  limit, the operations the DOS bridge will need later (open/create with
  attributes, read/write/seek, close, find-first/next with masks, attrib,
  times, rename, delete, mkdir/rmdir, cwd per drive, free space, commit).
- **Partitions:** MBR with primary and extended (EBR-relative logical
  starts), bounded walk, overlap and capacity checks, GPT protective entry
  refused.

## Host tests (mandatory)

`scripts/test/host_fs_tests.sh` builds and runs: fixtures made with
`mkfs.fat -F 12/16/32` (record tool versions, geometry, seed) and `mtools`;
read tests (names, aliases, sizes, hashes, orphan/bad-checksum LFN
entries); write workload (0-byte, one-cluster, 4 MiB files, directory
growth beyond one cluster, LFN alias collisions, rename/delete) followed by
`fsck.fat -n` exit 0 and `mtools` listing equality; crash injection that
cuts after every sector write and barrier, then remounts independently and
runs an ownership scan proving no cross-links (lost chains, dirty flags and
interrupted FAT-copy sync are the only allowed outcomes); torn-sector and
diverging-FAT-copy cases forcing read-only; ENOSPC and failed-write cases;
partition fixtures (valid, loop, overflow, overlap, protective GPT). All
artifacts under `build/host/fs/`, never `/tmp`; total under 200 MB.

## Acceptance by the lead

Diff review; all host tests pass under ASan/UBSan; the code also compiles
with the kernel flags (`scripts/build_kernel.py` CFLAGS) when `fs_port.h`
targets the kernel; no dynamic allocation in I/O paths beyond the cache's
pool. Reply with: files, the frozen `blkdev` and `vfs` headers, test
counts and results, fixture sizes, and any contract problem found.
