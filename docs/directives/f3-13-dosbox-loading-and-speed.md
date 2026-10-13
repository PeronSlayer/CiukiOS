# f3-13 — DOS Box: hide the DOS boot, show a loading window, and make DOOM fast on the T23

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f3-13` from `main`. Host-testable (replays and the VGA presenter harness); the lead measures on the T23.

## Evidence and owner feedback (T23, image e86305de, 2026-10-13; transcript `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/e86305de-usb-storage-fault-optical-missing/serial-clean.log`)

DOOM now runs (the f3-12 fix holds) but "lags a lot"; Wolfenstein 3D lags less. When a DOS program or game starts, the window shows the whole CiukiDOS boot text before the program; the owner wants the program directly, with a small loading window meanwhile.

## Task

1. **Loading window**: the DOS Box window stays hidden (or shows a "Loading <program>…" panel with the program's icon and a progress indication) until the program sets its video mode or writes its first frame; the CiukiDOS boot messages go to the VM's log, not to the screen; errors during boot still surface in the panel. Keep the existing behaviour available for the plain "DOS Box" prompt.
2. **Performance**: measure where the time goes for DOOM on the T23 using records (per second: VGA harvests, bytes converted, present time, VM run time vs monitor time, V86 exits per second by cause, PIT/IRQ delivery counts, VGA writes trapped vs direct through the f3-07 chained window). Then fix the largest costs: direct linear window for mode 13h with no trapping, palette-indexed to 32 bpp conversion by rows with a lookup table, present only changed rows (dirty tracking per 8-line band), harvest at the game's frame rate instead of a fixed timer, the DOS VM scheduled at a higher share while its window is focused, no verbose records inside the VM's hot paths in normal boot.
3. Targets on the T23: DOOM at 35 frames per second in the standard window (320x200 scaled 2x), Wolfenstein 3D at 70; measured by the records.

## Acceptance

`f3-workloads` and the kernel host suites green; the VGA presenter host benchmark before/after in the report; the lead measures on the T23.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f3-13-report.md`.
