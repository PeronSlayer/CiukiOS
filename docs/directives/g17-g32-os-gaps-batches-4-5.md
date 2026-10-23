# OS feature gaps, batches 4–5 (G17–G32 of the fx-20 inventory)

- **Written:** 2026-10-23 23:10 by the lead (owner: continuous development; use as many agents as the host allows to finish every feature sooner). Scope, rules and compile/ABI requirements are those of `g7-g12-os-gaps-batch-2.md` (header), binding here too; each item's scope is the inventory rows and work-item text of its G row in `docs/design/os-feature-inventory.md` ("Prioritised gap list"), which is the binding specification of the item. Every item updates the inventory rows it completes and lists the rest as gaps.
- **Implementers:** Codex gpt-6.1-sol (`launch-codex.sh`) and Claude Code CLI on the second account (`launch-claude.sh`), one worktree each from `main`; model and effort chosen per item by the lead (parsimonious: Sonnet medium/high for app and content work, Opus or Codex xhigh only for kernel/V86/storage).
- **Overlaps:** G19, G20 and G23 touch the desktop shell and theme also touched by G4 (taskbar and Windows keys); start them after G4 is merged, or keep edits minimal and list the overlap. G26 touches the network stack (fx-11), G29 the FAT repair code (G14 ScanDisk), G31 the LPT driver (f1-114).

| Item | Title |
| --- | --- |
| G17 | DOS window and program properties |
| G18 | Win32 shell integration |
| G19 | Desktop icon management beyond fx-19 |
| G20 | Taskbar and Start menu customisation |
| G21 | Startup programs |
| G22 | Multimedia completion |
| G23 | Display appearance and DPI |
| G24 | Regional Settings |
| G25 | Accessibility |
| G26 | Network user tools |
| G27 | Accessories completion |
| G28 | WordPad-class editor |
| G29 | Disk tools |
| G30 | System tools |
| G31 | Printing |
| G32 | Users, logon and passwords |
