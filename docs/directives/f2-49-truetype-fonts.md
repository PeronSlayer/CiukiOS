# f2-49 — TrueType fonts: rasteriser library, free metric-compatible font set, Fonts page

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f2-49` from `main`. Host-testable. Owner decision 2026-10-13: complete the F5 prerequisites now; this one gives GDI (F5 P3) and the native apps real scalable fonts.

## Why

The Win32 contract (`docs/design/win32-personality-contract.md`, file-system view: `C:\WINDOWS\FONTS` read-only; P3 `gdi32` fonts) needs TrueType rendering and a font set with the metrics of Arial, Times New Roman and Courier New, without Microsoft files. Today the desktop draws only its bitmap fonts (`apps/desktop/ui-fonts.json`, `apps/libdeskui/draw.c`).

## Task

1. **Research and decide** (report + `docs/design/fonts-contract.md`): `stb_truetype` (public domain, single header, already used style in `third_party/stb`) versus a FreeType port (hinting, size, build complexity). Criteria: readable text at 8–12 pt on a 1024x768 LCD with grey anti-aliasing, cost per glyph on a Pentium III (the desktop budget is in `docs/design/desktop-redraw-performance.md`), licence, and what GDI will need (TrueType `cmap`, `hmtx`, `kern`, `OS/2` metrics, `name` table for family/style). Cite sources.
2. **Font set**: Liberation Fonts 2.1.x (SIL Open Font License 1.1; Sans, Serif, Mono in regular, bold, italic, bold italic) in `assets/fonts/liberation/` with the licence text and the upstream hashes recorded like `assets/icons/README.md` does for Tango; installed by `scripts/build_image.py` into `/System/fonts/` with manifest hashes and inventory tests. Family aliases for F5 (`Arial` → Liberation Sans, `Times New Roman` → Liberation Serif, `Courier New` → Liberation Mono) written in the contract, not hard-coded in the rasteriser.
3. **Library** `libfont` in the SDK (`sdk/`, header `ciuki/font.h`): open a font file, font metrics (ascent, descent, line gap, units per em), glyph lookup by Unicode code point, rasterise a glyph at a pixel size to 8-bit coverage, measure and paint a UTF-8 run left to right with kerning when the `kern` table exists, bounded glyph cache (per face and size), no floating point in the per-pixel paths if the chosen rasteriser allows it. Thread-safe per face handle.
4. **libdeskui**: `du_text_face`/`du_text_ttf` painting through the same clip and colour model as `du_text`, with a sub-pixel-free grey AA blend exact in integer arithmetic; the existing bitmap text paths stay the default for the desktop (no visual change in this directive).
5. **Control Panel, "Fonts" page**: list of installed fonts with family, style, file, and a preview line ("Classic computing, one woof at a time." at 10 and 14 pt) rendered with `libfont`; this is the user-visible proof.
6. Host tests: metrics against the font files' tables, rasteriser goldens with a tolerance documented per pixel, cache bounds, UTF-8 edge cases, the Control Panel page through the desktop harness, inventory of the installed files; a host benchmark reporting µs per glyph at 10 pt.

## Acceptance

Host suites green; `make CAP= apps` and `make CAP= sdk` build; `docs/developer-api.md` regenerated; the contract names the licence obligations and the F5 alias table.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f2-49-report.md`.
