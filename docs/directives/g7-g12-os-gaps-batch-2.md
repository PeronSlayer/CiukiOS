# OS feature gaps, batch 2 (G7–G12 of the fx-20 inventory)

- **Written:** 2026-10-23 22:59 by the lead, on the owner's instruction to keep developing for several days until the partial and missing areas of `docs/design/os-feature-inventory.md` are complete. Each item's scope is the inventory rows and the work-item text of its G row ("Prioritised gap list"); the inventory's Windows 98 reference behaviour and sources are binding; any row not delivered is listed as a gap in the report, and the item updates the inventory rows it completes.
- **Implementers:** Codex gpt-6.1-sol (owner: Codex is available again), one worktree each from `main` at launch; Claude subagents when Codex capacity is full.
- **Rules:** research and write the expected behaviour in the relevant design document first; host tests for every behaviour, reproducing the Windows 98 behaviour; no QEMU, no systemd-run, no image builds, no git mutations, no dev_diary edits; `date` before timestamps; English; report in `build/codex/<id>-report.md`. ABI/protocol changes: `abi_layout` oracle rows (one field per line), the next free syscall/message numbers after the highest on `main` (state them in the report), a regenerated API reference. Before reporting, compile every touched app/SDK C file with `/home/peronslayer/Desktop/CiukiOS/build/tools/ciuki-sdk/bin/ciuki-cc -O2 -Wall -Wextra -Werror -I sdk/sysroot-overlay/include -I src/kernel/include -I apps/desktop -I apps/libdeskui -I apps/files -I sdk/win32/include -c <file> -o build/i686check/<name>.o` (4 KiB frame limit, CiukiOS errno set only, `#ifdef`-guard others) and every touched kernel file with the `scripts/build_kernel.py` flags. Every new screen-changing event gets a row in the fx-15 window-model table and `test_window_model`.

## G7 Find Files or Folders
Rows FM-30, SM-6, KB-12 (Win+F), FM-10 — search by name with wildcards, by contained text, look-in with subfolders, date, size and type; results list with the usual file actions; F3 in Files and on the desktop; Start ▸ Find; replace `desk_shell_find` (G4's placeholder). Uses G6's attributes/times. Size M.

## G8 File operations completeness
Rows FM-11, FM-15, FM-16, FM-17, FM-25, FM-26, FM-27, FM-31, DT-6, DT-7, SM-8 — Undo for rename/move/copy/delete (G2's bin for delete); Copy To / Move To; Send To with a user SendTo folder; a CiukiOS shortcut file type with Create Shortcut / Paste Shortcut / New ▸ Shortcut (and reading Win32 `.lnk` targets where possible), New ▸ Bitmap; registry-backed file types shared with Win32 (`HKCR`), Open With with "always use"; launching `.exe`, `.com`, `.bat`, `.pif` from Files and the desktop through one shared launcher (also used by G9). fx-19 left "Create Shortcut(s) Here" disabled until shortcuts exist: enable it. Size L.

## G9 Run dialog
Rows SM-8, KB-12 (Win+R) — history drop-down, Browse, absolute paths, documents via G8 associations, folders, URLs (NetSurf), Win32 and DOS programs. Starts after G8 is merged (uses its launcher). Size S.

## G10 Screen saver and monitor power
Rows DS-3, DS-4, SS-3 — idle timer fed by input, SC_SCREENSAVE and SPI_GET/SETSCREENSAVE*, savers Blank, Ciuki logo (the approved portrait, deterministic conversion only), starfield, with preview and wait time; VBE/DPMS monitor off from the Power page; DOS full-screen and full-screen games suppress the saver. Size M.

## G11 Power policy
Rows PW-3, PW-4, PW-5, PW-6, PW-8, PW-9, SD-1, SD-7, TB-8 — power schemes (monitor off, disks off, standby after; AC and battery), low/critical battery alarms with action, tray tooltip with percent and time, power-button and lid choices, WM_POWERBROADCAST, Stand by where APM 1.2 or ACPI S1/S3 is qualified with device resume. Builds on fx-18/fx-25 (APM on the T23). Size L.

## G12 Keyboard, language and mouse settings
Rows IN-2…IN-6, TB-11, WM-21 — repeat delay/rate (PS/2 0xF3 and USB repeat), cursor blink, PS/2 LEDs (0xED), Italian dead keys, Left Alt+Shift layout switch with tray indicator, button swap, pointer trails, wheel lines, pointer schemes (large, inverted — derived from the fx-13 Ciuki set), Win32 SPI values persisted. Size M.
