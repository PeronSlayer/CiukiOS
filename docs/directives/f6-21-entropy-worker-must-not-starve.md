# f6-21 — The entropy worker's jitter retry starves other tasks (f0-core `preempt` red)

- **Phase/model:** F6/F1, Claude agent (Opus). **Worktree:** `wt/f6-21` from `main`.
- **Evidence (lead, QEMU qemu-t23 profile, image 6b586080 of 2026-10-22 10:00):** f0-core probe `preempt` ends `status=FAIL reason=preemption` with `dispatch_a=5000 dispatch_b=5000 max_wait_a=59 max_wait_b=59` (ticks; the probe allows about twice the 1-tick quantum) while `[cpu] top=entropy#10:375k375s57,spin-b#18:295k0s0,spin-a#17:281k0s0` — the entropy worker (f6-19's runtime jitter retry at 1 s, 30 s, 120 s) runs the jitter collection as a tight loop for hundreds of milliseconds at an effective level (`s57`) that keeps the two spinning probe tasks off the CPU for 59 ms. On the T23 the same worker runs beside the audio refill and the desktop. The probe passed before f6-19.

## Required

1. The jitter collection at run time never starves anyone: it runs at the lowest level (idle class or `P_NORMAL - n`), in bounded slices (≤ 1 ms of collection per turn, then a yield/sleep), never with the starvation boost, and it pauses while the audio refill or any foreground task is runnable; total wall time may grow accordingly.
2. The boot-time collection before the scheduler starts stays as it is (f6-16/f6-19); the first runtime retry waits until the desktop's first frame is presented.
3. Host tests: `preempt`-style fairness test with the worker active (two spinners get the CPU within 2 ticks), the slice bound, no starvation boost for the worker; `test_kernel_latency` with the worker running and zero `audio_refill_overrun`; the entropy gate still reaches `health=ok` on the T23 fixture.

Rules: no QEMU (the lead reruns f0-core `preempt`), no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English. Report `build/codex/f6-21-report.md`.
