# f2-87 — Add/Remove Programs: one list for Win32, DOS and native programs, including the bundled games; removal with confirmation

- **Owner:** Codex, `gpt-6.1-sol`, effort `high`
- **Written:** 2026-10-20 by the lead (owner's 1.0 Beta goal: a manager of installed programs that can remove them, including the freeware/shareware games bundled in the image)
- **Worktree:** `../CiukiOS-wt-f2-87`, branch `wt/f2-87`

## Model

Windows 98's Control Panel "Add/Remove Programs" (Install/Uninstall tab) as documented in the owner's Windows 98 Resource Kit (`build/rskt/` in the main checkout, read-only, never copied) and in Microsoft's documentation of the `Uninstall` registry key (`DisplayName`, `UninstallString`, `DisplayVersion`, `Publisher`, `InstallLocation`, `EstimatedSize`, `NoRemove`/`NoModify`). The existing Control Panel already lists the Win32 `Uninstall` key through the f5-07A adapter (`apps/control/control.h:62-75`, `apps/control/main.c:345-351`, `apps/desktop/win32_adapter.h`): extend that applet into the complete "Add/Remove Programs" applet; do not create a second one.

## Required

1. **One list.** Entries from: the Win32 `Uninstall` keys of HKLM and HKCU (existing adapter); the DOS games bundled on the games disk (`scripts/build_games_disk.py` packages with their `UPSTREAM.json`: name, version, publisher, size, licence file); DOS programs the user added under `C:\Games`/`C:\Programs` with a `.ciuki` or the shell's launch record; native programs added by the user (the `/Programs` entries that are not system components). Columns: name, version, publisher, size on disk; sorted by name; a system-component entry (desktop, Files, Control Panel, Terminal, the bundled DOS kernels, w32srv, fonts) is listed as protected and cannot be removed.
2. **Remove.** Confirmation dialog in English naming the program and, for bundled games, stating that the game cannot be restored without reinstalling the system. Win32: run the `UninstallString` through the existing adapter (`uninstall_run`), wait for it, then re-read the key. DOS game bundled on the games disk: delete its directory inside `C:\Games\CIUKIGAM.IMG` through the kernel's FAT code (the image is a FAT volume the DOS VM code already reads; find the existing write path or the owner-side mount used by `dosvm_disk.c`/f3-47 and use it with the C: write gate rules), invalidate the f3-51c digest cache and any per-VM copy-on-write data of that game, remove the menu entry, icon and settings; user-added DOS or native program: delete its directory and launch record. Every removal is logged (`[control] remove name=… kind=… result=…`) to the system log.
3. **Menus and launchers refresh** after a removal without a reboot: Programs/Games submenus, desktop shortcuts pointing at the removed program become invalid and are removed with the same confirmation, the DOS Box refuses to launch a removed bundled game with a clear message.
4. **Add.** The tab's "Install" button opens the Files picker on removable media and the CD-ROM for a `SETUP.EXE`/`INSTALL.EXE`/`.BAT` and launches it through the existing shell launch path (Win32 or DOS); no new installer engine.
5. **Tests.** `tests/host/desktop` Control Panel tests (list composition with a fixture registry, a fixture games manifest and fixture directories; protected entries; removal of each kind on a fixture tree and a fixture FAT image; menu refresh; the confirmation and the log line), `make CAP= apps` once, `(cd tests/host && python3 -B -m unittest test_desktop_gate)`; no image build (the lead verifies the games-disk removal in QEMU).

## Constraints

UI copy in English; Tango icons; no change to the canonical image or its games disk at build time; the bundled games' licences stay in `/system/licenses`; `rg`/`sed` only; no QEMU, no `systemd-run`, no image builds, no commits, no diary edits; host tests one at a time with `nice -n 19`. Report `build/codex/f2-87-report.md`.
