# f1-113b — Persistent joystick selection and calibration

- **Phase/model:** F1, Codex gpt-6.1-sol, reasoning high. **Worktree:** `wt/f1-113b` from `wt/f1-113`.
- **Lead decision (2026-10-23 01:11)** on decision 2 of the f1-113 report: a retro gamer must not reselect or recalibrate a joystick after every reboot.

## Required

1. **Calibration** is stored per controller identity: USB VID/PID plus serial or interface path when there is no serial, and the game port by its I/O base. It is reapplied when the same controller appears again.
2. **Per-game joystick choice** (the f1-113 catalog preference) is stored by controller identity and survives reboots. If that controller is absent, the game falls back to the first available joystick with a visible note.
3. **Storage** uses the system configuration pattern of the Control Panel (registry or INI, whichever f1-113 already uses), with versioned records and corrupt-record fallback to defaults with a record.
4. **Tests:** reboot round trip in the host model, unplug and replug with a changed port, two identical controllers, corrupt storage, and migration from no stored data.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English. Host tests one at a time with `nice -n 19`. Write `build/codex/f1-113b-report.md`.
