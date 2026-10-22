# f2-98b — Windows 9x behaviour for the compositor and every foreign surface (Win32, DOS window, NetSurf)

- **Phase/model:** F2, Codex gpt-6.1-sol, reasoning xhigh. **Worktree:** `wt/f2-98b` from `main`.
- Evidence: T23 trial of image 3423912f… (2026-10-22 ~22:40–22:50): serial `build/lead-scratch/t23-3423912f-serial.txt` (14,803 lines), disk log `build/lead-scratch/t23-3423912f-BOOT.LOG`, `legacy/local/physical/3423912f/NETSURF.LOG`. Music and window moves are now smooth (owner). Records: `[desktop] accel fills=13 copies=102 … frames=53 lfb_bytes=2346064`, `writes=27 … lfb_bytes=1050912` (1 MB of CPU writes to the LFB per 2 s at idle-ish load), `dosbox#62:301–381k` and `dosvm` 42 % while a DOS window is open.
- Owner: the compositor must behave like Windows 9x (very stable, fluid). Note on scope recorded by the lead: Windows 9x has no compositor (windows draw directly into the frame buffer with clip regions; the display driver accelerates BitBlt). CiukiOS keeps per-window surfaces because applications are separate processes; this directive implements the Windows 9x update discipline on top of them: only damaged, visible pixels ever reach the screen; window moves and scrolls are engine screen-to-screen copies of the visible region plus repaint of the exposed strips; occluded pixels are never drawn; nothing is redrawn for an unchanged frame.

## Required

1. Audit every path in `apps/desktop` that writes to the screen (compositor, presenter of Win32 windows `win32_presenter.c`, GL present, DOS window presenter and its frame cadence, NetSurf/nsfb surface presents, menus, tooltips, notifications, taskbar clock, tray) with the region it writes per event; table in the report.
2. Fix each to the discipline above; foreign surfaces (Win32, DOS, NetSurf) present only the changed rectangles they report (extend the present protocol with damage rectangles where missing, with a fallback for clients that do not send them); the DOS window path copies only changed scanlines of the VM frame (the VGA plane diff already exists or must be added) and runs at the VM's real frame rate, not a fixed poll.
3. Budgets on the T23 model: desktop idle ≤ 2 %; a DOS game in a window at its native rate ≤ 25 % for the presenter path; a Win32 window animating a 200×200 area ≤ 5 %; report per path.
4. Tests: golden frames, region-per-event tests, budgets; `test_kernel_latency`.

Do not edit libdeskui or the native applications (f2-98a) or cursor code (f2-99). Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Write `build/codex/<id>-report.md`.
