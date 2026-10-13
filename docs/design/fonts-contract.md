# F2-49 scalable fonts

Research and implementation contract, 2026-10-13. Host evidence only; the lead
owns QEMU, LCD readability and Pentium III qualification.

## Rasteriser decision

Use [stb_truetype 1.26](https://github.com/nothings/stb/blob/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_truetype.h),
unmodified, with `STBTT_RASTERIZER_VERSION=1`. Its scanline coverage loop uses
10-bit fixed-point edges and integer pixel writes; outline flattening and edge
setup still use floating point (x87 on Ciuki). Version 2 uses floating point
per pixel. Upstream describes version 1 as roughly 15% slower; that is an
upstream comparison, not a Pentium III measurement. Retain the bounded cache
so repeated drawing only needs lookup, integer placement and integer blending.
Choose the header's Public Domain alternative; preserve both notices.

[FreeType](https://freetype.org/freetype2/docs/hinting/text-rendering-general.html)
offers TrueType bytecode/auto hinting, which better controls stems at small
sizes, and is the stronger future option if LCD qualification fails. It needs
a separate port, module/configuration selection, fixed-point support and a
larger build/runtime dependency than the existing stb style. Its
[FTL/GPLv2 choices](https://freetype.org/license.html) also need an explicit
distribution choice. stb has no hinting: do not promise identical GDI pixels.
At 96 logical dpi, 8–12 pt means 10.667–16 em pixels; grey AA is suitable for
the Fonts preview, subject to visual validation on the 1024x768 LCD. Desktop
bitmap paths remain the default. The [desktop budget](desktop-redraw-performance.md)
requires bounded redraw work and a 60 Hz ceiling; cold glyph timings from a
modern host are not evidence for its 50 ms T23 input bound. Report cold/hot
microseconds per glyph at 10 pt separately.

Upstream explicitly does not support hostile fonts. `libfont` accepts trusted
TrueType sfnt files only (not TTC/CFF); the bundled, hash-pinned store is the
supported source. Table-directory/range checks reject ordinary damaged files
but are not a security boundary. F5 must not pass arbitrary downloaded game
fonts to this backend without a validated parser or a FreeType replacement.

## Fonts and F5 aliases

Liberation 2.1.5: Sans, Serif, Mono, each Regular/Bold/Italic/Bold Italic.
Install the unchanged TTFs and `LIBERATION-LICENSE.txt` in `/System/fonts/`
(FAT lookup is case insensitive). The immutable Win32 view `C:\WINDOWS\FONTS`
will expose this store. The rasteriser resolves Unicode/glyphs only; F5's font
selection layer owns this alias table and style matching:

| Requested family | Installed family |
| --- | --- |
| Arial | Liberation Sans |
| Times New Roman | Liberation Serif |
| Courier New | Liberation Mono |

[Upstream](https://github.com/liberationfonts/liberation-fonts/tree/2.1.5)
states metric compatibility with those families, not identical outlines.
The [SIL OFL 1.1](https://openfontlicense.org/open-font-license-official-text/)
requires preserving copyright/license when redistributing; do not sell fonts
alone, keep derivatives under OFL and respect Reserved Font Name Liberation
when modifying. Documents rendered with them are not covered by OFL. No font
modifications or Microsoft font files are included. See
[asset provenance](../../assets/fonts/liberation/README.md) for source/version
and SHA-256 records; image inventory includes hashes of every installed file.
The worktree currently pins distribution-built 2.1.5 TTFs; verification against
the upstream binary archive is still pending and is not claimed by these hashes.

## SDK and rendering semantics

`ciuki/font.h`, `-lfont`: an opaque face owns file bytes and a mutex. Calls
serialize per face; close requires the caller to quiesce other users. Metrics
are design units: `head.unitsPerEm`, `hhea` ascent/descent/line gap and the
`OS/2` typographic/Windows metrics, weight and selection flags. Family/style
come from Unicode `name` records (English Windows preferred, UTF-16BE decoded
to bounded UTF-8). Glyph 0 is the missing character, as specified by
[cmap](https://learn.microsoft.com/en-us/typography/opentype/spec/cmap).
Use [hmtx](https://learn.microsoft.com/en-us/typography/opentype/spec/hmtx)
for advances/bearings, legacy horizontal format-0
[kern](https://learn.microsoft.com/en-us/typography/opentype/spec/kern) when
present (no GPOS shaping), [OS/2](https://learn.microsoft.com/en-us/typography/opentype/spec/os2)
and [name](https://learn.microsoft.com/en-us/typography/opentype/spec/name)
for future GDI enumeration/metrics.

Sizes are em pixels in unsigned 26.6 fixed point, 1–128 px. 10/14 pt at 96 dpi
are 853/1195 respectively, rounded to the nearest 1/64 px. Runs take explicit
UTF-8 byte lengths and are a single left-to-right line: no bidi, shaping,
fallback family, tab/newline expansion or combining-mark positioning. Invalid,
overlong, surrogate, out-of-range and truncated UTF-8 consumes one byte as
U+FFFD; embedded NUL is a code point when included in the length. Advances and
kerning use signed nearest rounding in 26.6, with overflow errors. The pen is
rounded to integer pixels for painting; no LCD colour subpixels or fractional
phase cache. Raster bitmaps have baseline-relative left/top offsets and an
8-bit coverage byte per pixel. Standalone raster results are owned snapshots;
paint callbacks borrow the bitmap under the face lock and must not re-enter
that face. Measure and paint share exactly the same placement calculation.

The per-face cache keys glyph and size, with at most 128 entries and 256 KiB
of coverage across all sizes; LRU eviction bounds both. The face file is at
most 8 MiB; a glyph is at most 64 KiB. Allocation errors propagate instead of
publishing partial cache entries. Warm painting allocates no coverage copies.
`du_text_face` paints at a baseline; `du_text_ttf` is the file convenience
wrapper. Both obey the existing canvas clip and XRGB model. Each channel is
`(foreground*a + background*(255-a) + 127)/255`; the destination high byte is
zero, exactly like bitmap text. All blend/clip/placement pixel loops are integer.

## Evidence

Host tests independently decode font tables, compare coverage goldens, cover
cache eviction across sizes, malformed UTF-8, missing glyphs, concurrent face
use, clipping/blending, and the production Control Panel page in its desktop
harness. Coverage golden tolerance is at most one level (out of 255) per
pixel for floating-point outline setup variation; dimensions and placement
are exact. Re-pinning requires a reviewed rasteriser or asset change. No host
result establishes actual 8 pt LCD readability or Pentium III glyph cost.

Final host measurement (x86_64, `-O2`, 2026-10-13, 95 ASCII glyphs, 10 pt/96 dpi,
no framebuffer blending): 9,500 cold glyphs averaged 0.9397 us/glyph; 95,000
cached glyphs averaged 0.0354 us/glyph. These figures characterize the host
backend/cache path, not the Pentium III or the complete display pipeline.
