# f1-53 — USB stick on the T23: device quarantined on transfer, then the port floods the logs

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f1-53` from `main`. Host-testable; the lead re-tests on the T23.

## Evidence (T23 ICH3 UHCI, Lexar 05dc:a81d, image 293c05af with f1-47, 2026-10-14; `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/293c05af-garbled/serial-clean.log` (garbled) and `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/293c05af-garbled/disk-log/BOOT.LOG`)

- Serial fragment: `[usb] quarantine dev… (tra)nsfer` — the stick is still quarantined on a transfer after f1-47 (the 10 s bulk budget and error classification).
- Disk log tail: `[usb] port=2 status=00000093 event=connect controller=usb-2481` repeated every 230–250 ms for the rest of the session (PORTSC 0x93: CCS and CSC set with the device quarantined), interleaved with `[usb] worker … scans_per_second=3.9`: the port scan re-logs the connect change every scan and never clears or rate-limits it; the 128 KiB disk log ring was overwritten by this flood and lost the boot records.

## Task

1. Port scan: a quarantined device's port must have its change bits acknowledged once and be reported once (`event=connect reason=quarantined`), then silent until a real disconnect; no record more often than once per second per port in any state. Rate-limit every per-scan record.
2. The transfer fault: with the f1-47 diagnostics unavailable (garbled capture), build the host model from what is known about this stick family (Lexar JumpDrive, Phison/SMI controllers: slow first INQUIRY, 64-byte packets, STALL on unsupported MODE SENSE, short READ CAPACITY responses) and the UHCI bulk behaviour on real ICH3 (babble/NAK/CRC classification, toggles after STALL clear), and make the attach sequence tolerant: skip MODE SENSE write-protect probing when stalled (assume writable only after a successful TEST UNIT READY + READ CAPACITY), retry INQUIRY once after a class reset, accept a 36-byte short INQUIRY; keep "never resend a write".
3. Diagnostics: the first fault of a device logs the complete transport state in one record (stage, CBW tag, expected/actual lengths, TD status, toggle, CSW status/residue, port status) and the quarantine record names the stage; verified in the host test.

## Acceptance

`tests/host/usb/storage_run.sh`, `tests/host/usb/run.sh` and kernel host suites green; i686 kernel compiles; the lead re-tests the Lexar stick on the T23 and `usb-storage` on QEMU.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f1-53-report.md`.
