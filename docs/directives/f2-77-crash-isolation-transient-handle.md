# f2-77 — crash-isolation: a transient desktop handle fails the restored-ledger comparison

- **Phase:** F2+ (desktop / probes). **Model:** gpt-6.1-sol / high. **Worktree:** `wt/f2-77` from main `bba7acaf`.
- **Authority:** `apps/desktop/desktop.c` (periodic polls: `media_read`, network state, mixer), `src/kernel/probes/f2_probes_crash*.c` (restored-ledger snapshot), `tests/host/desktop`, `tests/host` probe fixtures.

## Evidence (QEMU `qemu-t23`, f1-usb cases `usb-input-uhci` / `usb-input-ohci`, selector `f2:crash-isolation server=desktop`)

Intermittent on three images (79df4293 cycle 28 OHCI, a34118c1 cycle 9 UHCI, 64a531f9 cycle 33 UHCI; all three cases passed on 401f3caf):

```
probe=crash-isolation event=DATA case=restored server=desktop cycle=33 equal=1 processes_equal=0 procs=4/4 threads=2/2 zombies=0/0 handles=19/20 extents=141/141 backing=5262/5262 tables=9/9
probe=crash-isolation event=DATA case=ledger server=desktop objects_equal=1 processes_equal=1 descriptions=0 surfaces=0 pages=0 channels=0 messages=0 grants=0
probe=crash-isolation event=END server=desktop status=FAIL reason=desktop_contract
```

Every other cycle of the run and the final ledger are equal: one extra handle existed only at the moment of the restored snapshot. The desktop opens and closes descriptors on every poll (`media_read` opens `/dev/media` twice per turn, the network poll opens `/dev/net`, the mixer poll its device), so the snapshot can observe one of them open. Run directories: `build/test-runs/zz-usb/d395a9f8` (64a531f9), `zz-usb/badc5642`, `zz-usb/7fd10c05` (read-only).

## Required

1. Identify the handle from the kernel's per-process ledger at the snapshot (extend the `restored` record with the process and kind of the extra handle if needed to prove it) and confirm the poll race on the host desktop harness.
2. Fix on the desktop side: keep the `/dev/media`, `/dev/net` and mixer descriptors open for the desktop's lifetime (opened once, closed at shutdown; reopen only after an error), which also removes the open/close churn from every poll. If the probe's contract legitimately requires a quiescent handle count, make the comparison tolerant only of the desktop's own long-lived descriptors, never of leaked ones.
3. Host coverage: a desktop poll interleaved with the snapshot shows equal handle counts; the three f1-usb cases keep their predicates. Run `make CAP= desktop`, the desktop suite, the gate and `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`. Report in `build/codex/f2-77-report.md`.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`.
