# f1-75 — mount-crash probe: quiesce the boot-log drain across the crash-cut sequence

- **Phase:** F1 (filesystem probes). **Model:** gpt-6.1-sol / high. **Worktree:** `wt/f1-75` from main `56f8b659`.
- **Authority:** `src/kernel/probes/fat_probes.c` (`probe_mount_crash`), `src/kernel/core/bootlog.c` (a bounded pause/resume API only), `scripts/test/mount_fixtures.py` (diagnostic message only), host tests.

## Evidence (QEMU, case `mount-crash-reboot` of `tests/suites/f1-fat32.json`, image 27b35378 with f0-34; passed on 64a531f9 without f0-34)

The runner arms one blkdebug breakpoint (`break pwritev ciuki-write`) when it sees the `ARM action=crash_cut` record and expects the suspended request to be the probe's declared write (index 2). The serial shows `case=cut index=1,2,3` all completing and the runner reports `declared cut index already passed` (`mount_fixtures.py:202`): an unrelated write reached the breakpoint first. Since f0-34 the storage task drains the boot-log ring as soon as a backlog exists, so the ARM record itself triggers a log write to the same disk right after arming; before f0-34 drains were paced at one second and the probe's writes came first.

## Required

1. `probe_mount_crash` quiesces boot-log draining from just before the `ARM` record until the cut sequence has completed or failed (a bounded pause: the storage task skips `bootlog_drain` while paused, protected records keep accumulating in the ring, resume on every exit path; a pause longer than the probe's own deadline resumes by itself). No change to f0-34's classes or drain pacing on ordinary boots.
2. `mount_fixtures.py`: when the suspended request is observed before the first `case=cut` record, report "suspended write is not the probe's" distinctly from "declared cut index already passed" (message only, no behaviour change).
3. Host coverage: the probe's cut sequence with the production boot-log module shows zero log writes between ARM and the cut; pause/resume bounded; run `scripts/test/host_fs_tests.sh`, `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`, `python3 scripts/build_kernel.py`. Report in `build/codex/f1-75-report.md`. The lead reruns the f1-fat32 cases on QEMU.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`.
