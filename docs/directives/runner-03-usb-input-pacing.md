# runner-03 — Pace relative input to guest consumption in USB cases

- **Model/effort:** `gpt-6.1-sol`, `high`. **Worktree:** `../CiukiOS-wt-runner-03` from `main`. Host-testable.

## Why

f2-58 showed that with USB input QEMU merges consecutive relative mouse moves sent by `scripts/test/run.py` before the guest polls its HID pipe, so the scripted path (corner clamp, move onto the Start button, clicks) collapses. The runner currently paces batches by time; QMP acceptance does not prove consumption.

## Task

1. For cases whose profile declares `usb` input, add a consumption barrier between relative-move batches: wait until the probe's interaction counters (`motion=`, `buttons=`, the `[input]`/`gate` records already emitted every batch) show the previous batch consumed, with a bounded timeout and a `Refusal` message naming the stalled batch; keep the PS/2 behaviour unchanged (opt-in through the profile's input topology).
2. Split each relative move into ≤ 127-unit steps when the target is a USB mouse (boot-protocol report range), preserving the resulting position; record the pacing in the evidence (`input_pacing: {batches, waits_ms}`).
3. Host tests with the fake QEMU: USB profile paces and splits, PS/2 profile unchanged, timeout refusal.

## Acceptance

Runner host tests green; the lead re-runs `f1-usb` after f1-52.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble. Report `build/codex/runner-03-report.md`.
