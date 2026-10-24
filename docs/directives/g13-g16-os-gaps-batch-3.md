# OS feature gaps, batch 3 (G13–G16 of the fx-20 inventory)

- **Written:** 2026-10-23 23:04 by the lead (owner: continuous development for several days). Scope, rules and compile/ABI requirements are those of `g7-g12-os-gaps-batch-2.md` (header), binding here too; each item's scope is the inventory rows and work-item text of its G row in `docs/design/os-feature-inventory.md`.
- **Implementers:** Claude Code CLI sessions on the second account (owner 2026-10-23) through `build/lead-scratch/launch-claude.sh`, one worktree each from `main`; Codex when it has capacity.

## G13 Native fault dialog, shell restart, log viewer
Rows ER-1, ER-2, ER-3, ER-5, SY-8 — a dialog for native program faults (program name, "This program has performed an illegal operation and will be shut down", Details with registers/fault address, a crash file under `C:\SYSTEM\LOGS`); the desktop restarted by the supervisor after a desktop crash instead of the text console; a formatted fatal-error screen with a restart choice; a log viewer accessory for BOOT, NETSURF and CRASH files. Size M.

## G14 Startup menu, ScanDisk, registry backups
Rows BT-2, BT-3, BT-7, BT-8, BT-10, BT-11, BT-12, ST-9, SY-10 — Ctrl/F8 hold and an automatic menu after a failed boot (Normal, Logged, Safe mode, Step-by-step confirmation, Command prompt only → Terminal only, Safe mode command prompt); a visible ScanDisk on a dirty boot and on demand (report, `.CHK` salvage, cross-link repair) built on the existing FAT repair code; five dated registry backups with restore at boot (scanreg-like); a system-file checker against `build-manifest.json`; persistent skip list. The loader is size-limited: state the free bytes after your change. Size L.

## G15 Help everywhere
Rows HP-1…HP-4, KB-11, SM-7 — F1 opens the program's topic in Help; contents, index and search; Shift+F1 and the "?" caption button with WM_HELP; toolkit tooltips; topics for every built-in program (English); Start ▸ Help. Size M.

## G16 Common dialogs and controls
Rows CD-1, CD-2, CD-4…CD-10 — one toolkit Save As/Open with type filter, default extension, overwrite confirmation and New Folder (replacing per-app copies); Font, Color, Browse for Folder, Find and Replace dialogs; combo box, trackbar, up-down and tooltip widgets; MessageBeep; comdlg32 file dialog crash fixed and run in the guest. Size L.
