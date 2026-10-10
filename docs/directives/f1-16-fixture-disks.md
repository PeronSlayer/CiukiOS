# Directive f1-16: FAT12/FAT16 fixture disks — superfloppy mount and slot numbering

- **Step:** F1. **Contracts:** `f1-acceptance.md` (`fat-read` FAT12/16
  subcases on fixture disks attached by the runner), `vfs-storage-contract.md`
  (mount checks), the f1-09 report.
- **Implementer:** Codex, `gpt-6.1-sol`, effort `high`.
- **Worktree:** `wt/f1-fixture-disks`. Files: `src/kernel/fs/mount.c`,
  `src/kernel/fs/partition.c` only if a helper is needed,
  `src/kernel/probes/fat_probes.c`, `scripts/test/run.py` (fixture disk
  attachment), `tests/suites/f1-fat32.json`, `tests/host/fs/test_storage.c`,
  `tests/host/test_runner.py`.

## Observed on image `b0cb346a…` (`f1-fat32`, case `fat12-read`)

The runner attached `fixture-0.img` as the second IDE disk; the storage
ledger shows `[storage] kind=1 disk=1 part=0 … reason=disk_scan` and no
mount for disk 1 (the fixture is a plain `mkfs.fat` volume without an MBR,
a "superfloppy"); the probe then reports `case=fixture slot=2 status=absent`
and no `group=fat-read fat_type=12` record, so the case fails and every
later `f1-fat32` case is `not_run`.

## What to do

1. `storage_init`: a disk whose sector 0 holds no valid MBR but a valid FAT
   BPB (jump, bytes per sector 512, FAT type determinable, sector counts
   consistent with the capacity) is mounted as one volume covering the
   whole disk (superfloppy), with the mount record saying `part=0
   layout=superfloppy`; the boot disk keeps the MBR rule; the mount checks
   and read-only gate apply unchanged.
2. One numbering for fixture disks: the runner attaches fixtures on IDE
   `index=1` upward and reports the index in `result.json`; the probe
   enumerates `disk=1, 2…` and records `case=fixture disk=<n> status=…`;
   suites and predicates use the same numbers.
3. Host tests: superfloppy detection (FAT12 and FAT16 fixtures from the fs
   test generator, plus a disk with neither MBR nor BPB), the probe's
   enumeration with two fixtures present and with none, and the runner's
   attachment arguments.

## Acceptance by the lead

Host tests; kernel build; on QEMU `f1-fat32` runs `fat12-read` and
`fat16-read` on the fixture disks and `fat32-read` on the boot volume.
Reply with: files, test output, and any contract problem found.
