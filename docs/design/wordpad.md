# WordPad-class editor (G28)

- **Written:** 2026-10-24 for directive `g17-g32-os-gaps-batches-4-5.md`, item G28 (inventory rows AX-1 "offers
  WordPad for files too large" and AX-2). Source state: branch `wt/g28` from `a0e3ddb9`.
- **Result:** `/Programs/wordpad` (`apps/wordpad/`), an RTF and text editor on libdeskui with fonts, character and
  paragraph formatting, bullets, tab stops, a ruler, Find and Replace, Page Setup and print preview. CiukNote offers it
  for files above its 61,440-byte limit.

## Reference behaviour and sources

| Topic | Source | What it fixes here |
| --- | --- | --- |
| WordPad is a shipped component | Windows 98 Resource Kit, App. D `[OptionalComponents]`, `"WordPad"=1` (`build/rskt/wrkappd_3.htm`, key RK-comp of the inventory); the same book names WordPad as an editor for HOSTS-style text files (`wrkappf.htm`) | The program belongs to the accessories; it edits text files too |
| Rich Text Format | Microsoft, *Rich Text Format (RTF) Specification* 1.6 (introduction: <https://learn.microsoft.com/en-us/previous-versions/office/developer/office2000/aa140280(v=office.10)>) and 1.9.1, March 2008, the last version (Library of Congress description: <https://www.loc.gov/preservation/digital/formats/fdd/fdd000473.shtml>) | Control words, units (half-points, twips), destinations, `\*` ignorable destinations, `\'hh`, `\uN` with `\ucN`, bullets by `\pntext`/`\pn\pnlvlblt` and `\listtext` |
| Write `.wri` | PRONOM x-fmt/4 (<https://pronom.nationalarchives.gov.uk/x-fmt/4>): `wIdent` 0xBE31/0xBE32, `wTool` 0xAB00, `pnMac` at offset 96 | Only the signature is used, to refuse the file with an explanation; the record layout was not found in a primary source, so no importer was written (gap) |
| Word 6/97 `.doc` | OLE2 compound file signature `D0 CF 11 E0 A1 B1 1A E1` | Refused with an explanation (gap) |
| Windows 98 WordPad behaviour | Win98 user behaviour: one-level undo, text-only save warning ("will remove all formatting"), bullets via Format menu, Page Setup, print preview with Next/Prev/Zoom/Close | The same commands and warnings |

The Windows 98 default margins of WordPad could not be confirmed from a primary source; 0.75 inch on all sides is used
(decision below).

## Document model (`rich.h`, `rich.c`)

- UTF-8 text with `\n` between paragraphs and U+000B for RTF `\line`. Capacity 8 MiB of text (`WP_LIMIT`);
  files up to 16 MiB are read (`WP_FILE_MAX`). CiukNote keeps its 61,440-byte limit.
- One run table (`font`, `size` in half-points, `color`, flags bold/italic/underline/strike) covers the text; one
  paragraph record per paragraph (alignment, left/first-line/right indent in twips, bullet, up to 8 tab stops).
  Adjacent equal runs always merge. Every mutator is atomic: it reserves all memory first, so `-ENOMEM`/`-EFBIG`
  leave the document unchanged.
- Fonts: the three installed families (Arial, Times New Roman, Courier New, drawn with Liberation Sans, Serif, Mono, which
  are metric-compatible) plus the names a document brings, mapped to a family (`wp_family_for_name`, RTF `\froman`,
  `\fmodern`, `\fswiss`). The names survive a load and save.
- Joining paragraphs by deleting a mark keeps the attributes of the mark that survives (the following paragraph's), as
  Word and WordPad do; splitting copies the attributes.
- One undo level with redo (a snapshot before each command; typing, backspacing and deleting runs share one), as WordPad.

## RTF (`rtf.c`)

Reader: bounded group depth (64), bounded control word length, parameters clamped, `\bin` skipped by length,
unknown control words ignored, unknown `\*` destinations skipped; font and colour tables mapped into the document tables
(a full table falls back to the family's built-in face or the nearest colour); `\u` surrogate pairs combined, lone halves
become U+FFFD; control characters never reach the text; the final `\par` of a document does not add an empty paragraph
(the writer always ends the last paragraph with `\par`, so a save and load round trip is exact). Pictures (`\pict`) and
objects (`\object`, shapes) are skipped and counted; the application tells the user they will not survive a save.
Mutation fuzzing (4,000 cases, ASAN and UBSAN) is part of the host test.

Writer: `{\rtf1\ansi\ansicpg1252\deff0` with the font and colour tables, `\pard` per paragraph, character changes as
deltas, Latin-1/Windows-1252 as `\'hh`, everything else as `\uN?`, WordPad's bullet form `\pntext`/`\pn\pnlvlblt`. Lines
stay below 200 columns.

## Layout (`layout.h`, `layout.c`)

96 dpi: one pixel is 15 twips, a half-point is 2/3 pixel; x is tracked in 1/64 pixel so measured words add without drift.
The host supplies measuring and drawing (`struct wp_text_ops`), so the same code lays out the screen, the print
preview and a future printer page. The text is split into atoms (words, space runs, tabs, soft breaks, each inside one
run); words wrap, a word wider than the line breaks between characters; tabs go to custom stops, then to every half
inch; paragraphs have left, hanging first-line and right indents; centre and right alignment; bullets hang at
`left + first`. Drawing, the caret and hit testing re-walk the same atoms of a line, so all agree with the breaks
(`wp_layout_hit` of `wp_layout_caret` returns the same place for every position and width: host test, with fixed
advances and with the real faces). Layout is incremental from the first changed paragraph. Pagination
(`wp_paginate`) breaks the laid-out lines at the printable height of a `wp_page_setup` (Letter, Legal, A4, A5, portrait or
landscape, margins) and is what G31 will print from.

## Application (`app.c`, `main.c`, `fonts.c`)

- Menus File (New, Open, Save, Save As, Print (disabled until G31), Print Preview, Page Setup, Exit), Edit (Undo, Redo,
  Cut, Copy, Paste, Clear, Select All, Find, Find Next, Replace), View (Toolbar, Format Bar, Ruler, Status Bar), Insert
  (Date and Time), Format (Font, Bullet Style, Paragraph, Tabs), Help; toolbar, format bar (font, size, colour, bold,
  italic, underline, left, centre, right, bullets with pressed state), inch ruler with the paragraph's indent markers,
  context menu, find and replace bar, status bar.
- Keys: Ctrl+B/I/U, Ctrl+L/E/R, the usual movement keys with Shift, Ctrl+arrows by word, Ctrl+Home/End, PgUp/PgDn.
  Enter on an empty bullet and Backspace at the start of a bullet paragraph end the list.
- Mouse: click, shift-click, double click (a word with its trailing spaces), triple click (a paragraph), drag with
  auto-scroll, wheel (the toolkit gained an opt-in `DU_POINTER_WHEEL` for canvases, `du_widget.wheel_events`).
- Dialogs (property-sheet pages on the toolkit): Font (family, style, size, colour, strikeout, underline, sample),
  Paragraph, Tabs, Page Setup, Date and Time. G16 will add the common Font dialog; this one is the stop-gap.
- Files: `.rtf` read and written, `.txt` read (UTF-8 or Windows-1252, CRLF/CR/LF) and written (UTF-8, the file's own line
  endings, with the "Text-Only format will remove all formatting" question when formatting would be lost); default
  extension `.rtf`; overwrite question; file dialog filter `*.rtf;*.txt` (a typed pattern replaces it); documents named on
  the command line are read by a worker thread; dropped files open after the unsaved-changes question; G3 session
  end (close request, veto).
- Print Preview replaces the document view with the pages at 25–70 % (fitting the window), Previous, Next Page, Zoom
  Out, Zoom In, Close; Page Setup edits paper, orientation and margins in inches.
- Fonts: `fonts.c` measures and draws with libfont on `/System/fonts/Liberation*.ttf` (long texts in 255-byte chunks, the
  advance of the 26.6 measure used for the next chunk); with no face it falls back to the toolkit face.

## CiukNote hand-off

A file above 61,440 bytes (or whose decoded text would exceed it) now raises "`<name>` is too large for CiukNote (limit
61,440 bytes). Do you want to open it in WordPad?" (dialog `D_BIG`, Yes default). Yes sends the protocol v2 `LAUNCH` of
`wordpad` with the absolute path, as Files does; the desktop honours it for CiukNote (`client.c`: `note_wordpad`, an
absolute path and the program `wordpad` only) and CiukNote reports a refused launch. Scripted runs (`--test`) have no
launcher and keep the former message.

## Decisions needed from the lead

1. **Default margins and paper.** 0.75 inch on all sides (the Notepad value; the WordPad default could not be verified)
   and A4 unless `LANG` starts with `en_US` (G24 Regional Settings should choose the paper).
2. **Unsupported formats.** `.wri`/`.doc` import is not implemented (no primary source for the Write record layout was
   reachable; Word 6/97 is a compound file). The row stays open as a gap.
3. **Frame pins.** The frames pinned in `test_wordpad.py` use fixed-advance fake fonts; three were reviewed as screenshots, the rest are pinned from the first run;
   they are not guest evidence.

## Gaps left

Insert Object (OLE), embedded pictures (skipped and reported), Word and Write import, printing (needs G31: the menu item
and the preview's Print are disabled), "wrap to ruler" and "no wrap", units other than inches, drag-to-move indent markers
and tab stops on the ruler, justified text, right/centre/decimal tab alignments, multi-level undo, rich clipboard
formats (G5; a copy keeps its formatting only inside WordPad), Unicode-text save, the common Font dialog (G16), locale
date formats (G24), keyboard layouts beyond the toolkit's US text (G12), Help topics (G15: Help opens a static box).
