# f3-24 — Quake on the T23: interrupt-bound session, no frame, no screen text in the log (image a738204a)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f3-24` from `main`.

## Evidence (disk log lines 528–916, `window=Quake`, pid 4, VM generation 139279)

- `[launch] name=dosbox pid=4 click_us=25200000 … first_frame_us=31525000 total_us=6325000`; `[launch-image] pid=4 image_bytes=263006 image_requests=156 image_max=2048`.
- `group=vm_perf_share vm=1 n=1 wall_us=1008000 vm_permille=496 guest_permille=1 busy_permille=496`
- `group=vm_perf_cost vm=1 n=1 cause=rm_int exits=420 us=494836 avg_ns=1178180`; `rm_iret exits=215`; `io exits=336`; `rm_pushf 161`, `rm_popf 161`; `pit_pic 11`.
- `group=vm_perf_video vm=1 n=1…12 frames=0 flips=0 harvests=0 pages=0`; `vm_perf_rate frames_mfps=0` for every sample; no `group=guest` text record exists on hardware.

The guest executes about 420 real-mode software interrupts per second, each costing 1.18 ms inside the monitor (microseconds on QEMU), runs 0.1 % of the time itself and never produces a frame. The owner reports that Quake did not start; what DOS printed is unknown.

## Task

1. **Screen text on hardware.** At every launch and then every 5 s while `frames=0`, record the guest text screen as bounded `group=guest` records (the mechanism of the DOSREADY banner), so the lead can read the DOS messages from the disk log. Also record the vector histogram (`vm_perf_v`) in that case.
2. **The 1.18 ms interrupt.** Find which vector and which path cost it (suspects: INT 13h/21h reads served through the storage task in 2 KiB requests (`image_max=2048`), INT 16h polling with a sleep, a record emitted per interrupt). Fix the cost or the request size; document the measurement.
3. **Quake's start path.** Verify on the image that every file the shareware needs is present where the launch batch expects it, that the DOS session starts `QUAKE.EXE` from its directory with enough DPMI memory, and that the QEMU quake probe hashes the PAK in the guest at most once per boot (open item of 2026-10-13).

## Acceptance

QEMU `f3-vm` quake case green; host test for the screen-text record cadence; the next T23 log must show either frames above zero or the exact DOS message in the guest records.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary. Host tests one at a time with `nice -n 19`, `rg` not semble. Evidence files are read-only: `legacy/local/physical/a738204a/disk-BOOT.LOG` (T23 disk log of image a738204a, main `56d41c09`) and `serial.log` beside it. Plain technical prose in every document you touch, no product names of other operating systems, no filler. Report `build/codex/f3-24-report.md` with: root cause (file:line), files changed, test output verbatim, what the lead must verify on QEMU and on the T23.
