# f0-55b — Splash bar: visible fill and motion from the first splash frame

- **Phase/model:** F0, Claude agent (Opus). **Worktree:** `wt/f0-55b` from `main` (`08738760`).
- **Evidence (lead, QEMU qemu-t23 profile, image `288052706a…`, 2026-10-22 04:27):** the splash appears at about 13 s (after the loader stage and the accel probe), the desktop at about 17 s; screendumps at 13.0, 14.5 and 16.0 s show the bar track empty and identical (no sweep segment, no fill), while the handover record reports `phases=14 updates=14 claims=16`. On the host golden frames the fill is visible. So either every phase is reported before the splash is painted and the first paint does not apply the accumulated level, or the console worker's 40 ms indeterminate tick never runs between the splash paint and the handover, or the fill is drawn only in the last update.

## Required

1. From the first splash frame the bar must show state: if phases were reported before the splash was painted, the first paint draws the accumulated level (never an empty track after a reported phase); before any phase, the indeterminate sweep must visibly move at the 40 ms tick on the console worker, independent of klog traffic, and the worker turn must be scheduled while init runs (document which task runs it and how it is woken).
2. The level must reach the eye: the last phases (desktop image loaded 97 %, grant 100 %) must be drawn before the handover, and the frame at handover is 100 % (already covered) — add a serial record per phase draw when `serial=1` (`[console] splash phase=<n> permille=<p>`), bounded to 14 lines, so the T23 serial shows the timeline.
3. Order of init on QEMU: report where the 13 s before the splash go (loader ELF stage, accel probe `us=15006917`) and whether the splash can be painted before the accel probe on the real init order; if the probe runs after the framebuffer is ready, paint the splash first (no behaviour change for the probe).
4. Host tests: a test that reports three phases before the first paint and checks the first frame's fill; a test that the worker tick advances the sweep with no phases and no klog; the phase record format.

The lead verifies with QEMU screendumps at 13–16 s (the bar must differ between two frames one second apart or show a non-zero fill) before rebuilding the hardware image.

Rules: no QEMU, no image build, no git mutations, no diary edits, nothing deleted outside the worktree, `date` before timestamps, English. Report `build/codex/f0-55b-report.md`.
