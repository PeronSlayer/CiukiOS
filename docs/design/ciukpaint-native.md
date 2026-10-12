# CiukPaint native port (F2-40)

The behavioral reference is the complete `src/apps/paint.c` from the 0.8
baseline. It has 15 tools, a 28-swatch indexed palette, rectangular selections
and a process-local bitmap clipboard, one undo/redo level, previews, text with
CFN fonts, zoom at 1/2/4/6/8, scrolling, and image transforms. Its original
format is uncompressed 8-bit BMP, extension `.bmp`; there is no private format.
The default image is 512 × 384 inside a resizable 640 × 480 native window.

Research, checked 2026-10-12 before implementation:

- [Microsoft bitmap storage](https://learn.microsoft.com/en-us/windows/win32/gdi/bitmap-storage):
  file header, DIB header, BGR palette, bottom-up scanlines and four-byte row
  alignment agree with the legacy writer. Keep that indexed writer as the
  default for compatibility, with explicit 24-bit BMP export as well.
- [stb_image_write upstream](https://github.com/nothings/stb/blob/f58f558c120e9b32c217290b80bad1a0729fbb2c/stb_image_write.h):
  callback writers take top-down interleaved RGB. Callbacks do not report I/O
  failures: record short writes separately and check flush/close. Use these
  writers for PNG and 24-bit BMP; retain the upstream licence notice.
- [PNG specification](https://www.w3.org/TR/png-3/): RGB and alpha are separate
  channels. Import PNG into the legacy palette, compositing alpha over white;
  export lossless RGB. This preserves the original editor's colour model.

Native adaptation: replace segmented DOS/XMS storage with bounded heap images
and saved rows, preserving the original drawing and preview algorithms. Text
uses the unchanged CFN glyphs, widths, regular/bold styles and 1–4× scaling.
Toolkit canvas input needs both mouse buttons, capture beyond the canvas, and
cancellation on input resynchronization/focus loss. Add a drawn-by-application
canvas widget with pointer events through the existing libdeskui dispatch.
Menus, file browsing, modal dialogs and resize use libdeskui. Keep the legacy
tool glyphs (project artwork) and the Tango applications-graphics launcher icon.

The usual Attributes bounds stay 2048 × 1200. A quarter-turn may exchange the
axes; image storage and undo row tables accept either orientation, capped at
2048 per axis and 2048 × 1200 pixels. Allocation or decode failure must retain
the existing image. Imported and saved files use native paths and long names.

Host verification and per-feature status are recorded in
`build/codex/f2-40-report.md`. Guest and hardware qualification belong to the lead.

## Merge integration (2026-10-13)

The merged inventory retains CiukPaint, its CFN fonts and writer notice together
with main's CiukIDE/TinyCC and system sounds. CiukPaint adds no syscall; display
call 74 and launch-mark call 75 remain unchanged. Regenerate the native API
reference from the merged public headers so TinyCC's installed developer
reference includes both display/sound APIs and the canvas/dialog additions.

The TinyCC build guard exposed a missing vendored source:
[`conftest.c` at the pinned upstream commit](https://github.com/TinyCC/tinycc/blob/d348a9a51d32cece842b7885d27a411436d7887b/conftest.c).
Restore those exact upstream bytes, retaining the existing source/patch pins;
SHA-256 `3e0937949cf03a8983437f5997196fb4e2fafae6d3cdfb7a9baadbf7a4252f7a`
and Git blob `2824cc89d7dff62d7e436088c4f91c73c0324981` both match the recorded
inventories. The unchanged upstream `.gitignore` ignores `conftest*`, so the
lead must explicitly stage this source. Merge validation is recorded in
`build/codex/f2-40b-report.md`.
