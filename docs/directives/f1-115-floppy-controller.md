# f1-115 — Floppy disk controller (82077AA-compatible): native read/write, removable-media ownership, virtual FDC and INT 13h for DOS VMs

- **Phase/model:** F1/F4, Codex gpt-6.1-sol, reasoning xhigh, `--search`. **Worktree:** `wt/f1-115` from `main`.
- **Owner request (2026-10-22 ~23:20):** universal drivers for the peripherals of any retro PC, not only the T23. **Owner (2026-10-22 23:55):** every licence is allowed; keep notices and origin lines, and add every imported file to the credits.
- **Contract:** `docs/design/peripheral-driver-coverage.md` and `docs/design/peripheral-driver-source-audit.md`, with the lead decisions and the owner's licence decision at the end, binding. Read this directive's row, its class section and the device-model section completely. For DOS VM services, also read `docs/design/unified-dos-contract.md`: the native VMM owns the hardware, and VMs get leased virtual devices in the Windows 95/98 V86 model, not NT NTVDM.

## Required

1. Detect the FDC from the BIOS data area, CMOS drive types and ACPI/PnP PNP0700. Drive the 82077AA-compatible controller with ISA DMA channel 2: motor control, recalibrate and seek, read and write, format a track, data rates for 1.44 MB, 720 KB, 1.2 MB and 2.88 MB, and media change via DIR bit 7.
2. Errors (CRC, missing address mark, write protection) recover with bounded retries.
3. The drive is a block device mounted through the f1-115b FAT12 support as A:/B:.
4. **DOS VMs:** INT 13h floppy services and a virtual FDC lease bound to the physical drive or to a disk image file, with one owner at a time.
5. Host tests use the FDC register and DMA model, media change, errors, the FAT12 round trip through the block device, and the INT 13h fixture.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited (specifications, datasheets, donor files with path and tag). Host tests one at a time with `nice -n 19`. Write `build/codex/f1-115-report.md`.
