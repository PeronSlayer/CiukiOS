# f1-47 — USB mass storage on the T23: the first bulk data transfer faults (Lexar 05dc:a81d)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f1-47` from `main`. Host-testable; the lead qualifies on the T23.

## Evidence (T23, ICH3 UHCI, image e86305de, 2026-10-13; transcript `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/e86305de-usb-storage-fault-optical-missing/serial-clean.log`)

```
[usb] device addr=2 vid:pid=05dc:a81d class=00 speed=full
[usb-storage] interface=0 addr=2 max_lun=0 selected=0
[usb-storage] reset addr=2 result=0
[usb-storage] fault addr=2 reason=data_transfer tag=1
[usb-storage] attach addr=2 result=-5
```

The stick (Lexar JumpDrive, 16 GB, FAT32 with MBR) enumerates (device class 00, interface 08/06/50), Get Max LUN answers 0, the bulk-only reset succeeds, then the **first command's data phase** (tag 1, INQUIRY) fails and the device is not attached. The same stack works against the QEMU `usb-storage` model. The Logitech receiver on the same controller type works (interrupt transfers).

## Task

1. Root cause of the data-phase fault on real UHCI/full-speed bulk: TD chaining and max packet size (64) for the IN data stage, data toggles after the class reset, short packets (INQUIRY returns 36 bytes, less than the requested length), the STALL/NAK handling, retirement timing on a real 1 ms frame, and the CBW/CSW alignment rules of the BOT 1.0 spec and USB 1.1 chapter 5/8; compare with Linux `usb-storage` transport.c (GPL, cite) and with real-device quirks (many sticks need the "no Get Max LUN"/delay after reset, or return a short INQUIRY). Cite the specs.
2. Diagnostics: on every fault record the stage (CBW/data/CSW), expected and actual lengths, TD status bits, data toggle, elapsed frames, and the CSW status/residue when available, so the next capture is conclusive.
3. Fix with a host fixture reproducing the hardware behaviour (short INQUIRY, a 64-byte-packet device, a stall on the first data stage) that fails before and passes after; keep the fail-closed rules (no write resend).

## Acceptance

`tests/host/usb/storage_run.sh` and kernel host suites green; i686 kernel compiles. The lead re-tests the Lexar stick on the T23 and `usb-storage` on QEMU.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f1-47-report.md`.
