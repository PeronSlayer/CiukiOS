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
| Block device | F1 (ATA PIO), F4 (ATA DMA, ATAPI) | Sector I/O, geometry, error reporting, media change; one owner per controller (`device-firmware-ownership.md`) |
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

**[F2, f2-11 amendment]** Durable shutdown MUST preflight every attached
volume under the shared VFS namespace lock for native volume-backed open
descriptions and legacy VFS handles before stopping storage, committing or
detaching any volume. A refusal MUST return `-EBUSY`, leave storage running
and every volume attached with its mount generation unchanged, and permit a
later `storage_sync()` after the final close to succeed, provided no I/O error
intervenes. Duplicated/inherited fds retain their shared description until the
last close, following the [POSIX open-description lifetime](https://pubs.opengroup.org/onlinepubs/009604499/functions/close.html).
Cwd pins and structural/synthetic nodes MUST NOT count as open descriptions
on a volume. The namespace lock MUST remain held from preflight through
commit and durable detach, preventing concurrent opens from invalidating the
preflight. Successful detach MUST invalidate native mount identities and
reset legacy per-drive cwd to `/`; native cwd expiration follows
`posix-subset.md`. Optional native hooks MUST remain weak for standalone F1
storage. `storage_sync()` first drains/closes the boot-log sink; an EBUSY
refusal keeps storage usable but does not reactivate that sink. Existing
sticky write/flush errors remain errors and MUST NOT be converted into a
successful clean shutdown.

- Default size 8 MiB (`boot-memory.md` budget), adjustable at boot.
- Dirty blocks are written by a `device`-priority writer thread no later than
  5 seconds after they become dirty, on explicit `flush`, before unmount, and
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

## Mount, errors and corruption

- Mount checks: BPB values and signature, FAT count, cluster count matching
  the declared type, root cluster inside the volume, FSInfo signatures, and
  the media byte in `FAT[0]` and the reserved-entry and flag encoding of
  `FAT[1]`. A failure refuses the mount and logs why.
- A writable mount clears `ClnShutBitMask` (volume in use) and sets it again
  only after a successful durable unmount. A volume found with
  `ClnShutBitMask = 0` (not cleanly unmounted) or `HrdErrBitMask = 0`
  (I/O errors recorded) is mounted read-only until a check passes (a kernel
  checker arrives in F1 as a read-only scan; repair stays a host-side
  `fsck.fat` step in F1). The FAT16 and FAT32 masks differ; FAT12 has
  neither flag. The media byte in `FAT[0]` must match `BPB_Media`.
- Chain walks are bounded by the volume's cluster count, so a loop is
  detected instead of hanging.
- A read error returns `EIO` to the caller. A failed issued ATA command ends
  that request with `EIO` and puts the controller ownership in quarantine
  (`device-firmware-ownership.md`); there is no automatic retry, through the
  BIOS or otherwise, until a separately qualified recovery path exists. A
  metadata write failure turns the volume read-only and reports it.
- Removable media (F4): a media change invalidates cached blocks and nodes
  for that device; open handles then fail with `EIO`.

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
- F1 read-only paths: a dirty-bit image and a corrupted-BPB image mount
  read-only and refuse, respectively, with logged reasons.

## Open questions

- Whether the 5-second flush window is acceptable for the owner's use
  (Windows 9x used delayed writes too); measured against power-loss tests.
- Whether drive-letter assignment should be configurable in F1 or fixed.
- Whether the T23 and E500 disks support ATA DMA modes reliably with their
  chipsets (F4 qualification).

## Interfaces required from other contracts

- `boot-memory.md`: FAT32 layout and the cache budget.
- `execution-abi.md`: handle and syscall conventions, `device` priority for
  the writer thread.
- `device-firmware-ownership.md`: ATA/ATAPI controller ownership, IRQ and
  DMA rules, BIOS policy for disks.
- `dos-dpmi-contract.md`: bridge semantics for PSP/JFT/SFT, FCBs and DOS
  compatibility share mode.
