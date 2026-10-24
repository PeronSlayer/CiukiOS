# Accessories completion (G27)

Written 2026-10-24 for directive `g17-g32-os-gaps-batches-4-5.md`, item G27, rows AX-3, AX-4,
AX-5, AX-6, AX-8, AX-11, AX-17 and FT-7 of `os-feature-inventory.md` (the inventory carries the
status and the gaps of each row). The reference is the Windows 98 accessory of the same name.

## Calculator (AX-4)

Scientific view: precedence (Or/Xor < And < Lsh/Rsh < + - < * / Mod < x^y), 25 levels of stack,
Dec/Hex/Oct/Bin as 64-bit two's-complement integers, Degrees/Radians/Grads, Inv and Hyp as the
check boxes of the Windows calculator (cleared by the next function), F-E, Exp, dms, Dat/Ave/Sum/s
with Inv for the squares and the population deviation. Keys follow the Windows 98 shortcuts
(s o t q # y l n ! p x m ; i h v ( ) % & | ^ < ~, F2-F8, Ctrl+A/S/T/D). Results keep their full
precision internally while the display shows 15 digits, so `sin(pi)` is 0. The window is created
at the Scientific size: protocol v2 has no client-requested resize (only v3 `DESK_V3_RESIZE`).

## Character Map (AX-5, FT-7)

Fonts: the built-in face (bitmap, Basic Latin and Latin-1 as `glyph_index` in `theme.c`) and every
`.ttf` of `/System/fonts` through `sdk/libfont` (FreeType). Glyph presence comes from the font's
cmap (`ciuki_font_lookup`), so an absent character is an empty cell. Subsets are blocks of the
Unicode Character Database (`Blocks.txt`, https://www.unicode.org/Public/UCD/latest/ucd/Blocks.txt).
The Alt+0nnn keystroke lists the Windows (code page 1252) characters.

## Paint (AX-3)

Stretch/Skew (Windows 98 order: stretch, horizontal skew, vertical skew; 1-500 %, -89..89
degrees, nearest pixel so every result pixel stays a palette index). Edit Colours cannot recolour
the picture (the model is indexed), so a colour absent from the picture and from the boxes takes a
free palette entry (6-bit DAC values), an existing identical colour is reused, and with all 144 in
use the nearest one is taken. GIF: an own GIF89a writer (global table of the used indices, LZW with
a hash dictionary, 12-bit codes, clear on a full table), verified by decoding with stb_image;
JPEG: `stbi_write_jpg` at quality 90. Set As Wallpaper writes the picture as a 24-bit BMP into
`/system/wallpapers/CiukPaint.bmp` and updates `wallpaper` / `wallpaper_mode` in
`/system/settings/desktop.cfg` (other keys kept), then sends `DESK_SETTINGS_RELOAD`, the two
steps of the Control Panel (`apps/accessories/wallpaper.c`). The desktop decodes BMP, PNG and JPEG.

## Image Viewer (AX-6)

Rotation transforms the decoded picture (right, left, 180, `viewer_rotate`); G27B Save As
writes those transformed pixels into a new lossless BMP, and Set As Wallpaper uses the file as stored (the confirmation says so when
the picture is rotated). GIF cannot be a wallpaper (the desktop has no GIF decoder).

## Serial Terminal (AX-8)

Native COM ports through the f1-111 ABI (`/dev/com1..8`, `COM_SET_LINE`, `COM_SET_MODEM`,
`COM_GET_STATUS`), polled every 20 ms. Screen: ANSI/VT100 subset (cursor moves, erase, SGR bold and
reverse). XMODEM, XMODEM-CRC and XMODEM-1K follow Chuck Forsberg, *XMODEM/YMODEM Protocol Reference*
(1988; https://wiki.zubax.com/public/development/XMODEMYMODEM-protocol-reference): SOH/STX frames,
block number and complement, checksum or CRC-16/CCITT (`0x1021`, "123456789" = `0x31C3`), `C` to
request CRC and a fall-back to the checksum after three tries, 10 retries, CAN CAN, 0x1A padding
(trailing 0x1A bytes of a received file are removed, as every XMODEM receiver does). The receiver
waits 20 s before a NAK and the sender 10 s before repeating: an ACK carries no block number, so
two simultaneous repeats would make the sender skip a block. A finished receiver keeps
acknowledging a repeated EOT for 3 s.

## Games (AX-11)

- Minesweeper: levels and mine counts of Windows 98 (9x9/10, 16x16/40, 30x16/99, Custom 9-30 x
  9-24 up to (w-1)(h-1) mines), mines laid out at the first click (never under it), chording with
  both buttons or the middle button, ? marks option, 999-second timer, best-times file.
- Solitaire (Klondike): Standard scoring (+5 waste to column, +10 to a foundation, +5 turning a
  column card, -15 foundation to column, -100 per recycle in Draw One, -20 per recycle after the
  third pass in Draw Three, never below 0, timed bonus 700000 / seconds over 30 s). Sources for
  the figures: https://rarepike.com/solitaire/scoring/ and https://solitaire.com/blog/solitaire-scoring-system/
  (third-party; Microsoft's own text was not available, and the stock figures differ between
  sources: the Draw Three recycling penalty is the one most in doubt).
- FreeCell: the Microsoft deal numbering. Algorithm (https://rosettacode.org/wiki/Deal_cards_for_FreeCell):
  `seed = game; seed = seed*214013 + 2531011; index = (seed >> 16) & 0x7fff` modulo the remaining
  cards, the chosen card swapped with the last. Pinned by the known deals: game 1 starts
  `JD 2D 9H JC 5D 7H 7C 5H`, game 617 starts `7D AD 5C 3S 5S 8C 2D AH`, game 11982 is the single
  unwinnable deal of the original 32000 (https://en.wikipedia.org/wiki/FreeCell). Run limit
  (free cells + 1) * 2^(empty columns), an empty destination column not counted.
- Hearts: pass left, right, across, none; the two of clubs leads; follow suit; no points on the
  first trick unless forced; hearts cannot lead until broken unless only hearts remain; 1 point a
  heart, 13 the queen of spades; shooting the moon gives the others 26; the game ends at 100.

## Terminal (AX-17)

See `docs/terminal.md`. The pipeline runs stages sequentially through an in-memory capture, the
batch interpreter follows the MS-DOS 7 COMMAND.COM semantics listed there.

## Limits common to the accessories

Windows are fixed-size (no client resize in protocol v2). No ABI or public SDK header changed.
Screen-changing events use ordinary client damage; G27B appends fx-15 rows 104–107 after the existing integration scenarios.
Set As Wallpaper sends the existing `DESK_SETTINGS_RELOAD`.

## G27B continuation: expected behaviour and implementation decisions

Research checked 2026-10-24. The Windows 98 Resource Kit Appendix D
`wrkappd_3.htm` names Calculator, Paint, Imaging and HyperTerminal;
chapter 11 `wrkc11_1.htm` and `wrkc11_2.htm` describe the shared printing
and image output services. The local extracted book is research material only.

* Sta opens a Statistics Box with the stored data in insertion order, a selected
  row, RET (return), LOAD (copy the selected value to the display), CD (delete
  selected data) and CAD (clear all data). Opening/returning never clears data.
  The existing Dat/Ave/Sum/deviation engine remains the owner of the data.
* Paint already routes File Print/Page Setup to G31, snapshotting the entire
  unsaved picture, independent of zoom. Reuse and verify this integration.
* Viewer Save As writes the currently decoded and rotated pixels as a lossless
  24-bit BMP, with a shared Save dialog and overwrite confirmation. A failed
  save leaves the source intact; use a sibling temporary file then rename.
  BMP rows and padding follow Microsoft's [BITMAPINFOHEADER specification](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-bitmapinfoheader).
* FORMAT uses G29's generation-checked MEDIA_QUERY_FORMAT/MEDIA_FORMAT service,
  accepts a drive, Quick/Full, FAT family and label, prints capacity and a warning,
  and requires an explicit YES argument before any destructive call. Fixed and
  boot volumes remain refused by the service. Unsupported DOS switches fail.
* PING already uses G26 raw ICMP echo, identifier/sequence matching and timeouts,
  per [RFC 792](https://www.rfc-editor.org/rfc/rfc792). Keep one implementation.
* RUN executes a native child with arguments and redirected standard streams,
  waits while pumping UI input, reports the exit code and supports interruption.
  GUI/DOS/Win32 launches continue through START and the desktop launcher.
* REG exposes the existing native libreg transactional service: query, add,
  delete, import and export, with typed literals and explicit deletion force.
  It does not translate native registry files into Windows REGEDIT4 syntax.
* Serial transfers use bounded incremental engines, with output backpressure,
  retransmission, cancellation and binary integrity. YMODEM uses block zero,
  declared size, CRC16, ACK/NAK/EOT and empty end-of-batch header per Forsberg's
  [XMODEM/YMODEM reference](https://wiki.zubax.com/public/development/XMODEMYMODEM-protocol-reference).
  ZMODEM uses CRC16 headers/data, ZRINIT/ZFILE/ZRPOS/ZDATA/ZEOF/ZFIN and ZDLE quoting,
  per [Forsberg's original specification](https://www.tuhs.org/Usenet/comp.sources.unix/1986-May/004372.html).
  Kermit uses negotiated short packets, type-1 checksum, control quoting and
  stop-and-wait sequence numbers per Columbia's [Kermit Protocol Manual](https://www.kermitproject.org/kproto.pdf).
  Receiver output is the explicitly chosen local filename; remote names cannot
  select arbitrary local paths. One selected file per transfer invocation.


G27B implementation limits: Statistics Box uses the existing modal toolkit sheet;
RET/LOAD return to the calculator with the data retained, and Sta reopens the list.
It is not an independently movable modeless desktop window. This is a deliberate
reuse of the native one-window-per-channel toolkit, rather than a new multiwindow
protocol in this accessory item. The data count remains bounded at 256.
The action definitions were checked against Microsoft's [Windows 3.0 User's Guide,
Additional Accessories](https://www.bitsavers.org/pdf/microsoft/windows_3.0/Windows_3.0_Users_Guide_198909.pdf),
which documents the original Statistics Box workflow; independent modeless
window activation remains a difference from that reference.

Transfers select one file and an explicit local destination, preserving an existing
destination on cancellation/error through a sibling temporary file. YMODEM receives
SOH/STX blocks and exact declared sizes, and terminates the batch with an empty
block zero. A second nonempty batch header is refused. ZMODEM deliberately
advertises no CRC32/full-duplex/overlapped-I/O capabilities, receives CRC16 streaming
subpackets and sends bounded ZCRCW blocks with position recovery. Kermit negotiates
short packets and type-1 checksums, without long packets, sliding windows or repeat
compression. Automatic ZMODEM detection, multi-file batches and qualification
against independent period peers remain outside the host round-trip evidence.
RUN connects EOF when no redirected input exists; interactive child console input
is not implemented. REG uses native libreg roots and typed literals; `STRING text`
avoids ambiguity with shell quote removal. No public ABI or SDK headers change.

Paint G27B integration repair: the G31 Print/Page Setup entries shifted the
wallpaper commands, but `update_menus` still disabled file-menu indexes 5/6
when no wallpaper callback existed. Those are now Page Setup/Print, so Ctrl+P
was unreachable in the host fixture. Enablement now matches command IDs and
separately tests the print and wallpaper providers. Registering a print provider
refreshes menu state immediately; a missing provider disables its commands.

BMP output is refused above the existing 32 MiB Viewer file limit, so every
successfully saved picture remains readable within the same file budget.

Merge validation (2026-10-24): checked Microsoft's [menu item identifiers and
positions](https://learn.microsoft.com/en-us/windows/win32/menurc/about-menus#menu-item-identifier).
Command identifiers remain associated with commands as menu positions change.
Paint retains command-ID-based enablement for both the print and wallpaper
providers, preserving the existing G31 menu layout without hard-coded offsets.

## Visual audit fixes, group C (2026-10-24)

Decisions from the owner's QEMU visual audit; each item targets Windows 98 layout quality, not Microsoft artwork.

- **Music Player sliders.** Seek (0..1000) and Master volume (0..100) are `DU_TRACKBAR` widgets (as the Volume Control)
  instead of arrow-button scrollbars; the seek/volume event semantics and tests are unchanged.
- **Paint colour box.** The 28 swatches are the Windows 98 Paint default palette, now exact in the indexed palette:
  black and white keep indices 79/80, the other 26 colours replace the last 26 icon-derived palette entries (118..143).
  Values (R,G,B hex): 000000 808080 800000 808000 008000 008080 000080 800080 808040 004040 0080FF 004080 4000FF 804000 /
  FFFFFF C0C0C0 FF0000 FFFF00 00FF00 00FFFF 0000FF FF00FF FFFF80 00FF80 80FFFF 8080FF FF0080 FF8040. Source: the old-style
  (Windows 95/98) Paint default colour box as reproduced by open-source Paint reimplementations; all components are
  multiples of 0x40 (0xFF), hence exact in the 6-bit palette. No Microsoft documentation lists the values.
- **WordPad format bar.** Font and Size are editable combo boxes with drop-down lists (installed font names; sizes 8..72);
  Enter applies a known font name or a size in points (half points allowed). Bold/Italic/Underline are drawn letters
  (B bold face, I slanted, U underlined) with the Win98 dithered, sunken pressed state.
- **Cards.** The suit pip sits on the rank's line (top-left), so overlapped cards (16 px visible) show rank and suit.
- **Clock.** The "CiukiOS" caption on the dial is removed: the Windows 98 Clock face has none and it was unreadable under the hands.
