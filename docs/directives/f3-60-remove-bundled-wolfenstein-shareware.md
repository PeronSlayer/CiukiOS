# f3-60 — remove the bundled Wolfenstein 3D shareware from the image (licence: Apogee VENDOR.DOC §3C), keep the window-game support and the battery's test-media path

- **Owner:** Codex, `gpt-6.1-sol`, effort `high`
- **Written:** 2026-10-21 by the lead. The f3-53 cross-review read `third_party/games/wolf3d-shareware/VENDOR.DOC` §[3][C]: "If you wish to distribute … as part of a hardware or software bundle, or on CD-ROM, you must get PRIOR signed written permission from Apogee". A CiukiOS image that pre-installs the shareware episode is a software bundle; the 2026-10-12/14 inclusion relied on the "KEY POINTS" free-copying clause and missed §3C. Lead decision (owner informed, may override): the image stops shipping Wolfenstein 3D shareware; DOOM shareware (installed package, Carmack's clarification, FAQ files excluded) and Quake shareware (id SLICNSE §§6–7, f3-18) stay.
- **Worktree:** `../CiukiOS-wt-f3-60`, branch `wt/f3-60`

Required:
1. `scripts/build_image.py` (games disk: `\WOLF3D` and the comment at :305, the required directories at :607) and every payload inventory/provenance test stop including the Wolfenstein 3D files; the `third_party/games/wolf3d-shareware/` pin, `UPSTREAM.json` and notice stay in the repository as **test media** metadata (class `test-media`, the archive itself is not tracked; update `UPSTREAM.json`'s `redistribution` field to state §3C), consistent with f3-53's manifest classes.
2. The DOS window-game support for Wolfenstein 3D (`scripts/build_wolf_window.py`, launch records, Add/Remove Programs entry) stays as the path for a user-installed copy: the Start menu entry appears only when the game files are present (no dangling shortcut on a fresh image); Add/Remove Programs lists it only when installed.
3. Documentation: `docs/` user pages and `CHANGELOG.md` entry "Wolfenstein 3D shareware is no longer pre-installed (licence); install your own copy from Add/Remove Programs"; the licence inventory of the image loses the Wolfenstein notice; the game's VENDOR.DOC quotation goes into `docs/design/dos-title-battery.md` (test-media).
4. Host tests: image payload inventory, Add/Remove, desktop catalogue and the games-disk layout tests updated and green; nothing else changes.

Report `build/codex/f3-60-report.md`. `rg`/`sed` only; no QEMU, no `systemd-run`, no image builds, no commits, no diary edits; English.

## Withdrawn (owner decision, 2026-10-21)

The owner decided to keep Wolfenstein 3D shareware pre-installed ("lascialo comunque wolf3d"). This directive is withdrawn before any code change; the Codex session was stopped and the worktree removed. The licence reading above stays on record (dev diary 2026-10-21-05); the f3-53 manifest classifies the title `bundled` with a note pointing to that decision.
