# f3-22 — DOS full-screen probe: wait for DOOM's video mode up to 300 s, case timeout 600 s (TCG timing)

- **Model/effort:** `gpt-6-luna`, `low` (mechanical) or a Claude agent while Codex credit is exhausted. **Worktree:** `../CiukiOS-wt-f3-22` from `main`. Host-testable; the lead runs the QEMU evidence.

## Request

On QEMU TCG (`qemu-t23`) DOOM needs about 177 s after the typed command to reach mode 0x13 unchained (run `7d24c1c4` on image `94542c30`: DOS ready at 13 s, `ARM action=fullscreen_enter` at 190 s), while `probe_f3_vm_fullscreen` (`src/kernel/probes/f3_probes_video.c`, line ~370) waits 180 s. Run `50a5480e` on image `5ff88a8a` gave up at 198 s and the runner's 360 s case timeout closed the VM before the END record. The case is therefore marginal on TCG; nothing is wrong in the product.

## Task

1. Raise the probe's wait for DOOM's video mode to 300 s and record the elapsed milliseconds in a `DATA case=launch ms=…` record so the margin is visible in the evidence.
2. Raise `timeout` of `vm-fullscreen-qemu-t23` and `vm-fullscreen-cirrus-qemu-t23` in `tests/suites/f3-vm.json` to 600 (and keep every other field).
3. Host test: the suite's two full-screen cases carry `timeout` 600 and the probe source contains the 300 s wait (`tests/host` runner tests, same style as `test_runner_and_kernel_selectors` in `tests/host/win32/test_kernel_core.py`).

## Acceptance

Both full-screen cases pass on the next image (`[display] fullscreen=1 mode=19` within 200 ms of Alt+Enter, `case=demo active=1 ms>=10000`, `case=restore active=0`, END PASS), the Cirrus case included. The 10-minute F5 gate rule of 2026-10-14 (one long case per image, at night) applies to the sweep, not to these two cases.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`. Report `build/codex/f3-22-report.md`.

## State

Proposed 2026-10-15.
