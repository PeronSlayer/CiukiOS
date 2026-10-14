# f2-56 — The desktop shows the compositor's fallback top bar until the first input (QEMU desktop-shell case)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f2-56` from `main`. Host-testable; the lead re-runs the QEMU case.

## Evidence (QEMU `qemu-t23`, main 0a9fb089, image 477685dd, 2026-10-14 02:50; run `build/test-runs/zz-desktop-shell/67503e28/`, screendumps copied to `build/lead-scratch/desktop-before-fallback-bar.ppm` and `desktop-after-shell-bar.ppm`)

The crash-isolation probe (`src/kernel/probes/f2_probes_desktop.c`) launches the desktop with `--gate`, runs 100 victim cycles (all `processes_equal=1` now), then arms the interaction (`event=ARM action=post_fault_input presents=1951 input_events=0`). The runner's pre-input screendump, taken after that ARM (and retried for up to 3 s), shows the **compositor fallback top bar**: the 32 px portrait at the left edge and the plain "CiukiOS" text, no Ciuki button frame (the fallback `scene()` path of `apps/desktop/compositor.c`). The post-input screendump, after four keys (`stage=after presents=2082 input_events=63 keys=4`), shows the **shell's top bar** with the Ciuki button and the 18 px mark, pixel-exact. The runner's T0 rule (`scripts/test/run.py`, `observe_desktop_screen`) requires the mark in both screens; this case passed on 2026-10-13 before the evening merges (f2-50 rendering architecture, f2-55 preallocated chrome caches, f2-53 tray, f4-12 sounds, f2-51 AutoPlay, f2-52 Graphics Demo).

## Task

1. Find why, in `--gate` mode after the first frames, the composited top bar comes from the compositor fallback instead of `desk_shell_draw` until an input event arrives: a shell overlay not composited in the new direct-composition path until its first damage, a chrome cache populated lazily by input, the gate shell's `shell` flag, or the fallback branch condition in `scene()`. Reproduce in the desktop host harness (desktop_gate_test or shell_launch_test): gate desktop, first frames, no input → the top bar must be the shell's button bar.
2. Fix so that the shell's top bar is composited from the first frame in both normal and gate modes; the fallback stays only for a desktop without a shell.
3. Add the harness assertion (first frame of a gate desktop contains the 18 px mark at (13,7) exactly like `tests/host/test_desktop_gate.py` expects) to the desktop host suite.

## Acceptance

Desktop host suites and `tests/host/test_desktop_gate.py` green; the lead re-runs `desktop-shell-qemu-t23` and `f1-usb`.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f2-56-report.md`.
