# f2-61 — Desktop freezes after a theme change followed by a wallpaper change on the T23 (image ce2e6ce9)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f2-61` from `main`. Host-testable; the lead runs the desktop QEMU cases and the T23.

## Evidence (T23, image ce2e6ce9, first boot: serial capture `legacy/local/physical/ce2e6ce9/serial-clean.log` lines 1-339, lossy; the disk log of that boot was overwritten by the second boot)

Owner: after changing the theme the wallpaper disappeared; opening the wallpaper change the window stopped responding and the whole desktop froze (forced power-off). The serial tail shows the kernel alive (USB worker heartbeats, Savage status) while `[console] [display] frames=0 ... presents=0` repeats: the desktop process stopped producing frames. Second boot: `stage=entry us=0`, `settings_read us=9880000` (9.9 s, the Savage init of f4-22 was running on its thread for 10 s and the desktop's settings read waited behind it: f4-26 handles that), wallpaper from cache at 12.1 s, `first_input_handled us=14917000`; later samples with the Control Panel open: `present_us=240000 composite_us=120000 input_lag_us=133000` per 2 s. Changes in the desktop since the last good image: f4-18 (tray/popup refresh 250 ms, mixer worker, registry save), f4-20 (demo pipeline), f2-60 (icon family at init, reserved folder index, 4 MiB master arena, footprint harness), f1-59 (desktop.c: shell probe gate counters).

## Task

1. Reproduce in the host desktop harness the exact sequence: theme Platinum → Blue (Control Panel display settings), then wallpaper change (Control Panel/wallpaper dialog), with the wallpaper worker, the settings worker and the mixer worker all alive, under ASan/UBSan and a watchdog; find the deadlock or livelock (mailbox full, worker waiting on the main loop, presenter lock, theme invalidation freeing the wallpaper cache while the worker publishes, registry save worker and arena) and fix it.
2. The wallpaper must survive a theme change (cache keyed by theme? re-render instead of dropping); a theme change must not take seconds: measure the cost of the theme switch (icon family re-preparation of f2-60 at four sizes?) and bound it.
3. Add a desktop watchdog record: if the render loop makes no frame for 2 s while input is pending, emit `[desktop] stall stage=... worker=...` through the kernel console so the disk log explains the next freeze.

## Acceptance

`tests/host/desktop` and the gate green with the new sequence test; the lead runs `desktop-shell-qemu-t23`, then the T23: theme change and wallpaper change without a freeze, wallpaper kept. Files: `apps/desktop/*`, `tests/host/desktop/*`. Not `theme.c` colours (f2-62).

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Research the references first where named (`--search`), record links and decisions in the relevant `docs/design/` or `docs/validation/` document, then implement and test. Report `build/codex/f2-61-report.md`: what the T23 evidence proved, what changed, which host tests prove it, what the lead must verify.
