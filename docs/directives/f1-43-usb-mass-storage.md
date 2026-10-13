# f1-43 — USB mass storage: sticks and external drives as removable volumes

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f1-43` from `main`, **after f1-42 is merged** (the lead rebases this worktree). Host-testable; QEMU `usb-storage`.

## Task

1. Contract section in `docs/design/usb-contract.md`: bulk transfers, Bulk-Only Transport (USB mass storage class 1.0), SCSI transparent command set subset (INQUIRY, READ CAPACITY, READ(10), WRITE(10), TEST UNIT READY, REQUEST SENSE), hot-plug and safe removal (writes flushed, a "Safely remove" action in the tray and in Files), media change detection, 512-byte sectors only in this step.
2. Implementation: `usb_storage.c` as a `blkdev` beside ATA, partitions through `blkpart`, FAT12/16/32 read-write through the existing mount path with the removable flag (no dirty-recovery writes without the owner's confirmation rules of F1), drive letters in the Computer view of Files (`E:` onward), the tray icon for removable media, verbose records `[usb-storage] …`.
3. Host tests: synthetic bulk-only device with a FAT image, error paths (STALL, phase error, removal during I/O), the mount/unmount sequence, Files/Computer view.

## Acceptance

Host suites green; the lead copies a file to and from a QEMU `usb-storage` stick and then on hardware.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f1-43-report.md`.
