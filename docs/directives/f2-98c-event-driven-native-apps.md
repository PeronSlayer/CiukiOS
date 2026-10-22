# f2-98c — Event-driven native applications (no periodic idle wakeups)

- **Phase/model:** F2, Codex gpt-6.1-sol, reasoning high. **Worktree:** `wt/f2-98c`. Starts after f2-88c (native event/deadline waits, Control Panel jobs on the completion channel) merges.
- **Source:** decision 1 of the f2-98a report. The view does no rendering when nothing changed, but native loops still wake at 100 Hz for completion callbacks. Windows 9x model: a blocked thread is not scheduled until its event arrives (`win98-class-resource-management.md`).

## Required

1. **libdeskui run loop** (`du_run`) blocks on the window channel, the timers' nearest deadline and the registered job completions through the f2-88c wait. There is no fixed-period idle tick; timers fire at their deadlines only.
2. **Convert every native application and applet** that polls in its idle callback to completion events. That covers Files, Notepad, CiukPaint, Music Player, Tasks, Device Manager, the shell, the tray and NetSurf's chrome loop where applicable. Keep each application's behaviour and goldens.
3. **Host tests:** for each application, an idle window performs no periodic wakeups for 10 s of model time, and input or completions wake it within one turn.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English. Host tests one at a time with `nice -n 19`. Write `build/codex/f2-98c-report.md`.
