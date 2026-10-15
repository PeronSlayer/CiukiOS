# f2-65 — Desktop presents only on change in the interaction stage; demo coalesces queued input (regression on image 2e39fe38)

- **Model/effort:** Claude opus agent (Codex credit exhausted until 2026-10-20). **Worktree:** `../CiukiOS-wt-f2-65` from `main`. Host-testable; the lead runs the QEMU evidence.

## Request

On image `2e39fe38` (main `c7fd0e8e`) the case `desktop-shell-qemu-t23` reported `keys=0`: the typed keys never reached the demo. Diagnose from the run's serial log and the desktop/demo sources, fix the cause, cover it with host replay models.

## Finding

Since f0-21 the desktop runs at interactive priority. In the interaction stage it redrew the whole screen every 1 ms, starving every normal task: the launched demo could not answer the close request within 2 s, was SIGTERMed, and the shell scenario never reached stage 6, hence `keys=0`.

## Task

1. The desktop presents in the interaction stage only when something changed (damage or input), never on a fixed 1 ms cadence.
2. The demo coalesces the queued input events per turn and redraws once.
3. Host replay models: slow client and fast desktop, with reap status and frames-per-second assertions.

## Acceptance

Desktop host suites and gate green; `desktop-shell-qemu-t23` passes with `keys` > 0 on the next image (verified on `35b9e5e4`: PASS). Files: `apps/desktop/*`, the demo, `tests/host/desktop/*`.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, one host suite at a time.

## State

Delivered and merged 2026-10-15 (main `70e124e7`).
