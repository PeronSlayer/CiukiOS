# f2-53 — Taskbar tray polish

- **Model/effort:** `gpt-6.1-sol`, `high`. **Worktree:** `../CiukiOS-wt-f2-53` from `main`. Host-testable. Owner feedback 2026-10-13: the network and volume icons sit too far from the clock; the tray will gain more icons (removable media, later more).

## Task

1. Tray layout: icons packed right to left next to the clock with 4 px gaps and a 6 px margin, the tray width computed from its contents (no fixed `SH_TRAY_W`), a thin sunken frame around the whole tray, clock text vertically centred; order from the clock leftwards: volume, network, removable media, future icons. Tooltips and popups anchored to their icon; keyboard focus order unchanged.
2. Icon set: 16 px Tango icons already in the store; verify alignment of each icon's baseline and the disabled rendering.
3. Update the geometry record of `tests/host/desktop/shell_menu.py` and the goldens; the desktop case replays that click the tray must still pass.

## Acceptance

Desktop host suites green; screenshots in the report (host harness PNG) before and after.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f2-53-report.md`.
