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

## Win32 font selection (f5-10C, 2026-10-19)

The `gdi32` selection layer (`sdk/win32/gdi32/font.c`) realizes `LOGFONT`
requests on the store faces and hands the f5-10B text boundary a provider
per logical font. A native face backend (`w32_gdi_set_face_backend`: open,
lookup, raster, free, resident) supplies FreeType 2.13.3 coverage through the
unchanged SDK `libfont` API (`w32_gdi_native_backend()` in
`native_font.c`). Without a backend the layer runs metric-only: metrics,
extents and layout come from the validated tables (hmtx advances rounded to
whole pixels) and no coverage is drawn. In the guest personality the backend
is the PE-to-native bridge (f5-10C2, section below). Host evidence: `tests/host/win32/dialogs` (`fonts`,
`parser`, frames).

**Store.** `C:\WINDOWS\FONTS` maps to `/System/fonts` (the twelve pinned
Liberation 2.1.5 faces, read-only). Faces installed by `AddFontResource`
are copied, after validation, into the writable install directory configured
with `w32_gdi_font_store` (default `C:\WINDOWS\FONTS`; the f5-06 drive
service must present that view as the union of the read-only system faces
and a writable per-personality directory) and registered as
`HKLM\Software\Microsoft\Windows\CurrentVersion\Fonts\"<full name> (TrueType)"`
= file name, with the original path in `HKLM\Software\CiukiOS\Win32\Fonts`.
Registered faces load again in later sessions. `RemoveFontResource` of the
last reference removes the copy and both values; while a logical font still
uses the face, removal completes when that font is deleted. `.FON`/`.FNT`
raster resources and CFF/collection files are refused (error 13, W21).
`CreateScalableFontResource` writes a CiukiOS reference file (`CIUKIFOT`
signature and the TrueType path) that `AddFontResource` accepts; Windows
NE-format `.FOT` files are not parsed (W21).

**Families.** The Windows 98 core names are virtual families backed by the
metric-compatible faces; the physical `Liberation *` families and installed
faces are enumerated too. `GetTextFace` reports the realized family name.

| Request (and substitutes) | Face | Plan |
| --- | --- | --- |
| Arial (Helvetica) | Liberation Sans R/B/I/BI | scalable |
| Times New Roman (Times) | Liberation Serif R/B/I/BI | scalable |
| Courier New (Courier 10 Pitch) | Liberation Mono R/B/I/BI | scalable |
| MS Sans Serif (Helv, MS Shell Dlg), Tahoma (MS Shell Dlg 2), Small Fonts | Liberation Sans | raster-size rule |
| MS Serif (Tms Rmn) | Liberation Serif | raster-size rule |
| Courier | Liberation Mono | raster-size rule |
| System | Liberation Sans Bold | 13 px em, 16 px cell |
| Fixedsys | Liberation Mono | 13 px em, 15 px cell |
| Terminal (OEM_CHARSET) | Liberation Mono | 10 px em, 12 px cell |

Substitution order: WIN.INI `[FontSubstitutes]` (the Windows 98 location,
through `GetProfileString`), then
`HKLM\Software\Microsoft\Windows NT\CurrentVersion\FontSubstitutes`, then the
built-in list above. **Raster-size rule:** a positive (cell) height selects
the largest integer em whose cell is not taller than the request, as the
mapper treats bitmap sizes; a negative height is the em. Stock objects:
`SYSTEM_FONT`/`DEVICE_DEFAULT_FONT` System 16, `DEFAULT_GUI_FONT` MS Sans
Serif -11, `ANSI_VAR_FONT` MS Sans Serif 13, `ANSI_FIXED_FONT` Courier 13,
`SYSTEM_FIXED_FONT` Fixedsys 15, `OEM_FIXED_FONT` Terminal 12.

**Windows 98 dialog-font plan (host-measured, FreeType 2.13.3 v35).** Dialog
base units follow Windows: horizontal = (extent of `A`–`Z``a`–`z` / 26 + 1)
/ 2, vertical = `tmHeight`; `MapDialogRect` uses `MulDiv(x, base, 4)` and
`MulDiv(y, base, 8)`. The directive's references are 8x16 for the System font
and 6x13 for MS Sans Serif 8 pt at 96 dpi.

| Request | Realized | Cell (ascent+descent) | Internal leading | Avg width | Base units |
| --- | --- | --- | --- | --- | --- |
| System (stock) | Liberation Sans Bold, 13 px | 16 (13+3, pinned) | 3 | — | 8x16 |
| MS Sans Serif 8 pt (-11 or 13) | Liberation Sans, 11 px | 13 (10+3) | 2 | 6 | 6x13 |
| MS Shell Dlg / Helv 8 pt | as MS Sans Serif | 13 | 2 | 6 | 6x13 |
| Tahoma 8 pt | Liberation Sans, 11 px | 13 | 2 | 6 | 6x13 |

Only the System cell is pinned (natural 15 → 16, ascent raised to 13); the
8 pt dialog sizes are natural. With these units the dialog geometry equals the
Windows 98 references exactly (0 px per 100 units; `MapDialogRect` of 100x100
gives 150x163). Glyph shapes and other sizes are FreeType/Liberation, not
Microsoft raster fonts.

**Matching.** Each candidate (virtual family styles first, then physical
faces, in store order) receives the published Windows font-mapper penalties
(R. Gery, [Windows Font Mapping](https://learn.microsoft.com/en-us/previous-versions/ms969909(v=msdn.10)),
MSDN 1992, table 1): CharSet 65000, FixedPitch 15000, FaceName 10000, Family
9000, FamilyUnknown 8000, FaceNameSubst 500, PitchVariable 350,
FamilyUnlikely 50, Italic 4, Weight 3 per 10 units, ItalicSim 1,
DefaultPitchFixed 1; the lowest total wins and ties keep the first
candidate. The table documents Windows 3.0/3.1; Windows 98 is understood to
retain the core mapper, but no Windows 98-specific weight table was found, so
this remains a documented approximation. TrueType candidates carry no height,
width or aspect penalties (as in the source). `DEFAULT_CHARSET` matches the
ANSI faces (code page 1252 system). A full face name such as "Arial Bold"
matches that style.

**Metrics.** Ascent/descent are the OS/2 `usWin` values scaled to 26.6
(rounded, like the rasteriser's size metrics) and rounded outward to whole
pixels; a positive height fixes `tmHeight` to the request. External leading
uses the TrueType Windows formula max(0, lineGap − ((usWinAscent +
usWinDescent) − (ascender − descender))). `tmAveCharWidth` is OS/2
`xAvgCharWidth`, `tmMaxCharWidth` hhea `advanceWidthMax`; ANSI fonts report
`tmFirstChar` 30, `tmDefaultChar` 31, `tmBreakChar` 32. Advances and ABC
widths are the hinted FreeType advances. Text output applies no pair kerning
(GDI does not); `GetKerningPairs` reports the legacy `kern` table (Liberation
Sans/Serif have one, Mono none). Synthesis only for missing styles:
emboldening (+1 px coverage and advance), italic shear (21/64), underline
and strike-out bars from `post`/`OS/2`, and a threshold for
`NONANTIALIASED_QUALITY`. `w32_gdi_glyph.top` is the distance above the
baseline; `libfont` reports a y-down offset, which the backend negates.

**Budget (≤ 2 MiB shared).** Open faces (backend-reported resident bytes:
file, a 64 KiB face allowance and the SDK cache trimmed to 64 KiB) are an LRU
within 2 MiB minus the 256 KiB glyph cache (LRU, 1,024 entries, coverage after
synthesis) and the store records. Host measurement over 45 logical fonts (three
families, 8–36 px): total peak 1,837,350 bytes, faces 1,513,810 peak with 3
open, glyph cache 223,162 peak, 4,826 glyph and 9 face evictions.

**Parser bounds.** The table directory and every required table (`head`,
`hhea`, `maxp`, `hmtx`, `cmap` format 4, `name`, `post`, optional `OS/2`,
`kern` format 0) are range-checked before use; `glyf`/`loca` must exist; CFF
(`OTTO`), collections and NE files are refused. Host fuzzing truncates at
every offset up to the last table end, makes table lengths and offsets
overlong and flips bytes in 3,000 rounds without a fault.

**Limitations (W21 or open).** No rotated output for `lfEscapement`/
`lfOrientation`; `lfWidth` does not scale glyphs; heights are realized at
creation in `MM_TEXT` units; em sizes above 128 px clamp; no symbol faces
(Symbol/Wingdings requests fall back by penalty); OEM text uses the code-page
1252 glyph mapping; `GetGlyphOutline` serves `GGO_METRICS`, `GGO_BITMAP` and
`GGO_GRAY*_BITMAP` with the identity matrix only (`GGO_NATIVE`/`GGO_BEZIER`
return `GDI_ERROR`); `GetFontData` reads the store file per call.

## PE-to-native font bridge (f5-10C2, 2026-10-20)

The guest gdi32 is a freestanding PE; libfont and FreeType are native ELF
code. The bridge carries rasterised glyphs from the native engine to the
selection layer above without mapping any font file into a Win32 process.

**Where glyphs are rasterised.** `w32srv` (the existing Win32 service,
`W32_SERVICE_W32SRV`) links libfont (`apps/w32srv/fonts.c`, `-lfont`) and
opens each store face once for every Win32 process, with the same pinned
FreeType 2.13.3 configuration (interpreter v35, grey coverage,
`FT_LOAD_TARGET_NORMAL`). No new system call: requests use the existing
serialized transport (`w32_server_exchange` in ntdll, 256-byte stop-and-wait
fragments over the w32srv channel). gdi32 installs the client
(`sdk/win32/gdi32/bridge.c`) as its default face backend in the guest build;
host builds keep choosing their backend explicitly.

**Protocol** (`w32/server.h`): `W32R_FONT_CMAP` returns the 256 glyph indices
of one code-point page (`ciuki_font_lookup`, 0 = missing glyph, as the
[cmap](https://learn.microsoft.com/en-us/typography/opentype/spec/cmap)
defines glyph 0); `W32R_FONT_GLYPHS` returns up to 64 glyphs at one em size
(26.6 pixels, 64..8192), each a 16-byte record (index, width, height, left,
top, 26.6 hinted advance) followed by its 8-bit coverage rows, as many as fit
`W32R_DATA` (64 KiB); `W32R_FONT_STATS` returns the service counters. A face
is named by its native path, accepted only as a plain `.ttf` file name
directly inside the system store (`/System/fonts`) or the install store
(`/WINDOWS/FONTS`). The client asks for one glyph per miss (one BEGIN and one
or two READ fragments for user-interface sizes, measured on the host), keeps
four code-point pages per face, and retries a reply larger than its 4 KiB
stack buffer once with a 64 KiB heap buffer (large sizes only).

**Orientation.** `w32_gdi_glyph.top`, the wire `top` and GetGlyphOutline's
`gmptGlyphOrigin.y` are the distance from the baseline up to the first
coverage row: Microsoft
[GLYPHMETRICS](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-glyphmetrics)
(upper left corner of the black box, relative to the cell origin on the
baseline, per [GetGlyphOutline](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-getglyphoutlinea)),
FreeType `bitmap_top` ("the distance from the baseline to the top-most glyph
scanline, upwards y coordinates being positive",
[glyph retrieval](https://freetype.org/freetype2/docs/reference/ft2-glyph_retrieval.html)).
libfont stores `-bitmap_top` (y down), so every reader negates it: the
service, the f5-10C backend and, since f5-10C2, the f5-10B provider
`w32_gdi_native_font_open` (its ten text frames are re-pinned; a pinned test
checks glyph rows against the `glyf` yMin/yMax box). TEXTMETRIC ascent and
descent stay "units above/below the base line"
([TEXTMETRIC](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-textmetrica));
ABC widths keep A + B + C = advance
([ABC](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-abc)).

**Validation and refusals (W21, typed).** Before FreeType sees a file the
service checks the sfnt table directory
([font file organization](https://learn.microsoft.com/en-us/typography/opentype/spec/otff)):
version 0x00010000 or `true`, 1..64 table records inside the file, `head`
(magic 0x5F0F3CF5, unitsPerEm 16..16384), `hhea`, `maxp`, `hmtx`, `cmap`,
`glyf`, `loca`; files above 16 MiB are refused. Refusals: 13 for CFF
(`OTTO`), collections (`ttcf`) and damaged directories (remembered per path),
50 for em sizes outside 1..128 pixels, 8 for a face larger than the service
budget, 5 for a path outside the stores, 2 for a missing file, 87 for a
malformed request, 120 when the engine is absent. A refused face leaves its
logical fonts metric-only; gdi32 asks again only after 256 further requests
for that face.

**Budget.** The faces open in the service are an LRU inside 1,572,864 bytes
(`WF_BUDGET`): the shared 2 MiB minus 512 KiB for what the per-face accounting
does not see, i.e. the linked engine text (w32srv `text` grows from 106,300 to
290,488 bytes: 184,188 bytes) and FreeType's transient interpreter and
snapshot peak above the accounted face (F2-63: 731,509-byte peak against at
most 541,784 accounted bytes for Liberation Sans). A face accounts its file,
a 64 KiB allowance and the libfont cache, trimmed to 64 KiB after every
request. Per process, gdi32 keeps its 256 KiB glyph LRU and accounts only the
client (2,364 bytes per open face); the f5-10C 2 MiB envelope therefore holds
per process and the faces are counted once, system-wide.

**Host evidence** (`tests/host/win32/dialogs`, modes `bridge`,
`frames-bridge`, `exercise`): glyphs through the bridge are byte-identical to
the direct backend for seven fonts (dialog, System, bold italic, 24 px serif,
mono, 96 px); the 22 pinned f5-10C frames rendered through the bridge keep
their SHA-256; cache hits cross no request; service LRU evictions inside a
700 KiB budget; the typed refusals above; the w32-dialogs fixture's run
passes the suite predicates through the runner's evidence parser. Host
figures do not establish guest latency: a glyph miss costs one w32srv round
trip of two or three fragments, and w32srv now yields instead of sleeping
10 ms while a transaction is in flight (20 ms window).

## fx-28 built-in atlas coverage

`apps/desktop/font_codes.h` is the single atlas-index definition shared by the
converter, compositor and toolkit (including the textarea). Existing indices
remain unchanged. Additions cover hyphens, en/em dashes, curly quotes, chevron,
euro, four arrows and minus; Latin-1 already covers degree, multiplication and
copyright. The pinned font sources and licences are unchanged. The system-string
audit parses all apps C string literals, including UTF-8 hex/octal escapes; control
characters and byte-order signatures are not UI glyphs. Converter checks reject
missing additional glyphs, with Inconsolata U+2011 using ordinary hyphen ink.
