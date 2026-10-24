# Common dialogs and controls (G16)

Written 2026-10-23 23:50 CEST for directive G16
(`docs/directives/g13-g16-os-gaps-batch-3.md`). Rows of
`os-feature-inventory.md` it works on: CD-1, CD-2, CD-4, CD-5, CD-6, CD-7,
CD-8, CD-9, CD-10. The toolkit is `apps/libdeskui` (`controls.c`, `commdlg.c`,
`dialog.c`, `widgets.c`); tests are `tests/host/desktop/common_dialogs_test.c`
(run by `tests/host/desktop/test_deskui.py`) and the user32 host fixture.

## Research (before the code)

Reference behaviour is Windows 98 with the Win32 documentation of the same
interfaces; where Microsoft documents a later Windows only, the later text is
used for what did not change since 98 and recorded as such.

- Open and Save As: Microsoft,
  [OPENFILENAME](https://learn.microsoft.com/en-us/windows/win32/api/commdlg/ns-commdlg-openfilenamea).
  Filters are pairs "display string", "pattern" in the order given, patterns
  separated by `;`, only `*` and the name characters; `nFilterIndex` selects
  the initial one and returns the chosen one. `lpstrDefExt`: appended when the
  user types no extension, only its first three characters are used.
  `OFN_OVERWRITEPROMPT` asks before an existing Save As name is accepted;
  `OFN_FILEMUSTEXIST` (Open only) shows a warning for an unknown name and
  implies `OFN_PATHMUSTEXIST`, which warns about an unknown path.
- Controls, Microsoft Learn: [trackbar controls](https://learn.microsoft.com/en-us/windows/win32/controls/trackbar-controls)
  (line size from the arrow keys, page size from PgUp/PgDn and channel
  clicks, Home/End, `WM_HSCROLL` codes, `TB_ENDTRACK`, a timer while the
  button is held in the channel), [up-down controls](https://learn.microsoft.com/en-us/windows/win32/controls/up-down-controls)
  (buddy window, `UDS_SETBUDDYINT`, `UDS_ALIGNRIGHT`, `UDS_ARROWKEYS`, `UDS_WRAP`,
  a maximum below the minimum makes "up" decrease, a held button accelerates
  by a `UDACCEL` table, `WM_LBUTTONUP` selects the buddy's text),
  [`UDACCEL`](https://learn.microsoft.com/en-us/windows/win32/api/commctrl/ns-commctrl-udaccel),
  [`TTM_SETDELAYTIME`](https://learn.microsoft.com/en-us/windows/win32/controls/ttm-setdelaytime)
  (initial, autopop and reshow delays are 500 ms, 5000 ms and 100 ms at the
  default 500 ms double-click time).
- Common dialogs: [CHOOSECOLOR](https://learn.microsoft.com/en-us/windows/win32/api/commdlg/ns-commdlg-choosecolora)
  (16 custom colours updated in place, `CC_FULLOPEN`, `CC_PREVENTFULLOPEN`,
  `CC_RGBINIT`), [CHOOSEFONT](https://learn.microsoft.com/en-us/windows/win32/api/commdlg/ns-commdlg-choosefonta)
  (face, style, size in points, strike-out, underline, colour, sample;
  `CF_EFFECTS`), [FINDREPLACE](https://learn.microsoft.com/en-us/windows/win32/api/commdlg/ns-commdlg-findreplacea)
  (`FR_DOWN`, `FR_WHOLEWORD`, `FR_MATCHCASE`, Find Next, Replace, Replace All,
  the hide/disable flags), [BROWSEINFO](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/ns-shlobj_core-browseinfoa)
  (title text above a tree, only file-system folders, a Make New Folder button
  unless `BIF_NONEWFOLDERBUTTON`).
- [MessageBeep](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-messagebeep):
  the sound is the user's system sound for the icon class (asterisk,
  exclamation, hand, question, default beep); the call queues the sound and
  returns; the user can turn the beep off.
- Colour arithmetic: Microsoft KB Q29240 (the HLSMAX 240 / RGBMAX 255 integer
  RGB↔HLS routines of the Windows common colour dialog; hue 0–239, saturation
  and luminance 0–240). The 48 basic colours are the table of the Windows
  common colour dialog as reproduced in Wine 10.0 `dlls/comdlg32/colordlg.c`
  (LGPL-2.1; only the data is used).
- The local Windows 98 Resource Kit has no per-control specification for these
  dialogs; its dialog behaviour (Look in, Up One Level, Create New Folder, List
  and Details, Files of type) is the one the Open/Save As box shows.

## Decisions

### Controls (`controls.c`, hooks in `widgets.c`)

- **Combo box** (`DU_COMBO`): drop-down list (`CBS_DROPDOWNLIST`) or
  `editable` (`CBS_DROPDOWN`). The list is a transient overlay painted last
  (above windows and dialogs), eight rows high with a scroll bar, dropping up
  when there is no room below. Opening: the arrow, F4, Alt+Down. Up/Down move
  the highlight, Enter or releasing over a row picks it, Esc, F4 or a click
  elsewhere closes without a change (the outside click is consumed). Closed:
  Up/Down/Home/End change the selection, a typed letter jumps to the next
  entry starting with it. `DU_EV_SELECT` reports a pick, `DU_EV_CHANGE` an
  edited text, `DU_EV_COMMIT` Enter in an editable box.
- **Trackbar** (`DU_TRACKBAR`): vertical when the box is taller than wide.
  Left/Up and Right/Down step by the line size (default 1), PgUp/PgDn and a
  click in the channel by the page size (default a fifth of the range, the
  value Wine uses), Home/End the ends; a held channel click repeats (400 ms,
  then every 100 ms) until the thumb reaches the pointer; dragging follows the
  pointer; the wheel moves three lines. `DU_EV_CHANGE` on every move,
  `DU_EV_COMMIT` when the user lets go (`TB_ENDTRACK`).
- **Up-down** (`DU_UPDOWN`): `du_updown_attach` is `UDS_SETBUDDYINT |
  UDS_ALIGNRIGHT | UDS_ARROWKEYS`. The value is written to the buddy edit, the
  buddy's text is read back (and clamped) before every step, Up/Down in the
  buddy step it, pressing an arrow focuses the buddy and releasing selects its
  text. A held arrow repeats after 400 ms and then every 100 ms with the
  `UDACCEL` defaults 1, 5 after 2 s, 20 after 5 s. `lo > hi` makes Up move
  toward `hi`. The two repeat delays are the toolkit's choice: the
  documentation does not give them.
- **Tooltips**: any widget has a `tip` text (`du_set_tip`). A stationary pointer
  shows it after 500 ms below the pointer (yellow `COLOR_INFOBK`, black text and
  border); it is hidden after 5 s and does not return until the pointer leaves
  the tool, on press, key, wheel, focus loss, and moving to another tool shows
  that tool's tip after 100 ms. It works in windows and in dialogs.
- **Single-level undo** of the single-line edit (`EM_UNDO`): Ctrl+Z and
  Alt+Backspace; typing, or a run of Backspace/Delete, is one step; cut and
  paste are steps; undoing again redoes. The toolkit keeps one undo record, for
  the control edited last (the text area keeps its own).

### Open and Save As (`dialog.c`)

One entry, `du_file_open(u, id, &du_file_options)`, replaces the per-program
copies; `du_file_dialog` is a thin wrapper (Save As asks before replacing).
Look in, Up, New Folder, List and Details views, the Places pane (F4-30), File
name, Files of type. A name containing `*` or `?` becomes the filter until the
folder or type changes; a folder name navigates; names are normalised
(`.`, `..`, repeated slashes). Existing names are matched without regard to case.
The messages (Windows 98 wording where known) appear in a panel over the dialog,
as a native modal box cannot be stacked: "`<name>` already exists. Do you want
to replace it?" (Yes/No, default No), "File not found. Please verify the
correct file name was given.", "The path does not exist.", "The file name is
not valid." The result is `DU_EV_DIALOG` with the full path as text and the
chosen type index in `key`. New Folder asks for a name (default "New Folder",
"New Folder (2)", …), creates it through `make_dir` (default `mkdir(2)`), lists
and selects it. Programs now using it: CiukNote (text/all types, `txt`, replace
prompt, file-must-exist; its own filter, extension and replace code is gone),
Paint (bitmap/PNG types, default extension follows the format, replace prompt),
Music (playlist save with `m3u`), Registry Editor (`reg`). The Open/Save As of
Viewer, Help, NetSurf and the Control Panel keep the plain wrapper.

### Font, Color, Browse for Folder, Find and Replace (`commdlg.c`)

- **Font**: face list (caller's families, default the three Liberation
  families that stand for Courier New, Arial and Times New Roman), style list
  (Regular, Italic, Bold, Bold Italic), size list and edit (1–1638 points,
  refused with a message line otherwise), strike-out, underline, colour list,
  sample (the UI face bold or regular, underline and strike-out lines, in the
  chosen colour; an application can paint its own sample with real faces). Typing in
  the edits selects the matching list row. Result text
  `family|style|points|underline|strikeout|RRGGBB` (`du_font_format/parse`).
- **Color**: 48 basic colours, 16 custom colours (the caller's array, updated
  in place), "Define Custom Colors >>" widening the box with the hue/saturation
  spectrum, the luminance bar, the preview, hue/sat/lum and red/green/blue edits
  with up-down arrows, "Add to Custom Colors" (cycling through the 16 slots),
  arrow keys in the swatch grids. Result `#RRGGBB`.
- **Browse for Folder**: a tree of folders only, lazily listed through the
  lister, from `root`, opened on `initial`; prompt text above; "Make New Folder"
  creates "New Folder" (numbered when taken) under the selection.
- **Find and Replace**: modal inside the window, so the callback runs while the
  dialog stays open for Find Next, Replace and Replace All; a status line shown by
  `du_find_status` replaces the "Cannot find" message box; Cancel/Close and Esc
  end it. `du_find_text` is the shared search (case, whole word, direction).

### Win32

- **MessageBeep** sends a new v3 request `DESK_V3_SOUND` (opcode 80, the next
  free one on `main` at the time of writing; payload one 32-bit event number)
  from any open window of the process; the desktop plays the system sound
  through its scheme (error for hand/stop/exclamation, question, information
  for asterisk, default and any other value), so mute, volume and DOS-window
  suppression apply. No reply is sent (like DAMAGE). With `SPI_SETBEEP` off
  nothing is sent; with no window yet the call succeeds silently.
- **comdlg32** (CD-8) is the reused Wine 10.0 DLL; see "Gaps".

## What the tests pin

`common_dialogs_test.c`: wildcard patterns; HLS conversion (primaries, greys,
round trip within the 1/240 quantisation); font/colour text round trips; find
search; combo (open, hot row, release, outside click, keyboard, type-ahead,
wheel, upward drop, editable); trackbar (keys, paging with repeat, drag, commit,
wheel, vertical); up-down (arrows, buddy both ways, repeat and acceleration,
clamp, wrap, inverted range); tooltips (delays, autopop, reshow, hide events);
edit undo (typing run, redo, Alt+Backspace, cut, paste, last control); every
file-dialog path above plus the legacy wrapper; Font, Color, Folder and Find
dialogs; each transient is compared with a full redraw (partial render equal to
full render). Frames reviewed as PNG and pinned in
`common-dialogs-goldens.json`. `user32_test.c` checks the `DESK_V3_SOUND`
messages of `MessageBeep` for every icon class and with the beep off;
`v3_test.c` parses the new opcode.

## Gaps (reported, not delivered)

- Open/Save As: multiple selection (`OFN_ALLOWMULTISELECT`), the Read Only check
  box, a Network button, large icon views, an Explorer-style tree of Look in;
  `OFN_CREATEPROMPT`.
- Font: script selection, printer fonts; the sample uses the toolkit's faces
  unless the application supplies a painter. The three lists use the tree
  control's rows (indented) because the toolkit has no plain list box.
- Browse for Folder: no in-place rename of a new folder, no edit box
  (`BIF_EDITBOX`), no network or special-folder roots.
- Find and Replace: not modeless; CiukNote keeps its find bar.
- Tooltips: no balloon style, no tracking tooltips, no tool rectangles inside
  one widget (the whole widget is the tool).
- CD-9: rich edit, wizard frameworks.
- CD-8: see below.

### CD-8, Wine comdlg32 `filedlg`

Recorded state (`config/wine-upper.json`, unit `comdlg32:filedlg`): `crashed
c0000005` with no failed check reported, and 18 refused kernel record
operations `0x109` (`W32_FILE`, `src/kernel/include/ciuki/w32_core.h`) that the
H1 adapter ("no CiukiOS kernel") fails with ENOSYS; `finddlg` runs (43 checks, 18
failures) and `printdlg` passes. Research on the upstream test (Wine 10.0
`dlls/comdlg32/tests/filedlg.c`, read at
`raw.githubusercontent.com/wine-mirror/wine/wine-10.0`): every test but one
calls `GetOpenFileNameA` or `GetSaveFileNameA`, i.e. the Explorer-style box built
on shell32's `IShellBrowser`/`IShellView2` and a `FindFirstFileW` listing (the
`W32_FILE` traffic); `test_arrange` and `test_resizable2` pass the result of
`FindResourceA` to `LoadResource` without a check (`test_arrange` then reads the
template), so a missing `template_stc32` resource also ends in an access
violation. A crash before the first `ok()` points at one of these two places, but
nothing here can tell which: the upper build (`scripts/win32/wine_upper.py
conformance-build`) needs the Wine 10.0 source tree, which this workspace does not
have, and a guest run needs QEMU, which G16 may not use. CD-8 therefore stays
**open**; the next step is to build the unit, print the fault address from the
adapter and either supply the `W32_FILE` list/open operations to the adapter or
fix the resource staging, then run the same unit in the guest.
