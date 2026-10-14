# f2-64 — UI refinement: message boxes, system notifications, taskbar tooltips and balloons (owner request 2026-10-14)

- **Model/effort:** `gpt-6.1-sol`, `high`. **Worktree:** `../CiukiOS-wt-f2-64` from `main`. Host-testable; the lead reviews PNGs and the T23.

## Request

The owner asks for a graphic refinement of the desktop's transient UI, in the Ciuki Blue theme now default (f2-62): message boxes and system notifications (error/warning/info/question dialogs the desktop and applications show, for example "Program did not start", AutoPlay, shutdown prompts), popup messages, the tooltips and balloons shown when hovering or opening taskbar entries and tray icons (volume, network, removable media, clock), and the loading panel. Identity rules of `AGENTS.md` apply (approved Ciuki portrait, Tango icons, English copy).

## Task

1. **Message box family** (`apps/libdeskui/dialog.c`, desktop and application call sites): Windows 98-class layout with the Blue theme: icon column (Tango dialog-error/warning/information/question at 32 px), bold title in the caption gradient, body text with proper wrapping and margins, buttons of uniform width with default/cancel emphasis, keyboard mnemonics, sound events (f4-12 UI sounds) per class; a shared helper so every caller gets the same look; modality and focus return already in the toolkit.
2. **Notifications**: a small system notification panel near the tray (bottom-right, slides in, auto-dismisses after a timeout, click to open): used for removable media attach/detach, network link changes, volume changes from the hardware keys (EC), low battery if the kernel reports it, and errors from workers (settings save failure with the exact error text instead of a bare number: "error 30" becomes "Read-only file system (30)").
3. **Tooltips and balloons**: taskbar buttons show the full window title on hover; tray icons show status tooltips (volume level, link state and address, media letter and label, clock date) with a delay and consistent styling; balloons with a pointer for events that need attention.
4. **Loading panel** and progress: consistent frame, portrait icon, progress bar styling of the theme.
5. Goldens and PNGs: re-record affected goldens; put before/after PNGs under `build/host/f2-64/` (message box of each class, a notification, a tooltip, a balloon, the loading panel) for the lead.

## Acceptance

Desktop host suites green with re-recorded goldens; the lead checks the PNGs and the owner judges on the T23. Files: `apps/libdeskui/*`, `apps/desktop/*` (tray, shell, loading panel), application call sites, tests/goldens. Coordinate with f2-63 (FreeType) at merge: goldens may be re-recorded twice; keep both changes.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f2-64-report.md`.
