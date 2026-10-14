# f2-60 — Desktop allocates one extent after the first frame again: the crash-isolation gate fails on QEMU (image 93a04a4c)

- **Model/effort:** `gpt-6.1-sol`, `high`. **Worktree:** `../CiukiOS-wt-f2-60` from `main`. Host-testable; the lead runs `desktop-shell-qemu-t23` and `f1-usb` on QEMU.

## Evidence (QEMU, image 93a04a4c of main 3ebc9e26, runs `build/test-runs/zz-desktop-shell/7584e655` and `build/test-runs/f1-usb/2f99c787`)

Both `desktop-shell-qemu-t23` and `usb-input-uhci` end with the crash-isolation probe's `END status=FAIL reason=desktop_contract` ("terminal evidence before declared stimuli completed" for the runner). The `restored` record after one victim cycle:

```
case=restored server=desktop cycle=1 equal=1 processes_equal=0 procs=4/4 threads=2/2 zombies=0/0 handles=19/19 extents=31/32 backing=3971/3972 tables=8/8
```

One extent and one backing page appear in the desktop process after its first frame (the gate requires a frozen ledger: f2-55 preallocated the chrome caches and first-use buffers for exactly this). On image fb3147dc (before this wave) the same case passed. Candidates merged since: f4-18 (tray/popup refresh every 250 ms following the EC master, the delegated mixer worker that saves `System\Audio\Master` through libreg, `sound_storage` registry load), f4-20 (demo frame pipeline, FRAME/PRESENTED extensions, settings log capacity), f3-17 (none in the desktop), f3-18 (catalogue entry only). The host test `tests/host/desktop/test_memory_footprint.py` (`allocations_after_first_frame=0`) passed, so it does not exercise the path that allocates.

## Task

1. Find the allocation: run the desktop under the host footprint harness through the 250 ms refresh ticks, a mixer poll with and without an EC change, a preference save, the settings log at capacity, and a demo window present with the extended FRAME; make the harness fail on it, then preallocate or remove the lazy allocation (f2-55 style: fixed buffers at init, no first-use growth after the first frame).
2. Extend `test_memory_footprint.py` so every periodic path of the desktop (timers, workers, pollers) runs at least once under the allocation guard.
3. Keep behaviour: the tray/Control Panel still follow EC changes; saves still go through the worker.

## Acceptance

`tests/host/desktop` suite and `test_desktop_gate.py` green; the lead runs `desktop-shell-qemu-t23` and `f1-usb` on QEMU: `processes_equal=1`. Files: `apps/desktop/*`, `tests/host/desktop/*`. Do not touch the kernel or the USB stack (f1-59).

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f2-60-report.md`.
