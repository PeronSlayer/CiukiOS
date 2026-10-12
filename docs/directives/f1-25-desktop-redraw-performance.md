# f1-25 — Desktop redraw performance and stability on real hardware

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f1-25` from `main`. Host-testable (compositor model with counters); the lead measures on the T23 with the `L` present records of f1-24.

## Owner report (2026-10-13, T23)

"Il refresh grafico e la stabilità nel sistema sono carenti: lagga il redraw." Write-combining is active (`[display] mtrr wc ... result=ok`), so the remaining cost is in the compositor and its presents. Also seen: a display mode switch back to 1024x768 returned `result=-16` (EBUSY) right after a switch to 1280x1024 (mode 283 ok, mode 280 -16, then ok later).

## Task

1. **Measure** on the host model and expose on hardware: per frame, the damage area, the number of presents, bytes copied to the LFB, time in composite versus present versus input handling; a verbose-boot record every 2 s (`[display] frames=.. damage_px=.. present_us=.. composite_us=.. input_lag_us=..`) and a Task Manager "System" line.
2. **Fix the pipeline**: present only the damaged rectangles (never a full-screen present for a cursor move, a clock tick, a hover or a menu), coalesce damage per frame, double-buffer the composite so a partial repaint never shows tearing, draw the cursor as an overlay with save-under instead of re-compositing, cap the frame rate at the refresh rate and never spin, keep input handling ahead of rendering (process all pending input events, then one composite, then one present), and make wallpaper/icon-label caches cover the whole work area so a window move or menu close only copies. Measure before/after on the host model and state the expected T23 numbers.
3. **Stability**: find and fix sources of stalls visible on hardware: synchronous disk writes on the UI thread (settings, logs), the 8042/mouse polling cadence, the boot-log drain contention, blocking IPC waits, the 1 s clock repaint hitting the whole taskbar.
4. **Display switch EBUSY**: find why the second `ciuki_display_set` within the confirmation window fails with -16 (display grant held by the preview, the Control Panel's own window, or a DOS VM) and make the Control Panel sequence correct (wait/retry, clear message).
5. Tests: compositor model counters (damage coalescing, no full presents on cursor/hover/clock, double-buffer equivalence), input-before-render ordering, display switch sequence.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, one host suite at a time. Report `build/codex/f1-25-report.md` with the before/after numbers.
