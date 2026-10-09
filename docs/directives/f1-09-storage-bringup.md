# Directive f1-09: storage bring-up, FAT probes, boot log

- **Step:** F1. **Contracts:** `vfs-storage-contract.md` (mount, read gate
  before any write, durability, boot log only after storage qualification),
  `f1-acceptance.md` (probes `fat-read`, `fat-write`, `cache`,
  `mount-crash`, `bootlog`; FAT12/16 subcases on fixture disks supplied by
  the runner on IDE slots 1+), `boot-memory.md` (image layout: FAT32 at LBA
  2048, `\SYSTEM\` directory).
- **Implementer:** Codex, `gpt-6-astra`, effort `xhigh` (write paths and
  crash cases on real block devices).
- **Prerequisites on `main`:** f1-01 (fs), f1-03 (probe registration),
  f1-06 (ATA, `blkpart`, SHA-256), f1-08 (`drivers_init`).
- **Worktree:** `wt/f1-storage`. Files: new `src/kernel/fs/mount.c`,
  `src/kernel/include/ciuki/storage.h`, `src/kernel/fs/fs_port.c` (kernel
  calendar clock from the RTC read-only path if available, else 1980),
  `src/kernel/fs/*` only for integration fixes with a note each, new
  `src/kernel/probes/fat_probes.c`, `src/kernel/core/bootlog.c`,
  `src/kernel/include/ciuki/bootlog.h`, `src/kernel/core/init.c` (call
  `storage_init()` after `ata_init`), `tests/host/*`,
  `scripts/test/host_kernel_tests.sh`, `scripts/test/host_fs_tests.sh`.

## What to build

1. **Storage bring-up** (`mount.c`, `storage_init()`): for each qualified
   ATA device, walk the MBR with `partition.c`, create `blkpart` views, and
   mount every FAT volume read-only first (mount checks of the VFS
   contract); the boot volume (partition 1 of disk 0) becomes `C:`; other
   volumes get the next letters. A volume becomes writable only after the
   **read gate**: the mount checks pass, the FSInfo/clean flags are
   consistent, and the device is durable (`blkdev_durable`: verified
   write-cache state or working `FLUSH CACHE`); otherwise it stays read-only
   with the reason recorded. The 5-second write-back writer runs on the
   device worker (`kwork`) with the `cache_writeback_tick` hook.
2. **Probes** (`fat_probes.c`; register with `CIUKI_F1_PROBE`; evidence
   fields exactly as the contract table; file digests are SHA-256 and the
   runner compares them against its own computation):
   - `fat-read`: list `\SYSTEM\` and the fixture directories on the boot
     volume and on the FAT12/FAT16 fixture disks the runner attaches on IDE
     slots 1 and 2 (when present; absent fixture disks are reported, not
     failed), report names, aliases, sizes and digests, the mount mode,
     orphan/bad-checksum LFN handling, boundary chain and invalid-name
     behaviour, `writes=0` before the read gate.
   - `fat-write`: on the boot volume (overlay on QEMU): create, read back,
     overwrite, truncate, rename, delete files of 0 bytes, one cluster and
     4 MiB; grow a directory past one cluster; collide LFN aliases; reopen
     and report digests; then a cold-reboot subcase reads the same files
     back (the runner declares the reboot sequence on one overlay) and the
     runner runs `fsck.fat -n` and `mtools` on the exported overlay.
   - `cache`: barrier identifiers and persisted-write trace, the 5-second
     write-back, eviction, shared-handle coherence; the unsupported-flush
     case (a fixture device with `flush=NULL`) keeps the volume read-only;
     the delayed/flush error case (driver boundary fault) reaches the caller
     and prevents a clean unmount.
   - `mount-crash`: bad BPB refusal, dirty/error flags, mirrored-copy
     divergence and detected corruption force read-only and reject writes
     (on fixture images the runner prepares); the crash cut subcase stops
     (via the runner's declared cut after an `ARM` record) and the next boot
     verifies no cross-links and the declared interrupted states only.
   - `bootlog` (`bootlog.c`): before qualification `writes=0`; after the
     boot volume is writable, `\SYSTEM\BOOT.LOG` is opened, bounded at
     64 KiB (ring by truncation), receives the activation records and
     `klog` lines; a durable shutdown (`sync` + flush) then a reboot reopens
     it with matching records; read-only volumes report `unavailable`; a
     panic adds zero storage calls (the panic path never touches the VFS).
3. **Calendar clock**: `fs_set_calendar_clock` from a read-only RTC
   provider (`device-firmware-ownership.md` RTC rules: serialized index/data
   access, UIP check, BCD/binary, 12/24 h, timeout) in `fs_port.c` or a new
   `rtc.c` if cleaner; falls back to 1980-01-01 when the RTC read fails.

## Host tests (mandatory)

Extend the fs host suite with mount sequencing on fake devices (read gate,
durability decision, write-back timer hook), the boot log ring and its
reopen path, and the RTC decoder on scripted register values. The kernel
probes keep their decision logic in host-testable functions.

## Acceptance by the lead

Diff review; host tests; kernel build with audit; on QEMU `f1-storage`
and `f1-fat32` pass on `qemu-t23` and `qemu-e500` (FAT12/16 fixtures
attached), including the cold-reboot and crash-cut cases with `fsck.fat`
and `mtools` agreement; F0 and earlier F1 suites unchanged. Reply with:
files, test output, and any contract problem found.
