# f1-115b — Native FAT12 mounting (floppies and small media in the file manager)

- **Phase/model:** F1, Codex gpt-6.1-sol, reasoning xhigh. **Worktree:** `wt/f1-115b` from `wt/dirs`.
- **Lead decision 5 of `docs/design/peripheral-driver-coverage.md`:** native FAT12 mounting is approved, so that floppies (once f1-115 binds the controller) and FAT12 USB or optical images open in the file manager.
- **Contracts:** `docs/design/vfs-storage-contract.md` and the FAT sections of the storage design. The Windows 98/ME model of `docs/design/win98-class-resource-management.md` applies: cache, holds and write-behind.

## Required

1. **FAT12 in the existing FAT driver** (`src/kernel/fs/fat.c`): 12-bit FAT entry packing across sector boundaries, root directory region, BPB validation for the standard formats (160/180/320/360 KB, 720 KB, 1.2 MB, 1.44 MB, 2.88 MB, and DMF 1.68 MB read-only), and media-descriptor checks. Read and write, with the same cache, flush and quarantine rules as FAT16/32.
2. **Removable media semantics.** Media change invalidates the cache and generation; an open handle on a removed medium fails with a clear error; write protection is reported.
3. **Mounting.** Any block device with a FAT12 volume (USB mass storage, image files, later the f1-115 floppy) mounts like FAT16, as A:/B: for floppies once f1-115 exists.
4. **Host tests.** Images made with `mkfs.fat -F 12` for every standard geometry, verified with `fsck.fat -n` and mtools. Cover read/write/rename/delete and directory growth in the fixed root, a full disk, FAT chain corruption fixtures, and media-change invalidation.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited (Microsoft FAT specification "FAT: General Overview of On-Disk Format" 1.03). Host tests one at a time with `nice -n 19`. Write `build/codex/f1-115b-report.md`.
