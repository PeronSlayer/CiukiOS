# Directive f2-32: CiukNote text editor

- **Step:** F2+ (desktop applications). **Contracts:** as f2-31
  (`libdeskui`, protocol v2, POSIX subset files); parity reference: the
  0.8 CiukNote description in the legacy archive
  (`docs/desktop-apps-2026-09-29.md`).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `high`.
- **Worktree:** `wt/f2-ciuknote`, from `main` after f2-31 is merged.
  Files: new `apps/ciuknote/` on `libdeskui` (a multi-line text widget is
  added to the toolkit here if f2-31 did not deliver one), `apps/build_apps.py`
  and `scripts/build_image.py` (`/bin/ciuknote`), icon from the Tango
  archive, host tests, one QEMU case `ciuknote-qemu-t23`, `docs/desktop.md`.

## Scope

File: New, Open, Save, Save As (file dialog, `.txt` filter, replace
prompt), Exit, with the "The text in the <name> file has changed. Do you
want to save the changes?" prompt. Edit: Undo (one level), Cut, Copy,
Paste, Delete, Find, Find Next (F3), Replace (Find Next/Replace/Replace
All/Match case), Go To, Select All, Time/Date (F5). Format: Word Wrap,
Font (Regular/Bold of the single bitmap font). View: Status Bar (Ln, Col).
Help: About. Right-click menu, F10/Alt menus, double-click selects a word,
a first line `.LOG` appends time and date on opening, text limit 61,440
bytes with a message beyond it. Command-line argument: a path to open
(from Files). Printing to LPT1 is explicitly later (no parallel port
driver yet). UTF-8 input limited to the font's glyphs; others shown as a
placeholder glyph.

Tests: host tests of the editor model (insert/delete/undo/find/replace/
word wrap/line-column) and rendering digests; QEMU `ciuknote-qemu-t23`:
open from the menu, type text, Save As to `/system/tests/note.txt`,
reopen, verify content through the probe's file digest and
`desktop_screen` digests.

## Acceptance by the lead

Host tests; app/image build; `ciuknote-qemu-t23` PASS; screenshot for the
owner; `f2-all` unaffected.
