# f2-40 — CiukPaint: port of the 0.8 Paint application to the native desktop

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f2-40` from `main`. Host-testable (libdeskui harness, renders); the lead qualifies on QEMU/T23.

## Owner request (2026-10-12)

"Manca CiukPaint che deve essere uguale a quello che abbiamo fatto nella vecchia baseline."

## Scope

Port `src/apps/paint.c` (the 0.8 line, 3128 lines, DOS toolkit) to a libdeskui program `apps/paint` (`/Programs/paint`, Start > Programs > CiukPaint, Tango `applications-graphics`), keeping the same features and look: the tool box (pencil, brush sizes, line, rectangle, filled rectangle, ellipse, fill, text, eraser, picker, selection/move if present), the colour palette and the current colour pair, the canvas with scrolling, zoom if present, undo/redo depth as in the original, New/Open/Save/Save As (the original file format kept for compatibility plus BMP and PNG save via stb_image_write vendored under `third_party/stb` with its licence notice), Image > Attributes (size), Clear, Help/About with the tagline. Keyboard shortcuts as in the original. Read the original source thoroughly first and list in the report every feature with its status (ported, adapted, dropped with reason).

Window 640x480 default, resizable (f2-34 relayout). Files integration: `.bmp`/`.png`/the original extension open with CiukPaint (`apps/desktop/catalog.h` extensions).

## Tests

libdeskui harness: tool operations on a small canvas with pixel assertions, undo/redo, file round trips (original format, BMP, PNG), renders of the main window to `build/f2-40/*.png` for the lead's comparison with the legacy screenshots (`legacy/CiukiOS-docs-legacy-2026-10-09.zip` may contain them; do not restore archived files, extract to `build/` only).

## Constraints

No QEMU, no commits, no diary, `nice -n 19`. Report to `build/codex/f2-40-report.md`.
