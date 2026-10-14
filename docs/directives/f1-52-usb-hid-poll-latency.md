# f1-52 — USB HID reports reach the desktop 1.4–2.5 s apart on QEMU

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f1-52` from `main`. Host-testable; the lead re-runs `f1-usb`.

## Evidence (QEMU `qemu-desktop-2002`, UHCI, `f1-usb` case `usb-input-uhci`, 2026-10-14; transcripts in `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/f1-usb-keys0/`, f2-58 validation `docs/validation/2026-10-14-f2-58-usb-shell-stages.md`)

The HID interrupt pipes are polled with `interval=10` ms (`[usb] hid poll addr=1 … interval=10 … result=-11`), yet reports reach the desktop in batches 1.4–2.5 s apart; QEMU merges the runner's relative mouse moves meanwhile (six corner-clamp moves plus the move onto Start collapse into five (-127,-127) reports), so the first click lands at (0,0). The USB service runs in its own P_DEVICE thread with a 250 ms port scan; the per-pipe poll records show `poll_frames=2 frames=4 elapsed_ms=4` for most polls and occasionally `poll_frames=49 frames=99 elapsed_ms=99`.

## Task

1. Measure in the host harness and explain the latency between a report's arrival in the controller frame and its delivery to the merged input queue: the service loop's sleep/yield cadence between polls, the host mutex held by the port scan or by storage transfers (f1-43 bulk), the two-frame retirement rule of f1-45/f1-50, the TCG frame timing. Target: a report delivered within 20 ms of its frame on QEMU and on hardware.
2. Fix the cadence (poll the interrupt pipes every frame interval without waiting for the scan loop; never hold the host mutex across sleeps; storage bulk transfers must not starve HID polls) with records of the measured latency (`[usb] hid latency addr=… ms=…` on the first 20 reports and a per-second maximum).
3. Host test: the fixture asserts delivery latency ≤ 2 polling intervals with a concurrent bulk transfer in flight.

## Acceptance

USB and kernel host suites green; i686 kernel compiles; the lead re-runs `f1-usb` with the runner pacing of runner-03.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f1-52-report.md`.
