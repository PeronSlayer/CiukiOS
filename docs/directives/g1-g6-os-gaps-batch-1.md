# OS feature gaps, batch 1 (G1–G6 of the fx-20 inventory)

- **Written:** 2026-10-23 18:29 by the lead, from `docs/design/os-feature-inventory.md` ("Prioritised gap list", items G1–G6) on the owner's requirement that CiukiOS has the complete feature set of a real operating system. Each item's scope is the inventory rows it lists; the inventory's Windows 98 reference behaviour and sources are binding; any row not delivered is listed as a gap in the report.
- **Start:** after fx-14…fx-19 are merged (they own NetSurf limits, the window-drawing audit, DOOM, removable storage and transfer rules, APM power, drag and drop/selection). Implementers: Claude subagents, one worktree each from `main` at launch.
- **Rules:** research and write the expected behaviour in the relevant design document first; host tests for every behaviour; no QEMU, no systemd-run, no image builds, no git mutations, no dev_diary edits; `date` before timestamps; English; report in `build/codex/<id>-report.md`. ABI changes with `abi_layout` oracle rows (one field per line) and a regenerated API reference.

## G1 Local time, time zone, daylight saving, setting the date and time
Inventory rows TM-1, TM-2, TB-6, AX-15, FM-2. Programs run with `TZ=UTC0` and the RTC is read as UTC while machines set up for Windows keep local time (fx-11 open item). Windows model: RTC in local time, a time-zone setting with automatic DST, Date/Time Properties (calendar, clock, time zone) from the tray clock and Control Panel, setting the clock writes the RTC. Implementer: Opus, high.

## G2 Recycle Bin
Rows FM-13, FM-23, DT-1, DD-6, MM-2, DT-5, Win32 `SHFileOperation` FOF_ALLOWUNDO. Per-volume bin with original-path records, Delete moves to the bin (Shift+Delete deletes), Restore, Empty, properties (size limit, confirm), the desktop icon full/empty, drop onto the bin. Uses fx-17's transfer engine. Implementer: Sonnet, high.

## G3 Shutdown and end of session without data loss
Rows SD-1, SD-2, SD-5, DOS-5, WM-16. Windows model: WM_QUERYENDSESSION/WM_ENDSESSION — each program may veto or ask to save; "This program is not responding" with End Task after a timeout only when it does not answer; DOS windows warn before closing; no kill while a save prompt is open. Covers native, Win32 and DOS programs. Implementer: Opus, high.

## G4 Taskbar menu, window arrangement, Windows-key shortcuts
Rows WM-11, WM-12, WM-13, KB-5, KB-8, KB-9, KB-12, KB-13, SM-9. Taskbar context menu (Cascade, Tile Horizontally/Vertically, Minimize All Windows / Undo, Properties), Show Desktop, Win, Win+D/E/F/R/M/Shift+M/Break/Tab, Ctrl+Esc, Alt+Tab/Alt+Shift+Tab with the switcher, Alt+Esc, Alt+F4 on the desktop opens Shut Down. Implementer: Sonnet, high.

## G5 One system clipboard
Rows CB-1, CB-3…CB-8, KB-10. One desktop-owned clipboard shared by native, Win32 and DOS programs and Files (cut/copy/paste of files as CF_HDROP with move-on-paste for Cut), text (ANSI/Unicode with locale) and DIB, delayed rendering, viewer (Clipboard Viewer), Print Screen / Alt+Print Screen, DOS window Mark/Copy/Paste. Implementer: Opus, high.

## G6 Property sheets, file attributes, disk space
Rows FM-2, FM-7, FM-8, FM-22, FM-24, FM-28, FM-29, KB-6, DT-5. File/folder/drive Properties sheets (General: type, location, size, size on disk, contains, created/modified/accessed, attributes Read-only/Hidden/Archive/System; drive: used/free with the pie chart, label), Alt+Enter, the Attributes column; kernel/ABI fields for attributes, created/accessed times and volume free space. Implementer: Opus, high.
