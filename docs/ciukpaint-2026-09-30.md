# CiukPaint — 30 September 2026

`PAINT.APP` is CiukiOS's original bitmap editor. Its theme and tool glyphs
are drawn for CiukiOS; it does not ship Microsoft artwork. It has pencil,
brush, line, shapes, fill, eraser, selection, text, zoom and grid, edit and
image menus, a colour palette, context menu, help, and 8-bit BMP save plus
1-, 4-, 8-, 24- and 32-bit BMP open. Selection operations include move, cut, copy, paste,
flip and rotate. It uses the desktop's installed CFN fonts for text.

The default canvas is 512×384. Undo pixels are held in XMS when available;
the fallback is DOS memory. The application's 16-bit DGROUP is 64,640 bytes
including its stack, under the 65,536-byte limit. On the current QEMU image
the CiukPaint gate passes drawing, fill, text, undo, BMP save and reopen:
`scripts/qemu_test_ciukpaint.py`. Its file browser still lists 8.3 aliases,
and its path buffer is limited to 80 bytes. It has only one undo level.
Physical hardware is untested.
