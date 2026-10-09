# f1-01 handoff report — 2026-10-10

The FAT/VFS/cache implementation is ready for the lead's diff review. The host
matrix passes, but full directive acceptance is **not claimed**: the contract
limits below require the lead's decision. No Git commands, commits, QEMU,
`make build-full`, systemd scopes, or edits to contracts/drivers/runner were made.

## Results

- **14 test groups, 86,673 assertions, zero failures** under ASan/UBSan.
- **2,106 crash cuts:** FAT12 959, FAT16 561, FAT32 586; four persistence
  schedules, including cuts during flush persistence and at barriers. Both FAT
  copies scanned independently after each cut: zero cross-links and zero corrupt
  owned chains. `fsck.fat -n` ran at every cut; zero unclassified diagnostics.
- Three clean workloads: **`fsck.fat -n` exit 0** and exact independent mtools
  names/sizes/content-hash equality (63, 30 and 25 entries).
- **Six production translation units compile** with the exact
  `scripts/build_kernel.py` CFLAGS; no compiler runtime helpers are required.
- LeakSanitizer is not qualified: its ptrace requirement fails in the sandbox.
  ASan and UBSan remain enabled with halt-on-error.

## Files

- `src/kernel/fs/blkdev.h`
- `src/kernel/fs/cache.c`
- `src/kernel/fs/cache.h`
- `src/kernel/fs/fat.c`
- `src/kernel/fs/fat.h`
- `src/kernel/fs/fs_port.c`
- `src/kernel/fs/fs_port.h`
- `src/kernel/fs/partition.c`
- `src/kernel/fs/partition.h`
- `src/kernel/fs/path.c`
- `src/kernel/fs/path.h`
- `src/kernel/fs/vfs.c`
- `src/kernel/fs/vfs.h`
- `tests/host/fs/VALIDATION.md`
- `tests/host/fs/classify_crashes.py`
- `tests/host/fs/fake.c`
- `tests/host/fs/fake.h`
- `tests/host/fs/fixtures.py`
- `tests/host/fs/kernel_compile.py`
- `tests/host/fs/scan.c`
- `tests/host/fs/scan.h`
- `tests/host/fs/test_fs.c`
- `tests/host/fs/verify.py`
- `tests/host/fs/REPORT.md`
- `scripts/test/host_fs_tests.sh`

The implementation adds only files in the directive's allowed paths.
[VALIDATION.md](VALIDATION.md) records primary-source research, allocator/locking
integration, ordering decisions, test methodology and limits. All runtime test
artifacts are under `build/host/fs/`; the lead must write the Italian diary entry
outside this implementation scope.

## Frozen interfaces

The exact headers below are the handoff interface for ATA and kernel callers.
Errors are negative `FS_E*` from `fs_port.h`; numeric quarantine error is 200.
Capacity is a sector count, requests are synchronous, and a non-null flush
callback means the backend has qualified its durability behavior.

### `blkdev.h`

SHA-256: `5cc31f963a0ccc983ab9b416a9d62ea7f63b923564a12ce4c743d639a738d770`

```c
/* Frozen f1-01/ATA interface, v1. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_BLKDEV_H
#define CIUKI_BLKDEV_H
#include "fs_port.h"
#define BLKDEV_SECTOR_SIZE 512u
enum blkdev_cache_state { BLKDEV_CACHE_UNKNOWN, BLKDEV_CACHE_DISABLED, BLKDEV_CACHE_ENABLED };
struct blkdev {
    /* Synchronous, complete request or negative FS_EIO / FS_EINVAL /
     * FS_EQUARANTINED. No retry after an issued error. count is in sectors.
     * flush != NULL certifies a qualified durability implementation; returning
     * success means all earlier writes reached stable media. */
    int (*read)(struct blkdev *dev, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct blkdev *dev, uint64_t lba, uint32_t count, const void *buf);
    int (*flush)(struct blkdev *dev);
    uint64_t capacity;                 /* number of sectors, not last LBA */
    uint32_t sector_size;              /* F1 requires 512 */
    enum blkdev_cache_state write_cache_state;
    bool quarantined;
    void *ctx;                        /* owned by the device implementation */
};
static inline int blkdev_range(struct blkdev *d, uint64_t lba, uint32_t n) {
    if (!d || d->sector_size != 512 || !n || lba >= d->capacity || n > d->capacity-lba) return -FS_EINVAL;
    return d->quarantined ? -FS_EQUARANTINED : 0;
}
static inline bool blkdev_durable(struct blkdev *d) {
    return d && d->write && !d->quarantined &&
        (d->write_cache_state == BLKDEV_CACHE_DISABLED ||
         (d->write_cache_state == BLKDEV_CACHE_ENABLED && d->flush));
}
#endif
```

### `vfs.h`

SHA-256: `0c6660e7446e81bad0fd3aea0a46d5f520a04e8406fdc9db685ac87bfb312655`

```c
/* Frozen f1-01 VFS interface, v1. All failures are negative FS_E*.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_VFS_H
#define CIUKI_VFS_H
#include "fat.h"
#define VFS_NODES 64u
#define VFS_DESCRIPTIONS 128u
#define VFS_HANDLES 64u
#define VFS_TABLES 16u
#define VFS_READ 1u
#define VFS_WRITE 2u
#define VFS_CREATE 4u
#define VFS_EXCLUSIVE 8u
#define VFS_TRUNCATE 16u
/* Bitmask of access denied to other opens, checked in BOTH directions. */
enum vfs_share { VFS_DENY_NONE=0, VFS_DENY_READ=1, VFS_DENY_WRITE=2, VFS_DENY_ALL=3 };
enum vfs_whence { VFS_SEEK_SET, VFS_SEEK_CUR, VFS_SEEK_END };
struct vfs_node { struct fat_volume *volume; struct fat_entry entry; unsigned refs; };
struct vfs_description { struct vfs_node *node; uint64_t position; unsigned refs, access, deny; };
struct vfs_table;
struct vfs {
    fs_lock lock; /* namespace -> cache -> device; no IRQ/interrupts-off I/O */
    struct fat_volume *volumes[26];
    uint32_t generation[26];
    struct vfs_node nodes[VFS_NODES];
    struct vfs_description descriptions[VFS_DESCRIPTIONS];
    struct vfs_table *tables[VFS_TABLES];
};
struct vfs_table {
    struct vfs *vfs;
    struct vfs_description *handles[VFS_HANDLES];
    unsigned drive; /* 0=A, 2=C */
    uint32_t cwd_cluster[26]; /* zero denotes the volume root */
    char cwd[26][FS_PATH_BYTES];
};
struct vfs_find {
    unsigned drive; uint32_t generation, directory, cursor;
    uint8_t attr_mask; char pattern[FS_NAME_BYTES];
};
/* Caller owns all storage, including tables, mounted volumes and cache. No
 * allocations in open/read/write/find/etc. Capacity exhaustion is explicit.
 * Initialize once; destroy after tables/volumes are detached. */
void vfs_init(struct vfs *);
void vfs_destroy(struct vfs *);
int vfs_attach(struct vfs *, unsigned drive, struct fat_volume *);
int vfs_detach(struct vfs *, unsigned drive); /* durable unmount, refuses opens */
int vfs_table_init(struct vfs *, struct vfs_table *, unsigned default_drive);
void vfs_table_destroy(struct vfs_table *);
int vfs_open(struct vfs_table *, const char *path, unsigned flags, enum vfs_share, uint8_t create_attr);
int vfs_close(struct vfs_table *, int handle);
/* dst=-1 selects first free handle. Duplicates/inherited handles share position.
 * Explicit dst replacement releases the previous reference atomically. */
int vfs_dup(struct vfs_table *src, int handle, struct vfs_table *dst, int dst_handle);
int vfs_read(struct vfs_table *, int, void *, size_t, size_t *done);
int vfs_write(struct vfs_table *, int, const void *, size_t, size_t *done);
int vfs_seek(struct vfs_table *, int, int64_t offset, enum vfs_whence, uint64_t *position);
int vfs_truncate(struct vfs_table *, int, uint64_t size);
int vfs_commit(struct vfs_table *, int);
int vfs_stat(struct vfs_table *, const char *, struct fat_entry *);
int vfs_attrib(struct vfs_table *, const char *, uint8_t attr);
int vfs_times(struct vfs_table *, const char *, const struct fat_times *);
int vfs_find_first(struct vfs_table *, const char *pattern, uint8_t mask, struct vfs_find *, struct fat_entry *);
int vfs_find_next(struct vfs_table *, struct vfs_find *, struct fat_entry *);
int vfs_rename(struct vfs_table *, const char *from, const char *to);
int vfs_delete(struct vfs_table *, const char *);
int vfs_mkdir(struct vfs_table *, const char *, uint8_t attr);
int vfs_rmdir(struct vfs_table *, const char *);
int vfs_chdir(struct vfs_table *, const char *);
int vfs_set_drive(struct vfs_table *, unsigned drive);
int vfs_getcwd(struct vfs_table *, unsigned drive, char out[FS_PATH_BYTES]);
int vfs_free_space(struct vfs_table *, unsigned drive, uint64_t *bytes, uint32_t *cluster_bytes);
/* Byte locks/DOS compatibility mode are F3 bridge work, not silently emulated. */
#endif
```

## Fixtures and retained artifacts

Tool versions: `mkfs.fat 4.2 (2021-01-31)`, `mcopy (GNU mtools) 4.0.49`.
Seed: `0x00C1A001` (12,689,409). Every fixture has 512-byte sectors and two FATs.

| Fixture | Bytes | Sectors/cluster | Data clusters |
| --- | ---: | ---: | ---: |
| FAT12 | 12,582,912 | 8 | 3,063 |
| FAT16 | 16,777,216 | 2 | 16,303 |
| FAT32 | 41,943,040 | 1 | 80,628 |

`build/host/fs/fixtures.json` contains fixture SHA-256, payload SHA-256,
complete geometry, initial checker output and mtools listings. `work.img` is
reused sequentially and removed. Crash replay restores individual sectors
from an in-memory undo log; it does not accumulate image copies.

After removing four obsolete intermediate host object files, retained artifacts
are **23 files, 76,933,467 logical bytes, 8,126,464 allocated bytes**,
below 200,000,000 bytes by either measure. The exact run output below includes
those four intermediate objects in its earlier artifact count.

## Contract problems / remaining qualification

1. FAT has no general sector integrity checksum. The tested torn metadata
   cases become read-only through copy/structural checks; universal torn-sector
   detection, including valid-looking mixtures or file data, is impossible with
   the specified on-disk format.
2. Crash checker evidence also includes orphan LFNs (198 diagnoses), stale
   FSInfo hints (135), and unowned partial FAT12 entries (10). They are classified
   explicitly, with no owned-chain corruption. These outcomes follow from LFN
   publication, hints-last ordering and packed entries, but go beyond the literal
   list of permitted interrupted states. The test classifier is not a waiver.
3. Same-directory rename keeps the owning short entry in place. A longer LFN
   lacking immediately preceding free slots returns `ENOSPC`. Arbitrary relocation
   needs an agreed recovery or unlink-first rule.
4. Growth of an imported FAT12 directory with a tail entry spanning sectors
   returns `EOPNOTSUPP`. New directories avoid such tails; file updates use
   replacement chains. General live directory relocation needs a design decision.
5. FAT32 with mirroring disabled is readable from its selected FAT but remains
   read-only, pending clarification of the write-every-copy contract.

Additional integration limits are explicit in VALIDATION.md: whole-file COW
requires spare space, fixed handle/node capacities, configurable cache/scan
workspace bounds, task-context locking, and an optional calendar-clock hook
(default date 1980-01-01 because the existing kernel exposes only PIT time).
The five-second writer hook still needs scheduling by the lead. QEMU, hardware,
full kernel linking and release qualification were outside this directive.

## Exact command and output

Command (exit **0**):

```sh
scripts/test/host_fs_tests.sh > build/host/fs/results.log 2>&1
```

Combined stdout/stderr, verbatim:

```text
SANITIZERS ASan=on UBSan=on LSan=off (sandbox ptrace restriction)
PASS host compile: clang C17 ASan/UBSan -Wall -Wextra -Werror
PASS kernel compile: 6 translation units; exact scripts/build_kernel.py CFLAGS
PASS kernel objects: no compiler runtime helpers
fixture FAT12: bytes=12582912 sectors=24576 spc=8 clusters=3063 seed=12689409
fixture FAT16: bytes=16777216 sectors=32768 spc=2 clusters=16303 seed=12689409
fixture FAT32: bytes=41943040 sectors=81920 spc=1 clusters=80628 seed=12689409
tools: mkfs.fat 4.2 (2021-01-31); mcopy (GNU mtools) 4.0.49
PASS cache: LRU/5-second/barriers/reorder/sticky-errors/multiple-devices
RESULT groups=1 checks=95 failures=0 ASan/UBSan=enabled
PASS partitions: primary/EBR-relative/loop/overflow/overlap/GPT/signature/EBR-overlap
RESULT groups=1 checks=19 failures=0 ASan/UBSan=enabled
PASS FAT12 read/names/aliases/UTF-8/cwd/masks zero_writes
RESULT groups=1 checks=12378 failures=0 ASan/UBSan=enabled
PASS FAT12 faults: BPB/dirty/error/divergence/loop/torn/LFN/read/write/flush/ENOSPC/durability
RESULT groups=1 checks=161 failures=0 ASan/UBSan=enabled
PASS FAT12 crash: cuts=959 modes=4 copies_diverged=92 lost=772 dirty=0 crosslinks=0 corrupt_owners=0
RESULT groups=1 checks=10497 failures=0 ASan/UBSan=enabled
PASS FAT12 write/4MiB/overwrite/truncate/dir-growth/collisions/rename/delete/shared-handles
RESULT groups=1 checks=8551 failures=0 ASan/UBSan=enabled
fsck.fat 4.2 (2021-01-31)
build/host/fs/work.img: 63 files, 374/3063 clusters
PASS mtools equality: work.img entries=63 names/sizes/FNV1a32 match
PASS FAT16 read/names/aliases/UTF-8/cwd/masks zero_writes
RESULT groups=1 checks=12373 failures=0 ASan/UBSan=enabled
PASS FAT16 faults: BPB/dirty/error/divergence/loop/torn/LFN/read/write/flush/ENOSPC/durability
RESULT groups=1 checks=181 failures=0 ASan/UBSan=enabled
PASS FAT16 crash: cuts=561 modes=4 copies_diverged=70 lost=345 dirty=543 crosslinks=0 corrupt_owners=0
RESULT groups=1 checks=6239 failures=0 ASan/UBSan=enabled
PASS FAT16 write/4MiB/overwrite/truncate/dir-growth/collisions/rename/delete/shared-handles
RESULT groups=1 checks=8507 failures=0 ASan/UBSan=enabled
fsck.fat 4.2 (2021-01-31)
build/host/fs/work.img: 30 files, 59/16303 clusters
PASS mtools equality: work.img entries=30 names/sizes/FNV1a32 match
PASS FAT32 read/names/aliases/UTF-8/cwd/masks zero_writes
RESULT groups=1 checks=12373 failures=0 ASan/UBSan=enabled
PASS FAT32 faults: BPB/dirty/error/divergence/loop/torn/LFN/read/write/flush/ENOSPC/durability
RESULT groups=1 checks=189 failures=0 ASan/UBSan=enabled
PASS FAT32 crash: cuts=586 modes=4 copies_diverged=70 lost=282 dirty=568 crosslinks=0 corrupt_owners=0
RESULT groups=1 checks=6514 failures=0 ASan/UBSan=enabled
PASS FAT32 write/4MiB/overwrite/truncate/dir-growth/collisions/rename/delete/shared-handles
RESULT groups=1 checks=8596 failures=0 ASan/UBSan=enabled
fsck.fat 4.2 (2021-01-31)
build/host/fs/work.img: 25 files, 111/80628 clusters
PASS mtools equality: work.img entries=25 names/sizes/FNV1a32 match
PASS crash fsck classification: cuts=2106 unknown_diagnostics=0 dirty=1111 divergent_fats=232 lost_clusters=1399 orphan_lfn=198 stale_fsinfo_hint=135 unowned_fat12_fragment=10
ARTIFACTS files=27 logical_bytes=77599514 allocated_bytes=8798208 limit=200000000
PASS host filesystem suite: 14 groups, 3 fsck checks, 3 independent mtools comparisons
```
