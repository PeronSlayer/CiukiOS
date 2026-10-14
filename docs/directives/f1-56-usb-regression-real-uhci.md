# f1-56 — USB on the T23 after f1-52: control transfers time out and HID reports never arrive (mouse and stick dead)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f1-56` from `main`. Host-testable; the lead verifies on the T23.

## Evidence (T23 ICH3, three UHCI controllers, Logitech receiver 046d:c52b and Lexar stick, image 8c88b1ab, 2026-10-14; garbled capture `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/8c88b1ab-garbled-usb-regression/serial-clean.log`)

Before f1-52 (image 293c05af/e86305de) the receiver's keyboard and mouse worked on the T23. With f1-52 (independent nonblocking interrupt QHs/EDs per HID pipe, 1 ms service cadence, host mutex released before sleeps) merged over f1-53: `[usb] device addr=1 vid:pid=046d:…` enumerates, HID polls log `result=-11` (EAGAIN) continuously (`[usb] hid pipe addr=1 valid=1 td=188807ff … actual=-1`), the mouse never moves; a later device at address 0 gets `[usb] addr0 … result=-110 elapsed_ms=558` (GET_DESCRIPTOR timeout) and `[usb] quarantine device …`, so the stick never attaches. On QEMU the same code enumerates and delivers reports (f1-usb case up to the runner pacing step).

## Task

1. Verify the UHCI schedule built by f1-52 against the UHCI 1.1 design guide §3.4 and the QEMU/Linux behaviour differences: frame list entries → interrupt QHs (per pipe) → control QH → bulk QH, every QH's element/link pointers with the correct T bits and the breadth-first traversal; the "active" bit and IOC handling on the independent interrupt TDs; the depth/breadth bit of the interrupt TD link; whether the control QH is still reachable from the frame list after the interrupt QHs were made independent (a control transfer that never retires explains `-110` and a HID TD never visited explains the permanent EAGAIN). QEMU's UHCI is lenient about some link forms (it follows both pointers); real ICH3 silicon is not.
2. Fix with a host model that enforces the hardware traversal rules strictly (a QH not linked from the frame list is never executed; a TD with the wrong depth bit is not followed), reproducing the T23 symptoms before the fix. Keep the f1-52 cadence and the f1-53 port rules.
3. Add to the verbose boot a one-time dump of the schedule after enumeration (`[usb] schedule frame=… qh=… link=… element=…` for the first frames) so the next capture is conclusive.

## Acceptance

USB, storage and kernel host suites green; i686 kernel compiles; the lead verifies mouse, keyboard and the stick on the T23 and `f1-usb` on QEMU.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f1-56-report.md`.
