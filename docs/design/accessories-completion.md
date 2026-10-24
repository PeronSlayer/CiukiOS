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

Rotation is a view transform of the decoded picture (right, left, 180, `viewer_rotate`); the file
is not rewritten, and Set As Wallpaper uses the file as stored (the confirmation says so when
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
Screen-changing events: Set As Wallpaper sends the existing `DESK_SETTINGS_RELOAD`; the other
accessories are ordinary client windows (fx-15 window-model rows unchanged).
