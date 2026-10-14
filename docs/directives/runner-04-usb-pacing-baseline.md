# runner-04 — USB input pacing: missing counter baseline on the first batches

- **Model/effort:** `gpt-6.1-sol`, `high`. **Worktree:** `../CiukiOS-wt-runner-04` from `main`. Host-testable.

## Evidence (QEMU `qemu-desktop-2002`, `f1-usb` case `usb-input-uhci`, main d2177fa1 with runner-03, 2026-10-14 11:05; run copied to `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/f1-usb-pacing/`)

`FAIL: usb-input-uhci — USB input consumption stalled action=0 batch=2 repeat=0 step=0 (missing counter baseline)`. The runner-03 barrier waits for a cumulative counter observation (`case=gate`, `native-desktop`, `interaction`, or `group=input` records, or anchored `[input]`/`[gate]` lines) before the next batch; at batch 2 of the first action no baseline exists because the probe emits its interaction counters only at the `interaction` stages (before/after) and the kernel's `[input]` records only on attach/detach, not per event. Keys and mouse are delivered by the kernel (f1-50/f1-51/f1-52 evidence); the shell-test stage machine needs the scripted path to land on the Start button (f2-58).

## Task

1. Give the barrier a baseline it can observe: the kernel's merged input queue publishes a cumulative counter record on demand or periodically in verbose/test boots (`[input] counters keys=… motion=… buttons=… consumed=…`, once per 250 ms while a test request is active and immediately after each delivered event batch, bounded), or the crash-isolation probe reports its gate counters per batch (`case=gate …`) in the desktop-shell cases; pick the cheapest that does not disturb timing and document it in `docs/design/test-architecture.md`.
2. The runner consumes those records for the barrier; when no record source exists in a profile, it refuses with a clear message before QEMU starts.
3. Host tests with the fake QEMU and, if the kernel side changes, the kernel record tests.

## Acceptance

Runner and kernel host suites green; the lead re-runs `f1-usb` (UHCI and OHCI) and `desktop-shell-qemu-t23`.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/runner-04-report.md`.
