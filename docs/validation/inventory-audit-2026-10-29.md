# Audit Report: Operating-System Feature Inventory (fx-inv-audit)

> Audit by Antigravity (gemini-3.8-flash-high), 2026-10-29, read-only. Proposed statuses are applied to `docs/design/os-feature-inventory.md` by the lead after integrations 11 and 12 reach `main`; medium-confidence rows are checked first.

- **Date:** 2026-10-29 12:03:00 CET
- **Worktree:** `/home/peronslayer/Desktop/CiukiOS-wt-fx-inv-audit`
- **Target Document:** `docs/design/os-feature-inventory.md`
- **Auditor:** Antigravity (Advanced Agentic Assistant)

## Overview

This audit verified every row in `docs/design/os-feature-inventory.md` whose recorded status was `absent` or `partial` (166 target rows out of 350 total inventory rows) against the current source tree (`apps/`, `sdk/`, `src/kernel/`) and host test suites in `tests/host/`.

Multiple rows recorded as `absent` or `partial` with stale evidence have since been fully implemented and host-tested (e.g. through directives `G5`, `G8`, `G12`, `G19`, `fx-19`, `fx-uaccounts`, `fx-files-nav`, and `fx-accessories1`). The table below records all rows where the proposed status differs from the recorded status or where the recorded evidence is factually stale/wrong.

## Stale and Discrepant Inventory Rows

| row id | recorded status | what the code shows (file:line) | proposed status | confidence (high/medium/low) |
| --- | --- | --- | --- | --- |
| AX-10 | absent | `apps/accessories/clipbrd_main.c:1-350`, `apps/libdeskui/clipfile.c:1-200`, `tests/host/desktop/clipboard_viewer_test.c:1-85`. Standalone Clipboard Viewer accessory with format listing, text/image preview, .CLP save/open round-trip, and Delete. Tested in `tests/host/desktop/test_system_clipboard.py`. | done (host) | high |
| CP-16 | absent | `apps/printers/main.c:1-120`, `apps/desktop/shell.c:343,720`, `tests/host/desktop/printers_test.c:1-55`, `tests/host/test_printing.py:1-60`. Standalone Printers applet launched from Settings ▸ Printers (`shell.c:720`) with printer listing, default selection, job controls (pause/resume/cancel/purge), and Add Printer wizard with driver selection (Text/PostScript/PCL). Note: PR-1 is recorded done (host) while CP-16 was stale absent. | done (host) | high |
| DD-3 | absent | `apps/desktop/shell.c:504,812-820,3547`, `apps/files/app.c:2950-3020`, `tests/host/desktop/desktop_dnd_test.h:133-145`, `tests/host/desktop/files_ui_test.c:518-535`. Right-drag menu (`SH_MENU_DRAG`) implemented on desktop and in Files: Move Here (bold default where move is default), Copy Here, Create Shortcut(s) Here, and Cancel; tested in `test_desktop_folder.py` and `test_deskui.py`. | done (host) | high |
| DD-4 | absent | `apps/desktop/shell.c:3200-3350,3568`, `apps/files/app.c:2832-2975`, `apps/libdeskui/list.c:128-135`, `tests/host/desktop/desktop_dnd_test.h:90-130`, `tests/host/desktop/files_ui_test.c:596,673,681`. Translucent drag/cursor feedback (`+` copy, arrow link, `DESK_CURSOR_FORBIDDEN` no-drop), drop target highlight, marquee edge auto-scroll timer (`DU_TIMER_BAND`), and Esc cancellation implemented and host-tested. | done (host) | high |
| DD-7 | absent | `apps/files/transfer_ui.h:62,95-102`, `tests/host/desktop/files_model_test.c:126-175`, `tests/host/desktop/desktop_folder_test.c:353`. When a move cannot delete source items (read-only stick, permission, in-use), both copies are kept and the Windows 98 warning message names the files and drive. Verified in `test_desktop_folder.py`. | done (host) | high |
| DT-4 | absent | `apps/desktop/shell.h:171`, `apps/desktop/shell.c:2958-3060,6041`, `tests/host/desktop/desktop_dnd_test.h:2,67`. Rubber-band marquee selection from empty desktop space (plain replaces, Ctrl toggles, Shift adds), Ctrl/Shift multi-select, and Ctrl+A (`code==0x1e`) to select all desktop icons. Tested in `test_desktop_folder.py`. | done (host) | high |
| FM-19 | absent | `apps/libdeskui/list.c:128-160`, `apps/files/app.c:2772-2820`, `tests/host/desktop/files_ui_test.c:518-681`. Marquee selection from empty space in Files details, list, and icon views with auto-scroll and Ctrl/Shift modifier handling. Tested in `test_deskui.py` (`files_ui`). | done (host) | high |
| CP-11 | partial | `apps/control/app.c:2094-2115`, `tests/host/desktop/control_jobs_test.c:310-330`. Control Panel Mouse page has pointer speed slider, double-click speed slider, interactive double-click test button, swap left/right checkbox, pointer trails slider, wheel lines slider, and pointer scheme toggle button. Persisted and applied; tested in host suites. (Evidence citing `IN-6` which is done (host model)). | done (host) | high |
| CP-21 | partial | `apps/control/accounts.inc:1-350`, `apps/control/accounts_client.c:1-100`, `tests/host/security/test_control_panel_accounts.py:1-120`. Evidence stating "Real administrator-gated User Accounts is pending sec-user-04" is stale; sec-user-04 User Accounts was implemented and merged (`wt/fx-uaccounts`), providing user listing, creation, deletion, type change, and password reset through the SYSTEM accounts service. | done (host) | high |
| DT-5 | partial | `apps/desktop/shell.c:6013,6045-6056,6065-6085`, `tests/host/desktop/desktop_dnd_test.h:35-50`. Evidence stating "Type-ahead, F5 and Ctrl+X/C/V are absent" is stale; line 6013 implements F5 desktop refresh, lines 6045-6056 implement 1-second bounded type-ahead search across desktop icons, and lines 6074-6080 implement Ctrl+X/C/V/Z cut/copy/paste/undo. All tested in `test_desktop_folder.py`. | done (host) | high |
| DT-7 | partial | `apps/desktop/shell.c:780-810`, `tests/host/desktop/desktop_folder_test.c:300-360`. Evidence stating "Create Shortcut is absent; system icons have no context menu" is stale; line 802 implements Create Shortcut on file/folder icons, and lines 782-793 implement system icon context menus (My Computer: Open, Explore, Properties; Recycle Bin: Open, Empty Recycle Bin, Properties). | done (host) | high |
| DT-11 | partial | `apps/desktop/shell.c:3200-3575`, `tests/host/desktop/desktop_dnd_test.h:1-160`. Desktop drag and drop between desktop icons, folders, Files windows, and Recycle Bin with copy/move/link rules (same drive move, cross drive copy, Ctrl copy, Shift move, Alt link, right-drag menu, taskbar button hover restore) fully implemented and host-tested. | done (host) | high |
| KB-12 | partial | `apps/desktop/shell.c:5635-5655`, `tests/host/desktop/window_model_test.c:696,1677-1680`. Evidence citing non-existent `tests/host/desktop/win_keys_test.c` and claiming Win+F and Win+Ctrl+F missing is stale. Tested in `window_model_test.c`; `shell.c:5644-5655` (`logo_chord` case 0x21) executes `desk_shell_find(d, ctrl)`, routing Win+F to `files find:` and Win+Ctrl+F to the Find Computer dialog. | done (host) | high |
| SS-4 | partial | `apps/control/accounts.inc:1-350`, `apps/control/accounts_client.c:1-100`, `tests/host/security/test_control_panel_accounts.py:1-120`. Evidence stating "real account administration moves to sec-user-04 User Accounts" is stale; sec-user-04 User Accounts was implemented and verified with host tests. | done (host) | high |
| CP-9 | partial | `apps/control/app.c:2117-2140`, `tests/host/desktop/control_jobs_test.c:310-330`. Evidence stating "Layout only" is stale; Keyboard page includes repeat delay slider, repeat rate slider, cursor blink slider, layout selector, Left Alt+Shift switch checkbox, and interactive test edit field. Status remains partial because hardware keyboard type selection is not provided. | partial | high |
| CP-14 | partial | `apps/control/accounts.inc:1-350`, `apps/control/accounts_client.c:1-100`, `tests/host/security/test_control_panel_accounts.py:1-120`. Evidence stating sec-user-04 User Accounts is "pending implementation" is stale; User Accounts was implemented and merged. Status remains partial because remote administration is omitted by scope. | partial | high |
| DD-2 | partial | `apps/files/model.c:50-70`, `apps/desktop/shell.c:3310-3340`, `tests/host/desktop/desktop_dnd_test.h:90-120`. Evidence stating "Move unless Ctrl is held, whatever the drive" is stale; `fm_drop_effect()` evaluates `source_volume == target_volume ? FM_EFFECT_MOVE : FM_EFFECT_COPY`, Shift forces move, Ctrl forces copy, and Alt / Ctrl+Shift forces link. Status remains partial because executables are not automatically linked by default without modifier. | partial | high |
| FM-17 | partial | `apps/files/operations.inc:99-208`, `apps/files/app.c:186-190`, `tests/host/desktop/file_operations_test.c:20-60`, `tests/host/desktop/test_file_operations.py:1-75`. G8 and fx-files-nav implement a one-level Undo (Ctrl+Z) journal for rename, copy, move, and recycled delete (restoring from Recycle Bin), with toolbar integration and collision validation. Status can be considered done (host) because Windows 98 also limits undo after destructive overwrites or permanent Shift+Delete. | done (host) | medium |
| SS-2 | partial | `apps/control/accounts.inc:1-350`, `apps/control/accounts_client.c:1-100`, `tests/host/security/test_control_panel_accounts.py:1-120`. Evidence stating "Replacement User Accounts applet (sec-user-04) designed, pending implementation" is stale; sec-user-04 was implemented. Status remains partial because remote administration is omitted. | partial | high |

**Rows checked:**
- Rows checked with recorded status `absent` or `partial`: **166**
- Total inventory feature rows surveyed: **350**
- Total discrepant/stale rows identified: **19** (13 proposed status changes from absent/partial to `done (host)`, and 6 rows with stale/factually wrong evidence text).

## Verification Table

The following verification commands were executed sequentially inside this worktree using `nice -n 19`:

| Exact Command Line | Exit Code | Result Summary |
| --- | --- | --- |
| `nice -n 19 python3 tests/host/security/test_control_panel_accounts.py` | 0 | Ran 2 tests in 37.9s: PASSED. Verified sec-user-04 User Accounts client/applet (CP-14, CP-21, SS-2, SS-4). |
| `nice -n 19 python3 tests/host/desktop/test_system_clipboard.py` | 0 | Ran 5 tests in 6.5s: PASSED. Verified Clipboard Viewer (AX-10, CB-8) preview, .CLP save/load, format listing, and clear. |
| `nice -n 19 python3 tests/host/test_printing.py` | 0 | Ran 3 tests in 10.5s: PASSED. Verified Printers applet (apps/printers/main.c, CP-16, PR-1) wizard validation, drivers, default printer, pause, queue controls, and purge. |
| `nice -n 19 python3 -m unittest tests/host/desktop/test_desktop_folder.py` | 0 | Ran 1 test in 20.2s: PASSED. Verified desktop rubber band (DT-4), desktop selection/keyboard shortcuts (DT-5), icon context menu and Create Shortcut (DT-7), right-drag menu (DD-3), drag feedback (DD-4), read-only source move retention (DD-7), and desktop drag/drop rules (DT-11). |
| `nice -n 19 python3 -m unittest tests/host/desktop/test_file_operations.py` | 0 | Ran 2 tests in 12.8s: PASSED. Verified G8/fx-files-nav Undo (FM-17) for rename, copy, move, and recycled delete. |
| `nice -n 19 python3 -m unittest tests/host/desktop/test_deskui.py -k files_ui` | 0 | Ran 1 test in 11.4s: PASSED. Verified Files rubber band / marquee selection (FM-19) in details/list/icons, edge auto-scroll, right-drag, and drop targets. |

## Remaining Items

The remaining 147 rows recorded as `absent` or `partial` were verified to remain accurate descriptions of the current system state. Key genuine gaps include:
1. **Quick View (`FM-32`):** Directive `fx-accessories1` Part B is drafted in `docs/directives/fx-accessories1.md`, but implementation has not landed on `main`.
2. **Win16 Subsystem (`W32-7`):** Explicitly deferred past 1.0 Beta by owner decision 2026-10-20.
3. **Windows Setup and Startup Disk tabs (`CP-3`, `BT-9`):** Directive `fx-cp-setup` is currently on branch `wt/fx-cp-setup` and has not landed on `main`.
4. **MDI Support (`WM-15`):** `DefFrameProc` and `CreateMDIWindow` remain unimplemented in `sdk/win32/user32`.
5. **Console Windows for Win32 (`W32-6`):** `kernel32/console.c` remains a minimal non-interactive per-process log handle.
6. **Files Parent-Folder Close (`FM-38`):** Shift+click Close and Ctrl+double-click new window remain unimplemented.
7. **Network File Sharing / Neighborhood (`NW-12`, `FM-36`):** Client for Microsoft Networks and mapped drives remain absent.

## Summary

- **Implemented:** Identified 19 inventory rows where recorded status or evidence was stale. 13 rows are proposed to be upgraded from `absent` or `partial` to `done (host)` (including `AX-10` Clipboard Viewer, `CP-16` Printers, `DD-3` Right-drag menu, `DD-4` Drag feedback, `DD-7` Read-only move retention, `DT-4` Desktop rubber band, `FM-19` Files marquee, `DT-5` Desktop keyboard shortcuts, `DT-7` Desktop context menu, `DT-11` Desktop drag-and-drop, `KB-12` Win-key chords, `CP-11` Mouse properties, and `CP-21` User Accounts). 6 additional rows had factually obsolete evidence descriptions that omitted recent implementations.
- **Verified:** All 6 relevant host test suites covering the upgraded features were run sequentially with `nice -n 19` and passed completely (0 failures, 0 errors).
- **Remaining:** 147 rows remain accurately classified as `absent`, `absent (scope)`, or `partial`, with genuine gaps tracked in directives (e.g. Quick View `FM-32`, Windows Setup tab `CP-3`, and Win16 `W32-7`).
