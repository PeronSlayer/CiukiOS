# f0-27 — The serial line-rate quota of f0-24 drops boot evidence on QEMU (image c074bb8f)

- **Model/effort:** `gpt-6.1-sol`, `high`. **Worktree:** `../CiukiOS-wt-f0-27` from `main` (`d4900403`). Build the kernel before reporting.

## Evidence

`normal-boot-qemu-t23` (f0-smoke) on image `c074bb8f`: the kernel boots and the desktop runs (`timer_gaps=0/0`), but the capture has 99 lines and ends with `serial_drop=16` / `[serial] dropped=16`; the runner's expected records never arrive and the case times out. The runner reads QEMU's UART chardev, which has no line limit.

## Task

Evidence (`CIUKI_TEST`, probe records) and boot-stage records are never dropped: task-context producers wait for quota (`serial_evidence_begin`), only periodic diagnostics may be dropped whole; under the TCG hypervisor signature or with the fw_cfg test selector the quota is lifted to the chardev rate (FIFO gating kept); hardware keeps the 75 % quota. Host test for both rules; kernel host suite; kernel build.

## Acceptance

`f0-smoke`, `f1-usb`, `f3-vm` green on the next image; hardware capture still whole-record.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary; host tests one at a time with `nice -n 19`. Report `build/codex/f0-27-report.md`.
