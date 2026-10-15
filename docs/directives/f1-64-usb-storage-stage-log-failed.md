# f1-64 — USB mass storage: the stick is quarantined at `max_lun` by a logging failure (T23, image a738204a)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f1-64` from `main`.

## Evidence (disk log lines 295–393)

- `[usb] port=2 status=0000009b event=connect reason=state controller=usb-2807 tick=4106`, reset, release and enable at ticks 4206–4257, then address 2 is assigned (the Logitech receiver is address 1 on port 1 of the same controller, keyboard and mouse working).
- `[usb-storage] stage=max_lun addr=2`
- `[usb-storage] fault addr=2 reason=stage_log_failed tag=0 cmd=00 stage=none expected=0 actual=0 result=-11 td=00000000 toggle=0 csw=0 status=0 residue=0 port=00000000`
- `[usb-storage] td addr=2 tag=0 valid=0 … td=00000000`, `timing addr=2 tag=0 frames=0 elapsed_ms=0 retired=0`, `csw … available=0`
- `[usb-storage] quarantine addr=2 reason=stage_log_failed stage=max_lun transfer=none`, `[usb-storage] stage=max_lun addr=2 elapsed_ms=2000 result=-5`

No transfer descriptor was ever built (`td=00000000`): the first control request failed inside the driver's own stage-record bookkeeping with -EAGAIN, and the device was quarantined for it. This is the third T23 image on which the stick never mounts.

## Task

1. Find why the stage log returns -EAGAIN at the first stage of address 2 (candidates: a stage-record ring sized or reset per controller and already consumed by the HID device's polls on the same controller; a budget from f1-62 charged before the stage starts; a worker-ownership check against the wrong controller). Fix it.
2. A logging or bookkeeping failure must never quarantine a device: the transfer proceeds and the record is marked lost.
3. Host test that replays this exact sequence (HID device at address 1 polling on the controller, then a mass-storage device at address 2) through the existing USB host harness (`tests/host`, grep `usb_storage`), failing before the fix.
4. Keep the f1-62 bind budget records; make the stage records name the ring occupancy when they fail.

## Acceptance

USB host tests green with the new case; QEMU `f1-usb` (`usb-input-uhci`, `usb-input-ohci`) and the `f4-media` CD+USB cases green; the lead checks the next T23 log for `[usb-storage] … stage=inquiry` and a mount. Files: `src/kernel/usb/usb_storage.c`, `src/kernel/usb/uhci.c`, the USB worker.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary. Host tests one at a time with `nice -n 19`, `rg` not semble. Evidence files are read-only: `legacy/local/physical/a738204a/disk-BOOT.LOG` (T23 disk log of image a738204a, main `56d41c09`) and `serial.log` beside it. Plain technical prose in every document you touch, no product names of other operating systems, no filler. Report `build/codex/f1-64-report.md` with: root cause (file:line), files changed, test output verbatim, what the lead must verify on QEMU and on the T23.
