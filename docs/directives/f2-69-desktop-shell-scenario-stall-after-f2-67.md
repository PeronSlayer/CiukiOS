# f2-69 — Desktop keys regression on image aae6a170: the shell scenario stalls before stage 6

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f2-69` from `main` (`0712bc59`). Host-testable; the lead runs the QEMU evidence.

## Evidence

QEMU gates on image `aae6a170` (main `0712bc59`): `desktop-shell-qemu-t23` and `usb-input-uhci` fail with `desktop did not consume every input kind: keys=0 motion=47 buttons=12` (run `d555dcef`). The gate reports keys only when the shell scenario reached stage 6 with at least two gate keys (`apps/desktop/desktop.c:135`); motion and buttons are routed by pointer hit test and were consumed; the desktop CPU share was normal and the demo client alive (not the f2-65 starvation). The same cases passed on image `5ff88a8a` (main `56d41c09`); in between main merged f2-67 (resizing, cursors, activation and stacking rules, close fallback, cascade, outline transitions, modal blocking) and f2-68 (text selection, paint cursors). The desktop host suite (101) and host gate (10) pass on this tree: the host replay does not reproduce the QEMU stimuli.

## Task

1. Find the stalled stage by correlating the runner's stimuli (key codes, pointer coordinates and clicks of the suite case in `tests/suites/f2-desktop.json`) with the new behaviour (suspects: cascade moved the demo window so a scripted click misses; close fallback or activation leaves focus elsewhere; outline transitions; the scenario's own expectations in `apps/desktop/shell.c`).
2. Fix the cause in the desktop; if scripted coordinates are wrong because windows legitimately moved, the scenario must use the window's actual geometry.
3. Extend the host shell replay so it fails before the fix and passes after, with the same stimuli as the QEMU case.
4. Desktop suite and gate green.

## Acceptance

`desktop-shell-qemu-t23` and `usb-input-uhci` pass on the next image with `keys` > 0. Report `build/codex/f2-69-report.md`.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary; host tests one at a time with `nice -n 19`; `rg` not semble.
