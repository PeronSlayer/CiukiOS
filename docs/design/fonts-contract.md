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

## 2026-10-14 — F2-63: FreeType everywhere (supersedes rasteriser decision)

The owner's FreeType-everywhere decision replaces the stb backend above.
Pin **FreeType 2.13.3**, the latest 2.13.x release, to the
[upstream release](https://lists.nongnu.org/archive/html/freetype-devel/2024-08/msg00008.html)
and [archive/checksum](https://sourceforge.net/projects/freetype/files/freetype2/2.13.3/).
The SDK builds the same source/configuration for i686 and host evidence, offline
from the SHA-256-checked archive; there is no system-FreeType fallback. Compile
only base, SFNT, TrueType, smooth and autofit modules. Keep TrueType variations
for the existing Inter/Inconsolata UI axes; omit other format drivers,
colour/SVG, compression, PNG and HarfBuzz. Use grey coverage without LCD
presentation or filtering. Host and target builds run sequentially.

Choose `FT_LOAD_TARGET_NORMAL | FT_LOAD_NO_BITMAP` with normal grey rendering
and **TrueType interpreter version 35**. FreeType's
[driver properties](https://freetype.org/freetype2/docs/reference/ft2-properties.html)
associate v35 with the Windows 98 MS rasteriser v1.7. Normal hinting fits both
axes and advances to the pixel grid; light hinting preserves horizontal outlines
and spacing and is closer to later ClearType behavior. This is a documented
compatibility choice, not measured Windows 98 pixel parity. Explicitly configure
v35 rather than inherit a host or environment default. Autofit is retained for
fonts without usable bytecode. Apply the
[integer positioning bearing-delta correction](https://freetype.org/freetype2/docs/reference/ft2-glyph_retrieval.html)
for that fallback in both measure and paint.

Keep existing API signatures, design-unit metrics, UTF-8 rules, immutable trusted
store, clipping and coverage ownership. Add a size-metrics accessor returning
ascent/descent/line gap/maximum advance in signed 26.6 pixels, without rounding
away design precision. Raster and run advances use FreeType's hinted 26.6
advances. `FT_Set_Char_Size` preserves fractional em requests at 72 dpi. Use
`FT_Get_Kerning(..., FT_KERNING_UNFITTED)` for fractional legacy `kern` pairs.
The pinned 2.13.3 source does not offer the later basic GPOS option; implementing
GPOS/shaping is outside this change. See
[upstream kerning limitations](https://freetype.org/freetype2/docs/glyphs/glyphs-4.html).

Desktop, toolkit, Terminal and CiukNote already draw immutable FreeType-generated
coverage maps. Re-generate those maps with the same pinned minimal engine and
hinting choice instead of Pillow's independently versioned FreeType. Retain the
existing UI font families, variable-axis coordinates, sizes, integer placement,
fixed Terminal/editor grids and runtime allocation-free painting. These maps are
the bounded pre-rasterised UI cache; scalable faces keep the existing 128-entry,
256 KiB LRU and 64 KiB per-glyph coverage limit. FreeType's face/interpreter
working allocations are additional to coverage and are reported separately.
The approved portrait and other identity assets remain byte-for-byte unchanged.

Distribute under the [FreeType License (FTL)](https://freetype.org/license.html),
with upstream `FTL.TXT` and `LICENSE.TXT` in the SDK and `/system/licenses`.
The product notice acknowledges that parts of CiukiOS are based on the work of
the FreeType team. Preserve upstream source notices; the optional GPL alternative
is not selected. Reproducibility compares archives built in independent paths;
coverage goldens and before/after host PNGs are reviewed separately. Per-glyph
baseline budgets account for hinting changing boxes and advances: align coverage
at the baseline before comparing, rather than comparing unequal arrays. Host
checks cannot establish LCD readability, cold glyph cost or guest heap behavior;
the lead owns QEMU and T23 qualification.

The upstream 2.13.3 [configuration header](https://github.com/freetype/freetype/blob/VER-2-13-3/include/freetype/config/ftoption.h)
supports explicit options/module headers. Its
[module initialization](https://github.com/freetype/freetype/blob/VER-2-13-3/src/base/ftinit.c)
continues when a default module cannot be allocated. Therefore libfont uses a
per-face allocator with a failure flag and `FT_New_Library`/`FT_Add_Default_Modules`;
missing modules caused by OOM fail opening the face with ENOMEM. Do not accept a
partially initialized engine. The Demo's remaining handwritten 5x7 text loop
also moves to the existing FreeType UI atlas painter; the unused old glyph
array is removed. CiukNote's Font dialog names the actual Inconsolata atlas.

### 2026-10-14 — pinned-source qualification findings

The actual FreeType include chain searches a quoted options filename beside
`ftconfig.h` first. Use `ciuki_ftoption.h`/`ciuki_ftmodule.h`, with distinct names,
and test the macros after including `FT_FREETYPE_H`, rather than testing the
options file in isolation. Installed SDK configuration headers reflect the same
minimal configuration.

Allocation-failure injection found a null execution-context dereference inside
2.13.3 `tt_size_init_bytecode`: `TT_New_Context` can return NULL, but the function
continues into `TT_Load_Context`. The [pinned upstream source](https://raw.githubusercontent.com/freetype/freetype/VER-2-13-3/src/truetype/ttobjs.c)
confirms the missing check. Apply a small, explicitly marked CiukiOS patch that
returns `Out_Of_Memory` through the existing cleanup path. Pin the patch hash,
include it in source-cache validation and build provenance, and retain the
unchanged release archive. This fixes the failure path without changing raster
output or selecting another version. Reject allocation failures during face or
size initialization even if an optional upstream operation reports success.

[FreeType's glyph-metric conventions](https://freetype.org/freetype2/docs/glyphs/glyphs-3.html)
explain that grid fitting changes advances as well as bearings and bounds.
With the pinned Liberation instructions and v35, the measured 8–16 px ASCII
advance difference from scaled design units reaches 164/64 px (Sans Italic `w`,
16 px). The test budget is therefore 3 px for that full metric sweep; it also
requires integer hinted advances and exact unrounded 26.6 design metrics.
For the preserved 288-glyph stb comparison the maximum advance change is 85/64
px, bounded by 1.5 px. Keep the 3 px box/offset budget and tighten mean coverage to 25% (63.75/255),
after measuring a maximum of 61.7212/255 across all 288 samples;
measure coverage on the baseline-aligned union of ink boxes. These are explicit
migration tolerances, not claims of pixel identity with Windows.

### 2026-10-15 — F2-63 host measurements

Host only (x86_64 and i686 SDK archives), no guest or T23 evidence. The pinned
2.13.3 archives are reproducible from independent paths (host
`05138e25…af34`, i686 `3cae3f26…753b`). Per tested Liberation face the engine
holds 427,232 live bytes after open and peaks at 731,509 bytes including
interpreter, coverage cache and owned snapshot (budget 2 MiB); warm painting
allocates nothing and nothing leaks. The 288-sample comparison with the
preserved stb coverage stays within 85/64 px advance, 2 px box and a maximum
mean coverage difference of 61.72/255 (mean 20.68/255). The seven UI atlases
grow from 97,864 to 106,655 coverage bytes. Linking the engine adds 712,980 bytes
to `control`, the only `libfont` user; every other desktop program grows by
about 8 KiB (atlases), Demo by 127,480 bytes because it now links the atlases.
`libfont.a` grows from 221,622 to 1,321,514 bytes (SDK only). `stb_truetype.h`
is removed from `third_party/stb`; its notice in `/system/licenses` is replaced
by the three FreeType files (9,028 bytes).
