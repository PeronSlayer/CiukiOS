# f1-96 — USB mass storage reads one sector per command (85 KB/s copy)

- **Phase/model:** F1, Claude agent (Opus). **Worktree:** `wt/f1-96` from `main`.
- Evidence: T23 trial of image b453bc92… (2026-10-22 ~08:30, serial copy `build/lead-scratch/t23-b453bc92-serial.txt`, disk log `legacy/local/physical/b453bc92/BOOT.LOG` copied to `build/lead-scratch/t23-b453bc92-BOOT.LOG`).
- Records: `[user 41] [files] job=copy done ms=62239 bytes=5458058 rate_kbs=85`; `[usb-storage] bind addr=2 … reads=9 sectors=9 max_sectors=1`, `cadence packets=121 poll_frames=740 batch=32`, `[storage] queue name=usb-storage … split=50 … limit=4096`. One 512-byte sector per Bulk-Only command with the 1 ms UHCI frame cadence gives about 6 ms per sector, i.e. the measured 85 KB/s; a USB 1.1 stick should sustain about 900 KB/s with 64 KiB transfers.

## Required

1. Research: USB Mass Storage Bulk-Only Transport 1.0 (CBW/CSW, data phase of up to the device's max transfer), UHCI 1.1 (TD chaining, up to 64 bytes per full-speed bulk packet, bandwidth per frame), the Linux usb-storage `max_sectors` policy (240 sectors default) and SCSI READ(10) with large transfer lengths; cite.
2. Implement multi-sector READ(10)/WRITE(10) with a transfer length up to 64 KiB (or the device's limit), TD chains spanning frames, and the storage queue `limit` for usb-storage raised accordingly; keep `max_sectors=1` as a fallback after a STALL/CSW error with a record.
3. Measure on the host USB model: a 5 MB sequential copy at the UHCI full-speed budget must complete in under 8 s (model), and the `[usb-storage] bind … max_sectors=` record shows the negotiated size.
4. Host tests: CBW with transfer length > 1 sector, CSW residue handling, TD chain across frames, fallback on STALL; the storage queue test for the new limit.

Rules: no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Report `build/codex/f1-96-report.md`.
