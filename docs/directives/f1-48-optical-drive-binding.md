# f1-48 — The CD-ROM is identified by ATA but never bound to the optical volume

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f1-48` from `main`. Host-testable (synthetic ATAPI device); the lead tests a data CD on the T23 and an ISO on QEMU.

## Evidence (T23 with a data CD inserted, image e86305de, 2026-10-13; transcript `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/e86305de-usb-storage-fault-optical-missing/serial-clean.log`)

```
[ata1] unit=0 kind=atapi type=5 packet_bytes=12
[ata1] identified=1 quarantined=0
[storage] optical slot=2 drive=- mode=ro generation=2 result=-5 reason=unavailable
```

The secondary channel identifies the ATAPI drive, yet the optical mount reports no drive (`drive=-`) and `unavailable` on every boot, with or without a disc. On QEMU with `ide-cd` the F4 optical path (ISO 9660 read-only, drive letter) was accepted. Files shows no CD drive.

## Task

1. Trace the binding from `ata_probe.c`/`ata.c` (ATAPI device records) through `blkpart`/`mount.c` to the optical slot: which condition leaves `drive=-` on the T23 (unit numbering, the 12-byte packet size, TEST UNIT READY/REQUEST SENSE before media detection, the "slot=2" assignment, the admission flags of f1-44 for ATAPI staying on PIO). Add records for every step (`[optical] bind …`, `[optical] media …`, sense keys) so the next capture is conclusive.
2. Media detection at runtime: a disc inserted after boot must be noticed within two seconds (TEST UNIT READY polling or GET EVENT STATUS NOTIFICATION, cite SFF-8020i/MMC), mounted read-only as ISO 9660 with the next drive letter, shown in Files and removed on eject; the `/dev/media` state of f1-43 covers optical media too (type field).
3. Host tests: synthetic ATAPI device with no media, media inserted later, media removed, a CD with Joliet names; the QEMU `ide-cd` fixture unchanged.

## Acceptance

Kernel and filesystem host suites green; i686 kernel compiles. The lead: data CD on the T23, ISO on QEMU.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f1-48-report.md`.
