# Integrated serial wait and display relayout regressions

Research checked on 2026-10-16 against checkout `320211e6`:

- [Upstream wait queue implementation](https://raw.githubusercontent.com/torvalds/linux/v6.12/kernel/sched/wait.c),
  `prepare_to_wait`: publish the queue entry and task state under the IRQ
  lock, then restore the caller's flags. This agrees with the F0-30
  `kwait_wait_until` implementation. The serial fixture must enter and leave
  `schedule` with IF enabled; the caller reacquires its queue lock. Keep the
  producer port-access checks, whole-record comparisons, wait/wake checks
  and panic takeover coverage unchanged.
- [Window show state and tracking geometry](https://learn.microsoft.com/en-us/windows/win32/winmsg/window-features#minimized-maximized-and-restored-windows):
  maximization uses system-managed geometry, disables user border sizing,
  and preserves the previous normal geometry for restore. Ciuki's existing
  contract applies these concepts to its own work area and client limits.
  This reference supplies interaction semantics, not an implementation or ABI.

Repository finding: `desk_resize` does not reject zoomed clients in this
checkout. `desk_resize_hit` and `desk_keyboard_geometry` block user sizing.
`desk_display_relayout` passes the old origin to `desk_resize`, whose available
size calculation therefore clamps a requested maximized frame prematurely.
Set the requested origin to `(0, DESK_BAR)` before resizing. Retain client
minimum/maximum/step constraints, CONFIGURE serial generation, restore clamping
and the existing input rules.

Host coverage includes maximized growth/shrinkage, same-dimension relayout
for depth changes, saved normal geometry and rejected mouse/keyboard sizing.
Verbatim commands, results and final source references are recorded in
`build/codex/night-fix-report.md`. These are host checks; hardware display
switching and integrated image validation remain lead validation.

Final results: sanitized standalone serial replay PASS; five display-mode
tests OK; native desktop build exit 0; desktop suite 101 tests OK; desktop
gate ten tests OK; full kernel host suite exit 0 with leak detection disabled.
The desktop suite, gate and kernel suite each ran once, sequentially.
