# f2-54 — Compositor performance counters test fails after the Graphics Demo port onto the new presenter

- **Model/effort:** `gpt-6.1-sol`, `high`. **Worktree:** `../CiukiOS-wt-f2-54` from `main`. Host-testable.

## Evidence (main 13a976fc, 2026-10-14 01:35)

`python3 -m unittest discover -s tests/host -p test_desktop_performance.py` →
`performance: tests/host/desktop/performance_test.c:128: void assertions(void): Assertion 'd.frame_stats.damage_px==2150 && fake_presents==1' failed` (process exit -6). The desktop suite under `tests/host/desktop` and the gate tests pass. The merge of f2-52 (Graphics Demo) onto f2-50 (rendering architecture) ported the "repeated-row 2D blits" optimisation onto the direct presenter (`apps/desktop/compositor.c`, resolution session f2-52b); that port changed the damage/present accounting the test pins.

## Task

1. Determine whether the accounting changed legitimately (then update the expectation with the measured values and a comment explaining the f2-52 blit path) or whether the port double-counts or splits presents (then fix the presenter). Print the actual counters in the assertion message so the next failure is self-explaining.
2. Run `tests/host/test_desktop_performance.py`, the desktop suite (`tests/host/desktop`), the gate tests and the compositor goldens.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f2-54-report.md`.
