# WordPad-class editor (G28)

- **Written:** 2026-10-24 for directive `g17-g32-os-gaps-batches-4-5.md`, item G28 (inventory rows AX-1 "offers
  WordPad for files too large" and AX-2). Source state: branch `wt/g28` from `a0e3ddb9`.
- **Second part (G28b):** written 2026-10-24 for `docs/directives/second-parts-2026-10-24.md`, item G28b, on branch
  `wt/g28b` from `wt/integ` (`d927480c`, which has G28, G31, G5, G24 and G15 part 1). It adds Word and Write import,
  pictures and objects, printing, the rich clipboard, the ruler with units and dragging, justified text, no-wrap and
  multi-level undo; see "G28b" below. Where the first-part text says that something is missing, the G28b section and
  "Gaps left" at the end have the current state.
- **Result:** `/Programs/wordpad` (`apps/wordpad/`), an RTF and text editor on libdeskui with fonts, character and
  paragraph formatting, bullets, tab stops, a ruler, Find and Replace, Page Setup and print preview. CiukNote offers it
  for files above its 61,440-byte limit.

## Reference behaviour and sources

| Topic | Source | What it fixes here |
| --- | --- | --- |
| WordPad is a shipped component | Windows 98 Resource Kit, App. D `[OptionalComponents]`, `"WordPad"=1` (`build/rskt/wrkappd_3.htm`, key RK-comp of the inventory); the same book names WordPad as an editor for HOSTS-style text files (`wrkappf.htm`) | The program belongs to the accessories; it edits text files too |
| Rich Text Format | Microsoft, *Rich Text Format (RTF) Specification* 1.6 (introduction: <https://learn.microsoft.com/en-us/previous-versions/office/developer/office2000/aa140280(v=office.10)>) and 1.9.1, March 2008, the last version (Library of Congress description: <https://www.loc.gov/preservation/digital/formats/fdd/fdd000473.shtml>) | Control words, units (half-points, twips), destinations, `\*` ignorable destinations, `\'hh`, `\uN` with `\ucN`, bullets by `\pntext`/`\pn\pnlvlblt` and `\listtext` |
| Write `.wri` | G28 used PRONOM x-fmt/4 (<https://pronom.nationalarchives.gov.uk/x-fmt/4>) for the signature only. G28b found the layout in the Windows 3.1 SDK topic ".WRI Write File Format" (header, FODs, CHP/PAP, FFN, picture and OLE headers), reproduced at <https://web.archive.org/web/2016/http://msxnet.org/word2rtf/formats/write.txt> | `apps/wordpad/write.c` (see G28b) |
| Word 6/95/97 `.doc` | G28b: Microsoft, *Word 6.0 Binary File Format* (<https://web.archive.org/web/2012/http://www.opennet.ru/docs/formats/wword60.txt>), *Microsoft Office Word 97-2007 Binary File Format (.doc) Specification* (<https://download.microsoft.com/download/0/b/e/0be8bdd7-e5e8-422a-abfd-4342ed7ad886/word97-2007binaryfileformat(doc)specification.pdf>), [MS-CFB] (<https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-cfb/>) | `apps/wordpad/ole.c`, `apps/wordpad/msword.c` (see G28b) |
| Windows 98 WordPad behaviour | Win98 user behaviour: undo (one level there, many levels by the G28b directive), text-only save warning ("will remove all formatting"), bullets via Format menu, Page Setup, print preview with Next/Prev/Zoom/Close | The same commands and warnings |

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
- Undo with redo: a snapshot before each command, typing, backspacing and deleting runs sharing one (one level in the first part, as Windows 98 WordPad; many levels since G28b, see G28b).

## RTF (`rtf.c`)

Reader: bounded group depth (64), bounded control word length, parameters clamped, `\bin` skipped by length,
unknown control words ignored, unknown `\*` destinations skipped; font and colour tables mapped into the document tables
(a full table falls back to the family's built-in face or the nearest colour); `\u` surrogate pairs combined, lone halves
become U+FFFD; control characters never reach the text; the final `\par` of a document does not add an empty paragraph
(the writer always ends the last paragraph with `\par`, so a save and load round trip is exact). Pictures (`\pict`) and
objects (`\object`) were skipped and counted in the first part; since G28b they are kept (see G28b); shapes are still skipped and counted.
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

- Menus File (New, Open, Save, Save As, Print (G31, in G28b), Print Preview, Page Setup, Exit), Edit (Undo, Redo,
  Cut, Copy, Paste, Clear, Select All, Find, Find Next, Replace), View (Toolbar, Format Bar, Ruler, Status Bar), Insert
  (Date and Time, Picture and Object since G28b), Format (Font, Bullet Style, Paragraph, Tabs), Help; toolbar, format bar (font, size, colour, bold,
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
- Print Preview (with a Print button since G28b) replaces the document view with the pages at 25–70 % (fitting the window), Previous, Next Page, Zoom
  Out, Zoom In, Close; Page Setup edits paper, orientation and margins in inches.
- Fonts: `fonts.c` measures and draws with libfont on `/System/fonts/Liberation*.ttf` (long texts in 255-byte chunks, the
  advance of the 26.6 measure used for the next chunk); with no face it falls back to the toolkit face.

## CiukNote hand-off

A file above 61,440 bytes (or whose decoded text would exceed it) now raises "`<name>` is too large for CiukNote (limit
61,440 bytes). Do you want to open it in WordPad?" (dialog `D_BIG`, Yes default). Yes sends the protocol v2 `LAUNCH` of
`wordpad` with the absolute path, as Files does; the desktop honours it for CiukNote (`client.c`: `note_wordpad`, an
absolute path and the program `wordpad` only) and CiukNote reports a refused launch. Scripted runs (`--test`) have no
launcher and keep the former message.

## G28b: Word and Write import, pictures, printing, clipboard, ruler

### Sources checked on 2026-10-24

- [MS-CFB], *Compound File Binary File Format*: header (signature, sector shifts 9 or 12, mini cutoff 4096, 109 DIFAT entries
  and the DIFAT chain), FAT, directory entries (128 bytes, name length, type, child/sibling links, start sector, size), mini
  FAT and the mini stream held by the root entry.
- *Word 6.0 Binary File Format*, FIB (`wIdent`, `nFib` 101 and 104, `fcMin`, `fcMac`, `ccpText` at 0x34, `fcStshf` 0x60,
  `fcPlcfbteChpx` 184, `fcPlcfbtePapx` 192, `fcSttbfffn` 208, `fcClx` 352), text as bytes from `fcMin`, FKP pages (CHPX FKP
  with 1-byte offsets, PAPX FKP with 7-byte BX), bin tables with 2-byte page numbers, STSH (STSHI, STD base of 8 bytes, UPX),
  the sprm table with 1-byte opcodes, PIC (58 bytes, `mfp.mm`, `dxaGoal`, `mx`), ANLD, PRM and the piece table of fast-saved files.
- *Word 97-2007 Binary File Format*, FIB (`csw` 14, `cslw` 22, `fcStshf` 0xA2, `fcPlcfbteChpx` 0xFA, `fcPlcfbtePapx` 0x102,
  `fcSttbfffn` 0x112, `fcClx` 0x1A2, `fcPlcfLst` 0x2E2, `fcPlfLfo` 0x2EA, `fWhichTblStm`, `fEncrypted`, `fComplex`), the Clx
  (grpprls then PlcPcd with 8-byte PCDs, `fCompressed` and `fc/2`), UTF-16 and compressed text, FKPs (13-byte BX, PapxInFkp),
  Stshif/STD (10-byte base, xstz names), the 2-byte sprm opcodes and the spra operand sizes (including `sprmPChgTabs` with
  cb 255 and `sprmTDefTable`), LSTF/LVLF/LFO, PICF (`cbHeader`, `mm` 0x64/0x66) and the OfficeArt BLIP records
  (FBSE 0xF007, JPEG 0xF01D, PNG 0xF01E, DIB 0xF01F, one or two UIDs by the odd `recInstance`).
- Windows 3.1 SDK ".WRI Write File Format": header words (`fcMac` at byte 14, `pnPara` 18, `pnFntb` 20, `pnFfntb` 28,
  `pnMac` at byte 96), text from byte 128, 128-byte FOD pages (4-byte `fcFirst`, 6-byte FODs, FPROPs, count at byte 127),
  CHP and PAP prefixes with their default values, TBD tab descriptors, FFN table, picture header (40 bytes: `mm` 0xE3
  bitmap or 0x88 metafile, `dxaSize`, `cbSize`, `mx`, BITMAP at byte 16), OLE object header (`mm` 0xE4) and the static DIB
  object layout.
- Microsoft, RTF Specification 1.6/1.9.1 for `\pict` (`\pngblip`, `\jpegblip`, `\emfblip`, `\wmetafile`, `\dibitmap`, `\picw`,
  `\pich`, `\picwgoal`, `\pichgoal`, `\picscalex`, hex or `\bin` data), `\object` (`\objemb`, `\objlink`, `\objautlink`, `\objclass`,
  `\objdata`, `\result`), `\shppict`/`\nonshppict` and `\qj`.
- Windows 98 Resource Kit, App. D `"WordPad"=1` (`build/rskt/wrkappd_3.htm`); the behaviour of WordPad 98 for Options (units
  and word wrap per document type), the ruler and Insert Object follows the Windows 98 user documentation and is described below.
- Local checks (not committed, never redistributed): the Windows 98 Resource Kit `README.DOC` (Word 6.0 for Windows, nFib 101,
  `wIdent` 0xA5DC) and the five Write 3.0 files of `third_party/windows31/installed/` were read with the importers and
  their fonts, sizes, styles, tabs and text matched; `tests/host/desktop/fixtures/wordpad-lo97.doc` is a Word 97 file
  written by LibreOffice from `wordpad-lo97.rtf` and is committed.

### Document model additions (`rich.c`, `rich.h`)

- A picture or object is one U+FFFC character; its run carries `wp_cattr.pic` (1-based id into `doc->pics`). Pictures are
  immutable, reference counted (atomic), shared by undo levels, clipboard slices and print jobs, and bounded:
  `WP_PICS_MAX` (2048) per document, `WP_PIC_BYTES` (96 MiB) of picture and object bytes; unreferenced pictures are dropped by
  compaction when a limit is reached. Runs with a picture never merge. `wp_attr_at/of` strip the picture so typed text never
  becomes one. Plain-text export and pasted text drop U+FFFC.
- `struct wp_pic` keeps the bytes exactly as read (kind PNG, JPEG, DIB, WMF, EMF or PICT; for an object also class,
  `\objdata` and its result picture), the display size (goal in twips and scale in percent) and a decoded-pixel cache that the
  application fills on first display. `wp_pic_sniff` reads PNG, JPEG and DIB sizes; `wp_dib_to_bmp`/`wp_bmp_to_dib` convert.
- `WP_JUSTIFY` is the fourth alignment (`\qj`).

### RTF (`rtf.c`)

Pictures are read (hex and `\bin`, size goals, scales, `\shppict` read and `\nonshppict` skipped) and written back
byte for byte; objects keep class, data and result picture and are written as `\object`. Unsupported pictures
(`\wbitmap`, no data) are counted in the report. `Symbol` in a font table is not a font of the document (the writer adds
it for bullets), which makes write-read-write stable.

### Word import (`ole.c`, `msword.c`)

`wp_word_read` opens the compound file, reads `WordDocument` (and `0Table`/`1Table`, `Data`), decodes the main text through the
piece table (compressed 8-bit text through Windows-1252, Mac Roman for `chs` 256, UTF-16 with surrogate pairs), builds
paragraphs from the PAPX FKPs, character runs from the CHPX FKPs and the grpprls of the piece table, resolves styles
(based-on chains, character styles, the toggles 128/129) and maps font, size, bold, italic, underline, strike, colour
(`ico` and `sprmCCv`), capitals, hidden text, alignment, left/right/first-line indents and tab stops. Word 97 lists give
bullets or numbers ("1.", "a)", roman) with the level's indents, Word 6 `sprmPAnld` gives bullets. Tables become tab-separated
text with a paragraph per row, fields show their result, special characters 0x01 insert the picture (Word 6 metafile from the
main stream; Word 97 PNG, JPEG or DIB BLIP from the Data stream). Footnotes, headers, annotations and text boxes are counted
(`ignored_parts`) and the user is told. Encrypted files and versions before Word 6 are refused with a reason. Every
read is bounded; damaged files are refused or read, tested by 400 mutations of each fixture under ASAN and UBSAN.

### Write import (`write.c`)

Header, FOD pages, CHP/PAP prefixes with the SDK defaults, fonts, text (Windows-1252), tabs, alignment (including both-margin
justification), picture paragraphs: monochrome and 16-colour device-dependent bitmaps become DIBs, metafiles are kept, static DIB
OLE objects are shown; other objects and header or footer paragraphs are counted.

### Application

- **Open and save.** `.doc` and `.wri` appear in the Open filter; a PNG, JPEG, GIF or BMP file opened as a document is refused
  with "use Insert > Picture". A Word or Write document is saved as Rich Text Format: Save asks for a name, suggesting the
  same name with `.rtf`; the notice at open time says so.
- **Pictures and objects.** Insert > Picture (file dialog) and Insert > Object (Create New: a blank 256 x 192 Bitmap Image;
  Create from File: a path typed in the dialog). PNG and JPEG are kept as they are, BMP becomes a DIB, GIF is re-encoded
  as PNG (stb_image and stb_image_write, `image.c`). A picture wider than the printable width is scaled to fit. Pictures are
  characters: caret, selection frame, Backspace, copy and paste, undo. A picture that cannot be decoded (a metafile) is shown
  as a labelled frame, saved and printed as a frame.
- **Printing (G31).** File > Print (Ctrl+P), the toolbar button and the preview's Print button open the shared Print dialog
  through `cp_ui_doc` with the document's own paper and margins; when OK is pressed the document is laid out for the chosen printer
  (`wp_print_prepare`, on the UI thread, so libfont stays single-threaded) into a page description (`struct cp_doc`: text, rules
  and pictures in twips) that the worker writes with `cp_submit_doc`: PostScript uses Helvetica, Times and Courier with
  ISOLatin1 re-encoding, PCL its scalable Univers, CG Times and Courier with the Windows Latin 1 symbol set, a text-only printer
  gets the characters. The same atoms the screen and the preview use (`wp_layout_pieces`) place every word, so the pages
  match the preview (tested page for page).
- **Clipboard (G5).** Copy and Cut publish Unicode text, "Rich Text Format" (registered, NUL-terminated) and, for a lone
  picture, a DIB; Paste prefers Rich Text Format, then text, then a DIB picture. Without a desktop clipboard the former private
  copy keeps working.
- **Help (G15).** F1 and Help > Help Topics open CiukiOS Help at the `wordpad` topic; six WordPad topics are added to the Programs
  book; Shift+F1 starts What's This?; the toolbar and format bar buttons, the document and the ruler have tooltips and
  What's This? texts.
- **Regional dates (G24).** Insert > Date and Time lists the short date, the long date, `d MMMM yyyy`, `MMMM d, yyyy`, the time
  and combinations from `strftime` after `setlocale(LC_ALL, "")`, so the Regional Settings pictures and month names apply.
- **View > Options.** Measurement units (inches, centimeters, points, picas) for the ruler and the Paragraph, Tabs and Page
  Setup dialogs (values can carry a unit: `2 cm`, `36pt`, `1.5pi`, `5mm`), word wrap per document type (Rich Text, Text,
  Word, Write: wrap to window, no wrap with a horizontal scroll bar, wrap to ruler = the printable page width) and the four
  bars. Settings are kept in `/system/settings/wordpad.cfg` (Windows 98 keeps them in the registry).
- **Ruler.** Ticks for the unit (eighths of an inch, half centimetres), markers for the first-line, left and right indents and
  every left tab stop of the paragraph at the caret. Dragging a marker changes the selected paragraphs (the first-line marker
  alone; the left marker moves the paragraph with its hanging offset; the right marker the right indent); a click on an empty
  place adds a tab stop, dragging moves it and dragging it off the ruler removes it. A guide shows the position while dragging
  and one undo level covers the whole drag. Positions snap to 1/16 inch, 1 mm, 1 point or 1/4 pica.
- **Justified text.** Format > Paragraph > Justify and Ctrl+J: the first `n` spaces of every wrapped line share the missing width
  in whole 1/64 pixels (no drift), so the caret, selection and hit testing agree on justified lines. The last line of a paragraph and
  lines ended by a soft break are not stretched.
- **Undo.** Up to 100 levels (`WP_UNDO_LEVELS`) and 64 MiB of text (`WP_UNDO_BYTES`, the oldest levels go first; one document over
  the budget keeps no undo); redo until the next edit; typing and deleting runs share a level; the selection is restored.
- **Fixed in passing.** The selection of a wrapped line was not painted (the caret at `ln->end` lies on the next line); the
  highlight now ends at the last piece of the line.

### Decisions needed from the lead (G28b)

1. **Saving Word and Write documents.** WordPad cannot write `.doc` or `.wri`; they are saved as `.rtf` through Save As. Windows 98
   offers "Word for Windows 6.0" and Write converters on save; writing Word 6 is a large separate item.
2. **Settings file.** The options use `/system/settings/wordpad.cfg`; moving them to the registry (`HKCU\Software\Microsoft\Windows\
   CurrentVersion\Applets\Wordpad\Options`, as Windows 98) needs the registry client in libdeskui programs.
3. **G16 switch.** The Font dialog stays the stop-gap behind `font_dialog()` in `app.c` (called from `command()`, result through
   `apply_chars()`); the lead replaces its body by the common `du_font` dialog at integration (see the comment above `font_dialog`).
4. **Printing from the UI thread.** The page layout runs when OK is pressed (a very long document takes as long as one relayout);
   the worker only writes. A worker layout would need a thread-safe libfont.
5. **PCL page offsets.** The PCL driver positions in dots from the logical page without the printer's unprintable margin, as
   the existing text and image paths; real printers may need `ESC&l0E` calibration on hardware.
6. **Metafiles.** WMF, EMF and PICT pictures are kept and printed as frames, not drawn (no metafile renderer exists).

### First-part decisions that stay open

Default margins are 0.75 inch on all sides with A4 unless the region (`LANG`, set by the desktop from Regional Settings)
starts with `en_US`; the Windows 98 WordPad defaults could not be confirmed from a primary source. The pinned frames of the
application test use fixed-advance fake fonts and are not guest evidence.

## Gaps left

Word and Write: floating drawings and shapes, compressed metafile BLIPs, single-sprm piece modifiers (prm variant 1) of
fast-saved files (the table of operand sprms is not in the specifications that were reachable), real tables, section and page
properties (the page setup stays WordPad's), headers, footers and footnotes, field codes (only results), numbering formats beyond
decimal, roman and letters, bidirectional and East Asian text, Word 2000+ features not stored in the Word 97 structures;
`.doc` and `.wri` export; OLE activation (double-click to edit an object) and linking; other OLE objects than pictures when
inserting; WMF/EMF/PICT drawing; picture sizing and cropping by dragging; right, centre and decimal tab alignments (the
importers keep left tabs only); registry persistence of options; the common Font dialog (G16); What's This? on dialog
controls and troubleshooters (G15b); Win32 common-dialog guest qualification, hardware printing and guest evidence of every
screen (all host tests use fixed-advance fonts and pinned frames reviewed as screenshots on 2026-10-24).

### Integration: G15/G16 tooltip storage and G5 plain-text paste

WordPad uses G16's `du_set_tip` for the owned tooltip array, retaining G15's
help strings. The G5 clipboard fixture exposed that the plain-text paste path
still read the legacy file clipboard after detecting system Unicode text.
[Microsoft's standard clipboard formats](https://learn.microsoft.com/en-us/windows/win32/dataxchg/standard-clipboard-formats)
defines CF_UNICODETEXT as NUL-terminated Unicode with CR/LF lines. Reuse the
existing system text decoder for paste and its menu availability; fall back to
the toolkit file clipboard only when no system transport is bound. RTF and DIB
priority and newline sanitization retain their existing paths. The production
WordPad host fixture covers formatted, picture and cross-client plain text.
