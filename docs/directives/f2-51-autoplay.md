# f2-51 — AutoPlay for removable media and CDs

- **Model/effort:** `gpt-6.1-sol`, `high`. **Worktree:** `../CiukiOS-wt-f2-51` from `main`. Host-testable. Owner request 2026-10-13: "an autorun like Windows".

## Task

1. When a removable volume or a CD mounts (`/dev/media` events of f1-43/f1-48), the desktop shows an AutoPlay dialog in the Windows 98/ME style: volume label and drive letter, actions "Open folder to view files" (Files on that drive), "Play music" when audio files are found at the root or in a `MUSIC`/`Music` folder (Music Player playlist), "View pictures" when images are found (Image Viewer), "Run <program>" when `AUTORUN.INF` declares `open=` a DOS program on the medium (DOS Box with that program and working directory; Win32 entries are shown disabled until F5), and "Do nothing"; a checkbox "Always do this for this device" stored in the registry (`Desktop\AutoPlay\<volume id>`), and a Control Panel AutoPlay page to review the choices. Parse `AUTORUN.INF` (`[autorun]` section, `open`, `icon`, `label`) and use `icon=` for the drive icon in Files when it points to an `.ico` on the medium (the icon decoder already exists for Tango/ICO? check; otherwise the label only).
2. Sound: the device-connect and disconnect sounds of f4-12 when available.
3. Host tests through the desktop harness: mount events for a stick with music, a CD with AUTORUN.INF, a stick with pictures, the "always" preference, eject while the dialog is open.

## Acceptance

Desktop host suites green; `make CAP= apps` builds; docs updated (`docs/desktop.md`).

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f2-51-report.md`.
