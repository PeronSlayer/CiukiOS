# f2-93 — `desktop.test_memory_footprint` red on main (wallpaper_boot_test.h:145, sound_runtime_test.c:130)

- **Phase/model:** F2, Claude agent (Sonnet). **Worktree:** `wt/f2-93` from `main`.
- **Evidence (f1-100 report, 2026-10-22):** `tests.host.desktop.test_memory_footprint` fails on an untouched main at `wallpaper_boot_test.h:145` and `sound_runtime_test.c:130`; the footprint binary's own `--memory-footprint` mode passes. Candidates: f2-90/f2-90b (wallpaper preload and the cache identity), f1-97 (notice card at the first frame), f2-88b, f2-91 (desktop queues).

## Required

1. Find which merge broke each assertion (git log on the two test files and the desktop sources; run the test at the merge commits if needed) and fix the product or the expectation with a one-paragraph justification each; no assertion weakened without a stated reason.
2. Run `tests.host.desktop.test_memory_footprint`, `test_desktop`, `test_v3`, `test_wallpaper_startup` and `test_kernel_latency` (nice -n 19, one at a time), all green.

Rules: no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English. Report `build/codex/f2-93-report.md`.
