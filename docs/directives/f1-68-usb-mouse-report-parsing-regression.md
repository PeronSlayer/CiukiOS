# f1-68 — USB mouse after f1-66: lag, no right button, no wheel in the desktop (T23, image 5e018550)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f1-68` from `main`. Build the kernel (`python3 scripts/build_kernel.py`) before reporting.

## Evidence

Owner: the USB mouse (Logitech receiver 046d:c52b) lags, the right button and the wheel do nothing. Serial capture `legacy/local/physical/5e018550/serial.log` (interleaved; usable fragments): `hid mouse … wheel=1` (report protocol selected by f1-66), `hid descriptor ad…0509 5010 1938 8106 050c 0a38 02 95…` (Usage Page Button with a report count of 16, Consumer AC Pan 0x0238: the receiver's mouse report is report ID + 16 button bits + 12-bit X/Y + 8-bit wheel + 8-bit horizontal pan), `usb wheel addr=1 interface=1 value=-1` (the kernel decodes wheel reports) with no `[console] [desktop] usb wheel` record and no scrolling. The QEMU case `usb-wheel-uhci` also fails: `missing expected serial pattern [console] [desktop] usb wheel value=-1` with QEMU's mouse (0627:0001, descriptor bytes in build/test-runs/zz-usb/*/serial.log).

## Task

1. Parse the receiver's descriptor exactly: report ID prefix, multi-byte button fields (16 bits), 12-bit signed X/Y, wheel and AC pan; fixture from the dump bytes (and from QEMU's descriptor) with a decoded-report test for left, right, middle, motion, wheel; host harness proves the decoded buttons and deltas.
2. Deliver the decoded events to the desktop input stream (buttons mapped like PS/2, motion scaled like PS/2, wheel as `CIUKI_INPUT_WHEEL`) and emit the `[console] [desktop] usb wheel value=…` record the QEMU case expects; the desktop/toolkit scroll on wheel events (verify `apps/desktop/input.c` and libdeskui handle the type).
3. Lag: the report path must not wait for the log or poll slower than the endpoint interval; measure the per-report cost in the host harness.

## Acceptance

USB host tests green; QEMU `usb-input-uhci`, `usb-input-ohci`, `usb-wheel-uhci` green; on the T23 right button, wheel and smooth motion.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary; host tests one at a time with `nice -n 19`. Report `build/codex/f1-68-report.md`.
