# fx-fonts-fallback: font fallback, symbol fonts and raster .FON fonts (FT-6, FT-2)

- **Written:** 2026-10-29 11:59 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `high`.
- **Worktree:** `/home/peronslayer/Desktop/CiukiOS-wt-fx-fonts-fallback`, branch `wt/fx-fonts-fallback` from `main`.
- **Inventory:**
  - FT-6 "Font fallback and symbol fonts": absent ("No fallback family", `fonts-contract.md:63`);
  - FT-2 raster fonts: partial ("Emulated with Liberation faces; `.FON`/`.FNT` are refused").
- **Code:** FreeType 2.13.3 behind `apps/libdeskui/text_ttf.c` and Win32 `sdk/win32/gdi32/font.c`; contract `docs/design/fonts-contract.md`; tests `tests/host/font/*`.

## Required behaviour

Research first, and record the sources and decisions in `docs/design/fonts-contract.md`:
- Windows font linking and fallback (`HKLM\...\FontLink\SystemLink`, `FontSubstitutes`);
- the GDI font mapper's rules for charset, pitch and family (Microsoft Learn: `LOGFONT`, `CreateFontIndirect`, "Font Mapping");
- `SYMBOL_CHARSET` and the Symbol and Wingdings code points (the `0xF000` private-use mapping of symbol cmaps);
- the Windows 3.x/9x `.FON` (NE resource) and `.FNT` 2.0/3.0 formats from Microsoft's documentation, with Wine's `dlls/gdi32` and `win32u` font code as a reference implementation. All licences are allowed; keep the notices.

1. **Fallback.**
   - A glyph missing from the requested face is drawn from an ordered fallback list (for example DejaVu Sans for wide Latin/Greek/Cyrillic coverage), both in the native toolkit and in Win32 GDI text output.
   - GDI keeps the requested face's metrics for layout; the fallback provides only the outline.
   - The list is configurable in the registry the way `SystemLink` is.
2. **Symbol fonts.**
   - Ship free metric-compatible or replacement faces for Symbol and Wingdings: OpenSymbol (LibreOffice, MPL/LGPL) or any other free font whose licence the repository already allows. Add their licence notices.
   - Map `SYMBOL_CHARSET` requests and the `0xF020`–`0xF0FF` and plain `0x20`–`0xFF` code points the way Windows does, so that a program that draws Wingdings bullets or Symbol Greek letters shows the right glyphs.
3. **Raster fonts (FT-2).**
   - Load real `.FON`/`.FNT` raster fonts (NE font resources, FNT 2.0/3.0) for GDI, at their native sizes, with the bold and italic simulation GDI applies.
   - `MS Sans Serif`, `System`, `Fixedsys`, `Terminal` and `Courier` keep resolving to the shipped faces when no `.FON` file is installed. A `.FON` file installed by a program, or present in `C:\WINDOWS\FONTS`, is used.
   - Ship no Microsoft raster fonts.
4. **Image.** Stage the new faces and notices through the existing font payload machinery (`font_payloads()`), within the image's font budget. Report the sizes.

## Tests

- Host tests:
  - fallback for a missing glyph, natively and through GDI `TextOutW`/`GetGlyphIndicesW`;
  - Symbol and Wingdings code points under `SYMBOL_CHARSET`;
  - a generated `.FNT` 3.0 and an NE `.FON` loaded and drawn at their size, with bold simulation;
  - the registry-driven fallback order;
  - the font payload inventory.
- Update FT-6 and FT-2 in the inventory.
- Run `tests/host/font`, the Win32 GDI tests and the desktop font tests sequentially with `nice -n 19`.

## Rules

- Never commit or stage. Never run QEMU, `systemd-run` or image builds.
- Write the report to `build/codex/fx-fonts-fallback-report.md`.
