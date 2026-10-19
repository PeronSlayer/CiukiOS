# F5-10B GDI engine and integration boundary

Research record, 2026-10-19, before implementation. The existing builtin store
contains a GDI placeholder. Protocol v3 retains client-owned surfaces; the
presenter describes positive pitches, RGB565/1555, XRGB8888 and indexed RGB
tables. The SDK font API already supplies owned grey coverage and 26.6 metrics.

Primary references consulted:

- Microsoft Windows 98 Resource Kit, local `build/rskt/wrkc24_3.htm` in the
  main checkout, section “Display Minidrivers”; archived
  [chapter 24](https://library.thedatadungeon.com/msdn-2000-04/rk98book/html/wrkc24_3.content.htm).
  Its 32-bit DIB engine handles memory drawing and unaccelerated display work.
- Microsoft period Platform SDK [SetStretchBltMode](https://techshelps.github.io/MSDN/GDI/devdoc/live/pdgrmm/bitmaps_6cth.htm),
  and the Microsoft DDK [StretchBlt](https://dos-help.soulsphere.org/ddag31qh.hlp/StretchBlt.html)
  (Windows 3.1, lineage only; not labelled a retrieved Windows 98 DDK).
- Microsoft [ternary ROPs](https://learn.microsoft.com/en-us/windows/win32/gdi/ternary-raster-operations),
  [DIB headers](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-bitmapinfoheader),
  [DIB sections](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-createdibsection),
  [mapping modes](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-setmapmode),
  [system palette](https://learn.microsoft.com/en-us/windows/win32/gdi/system-palette),
  [ExtTextOut](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-exttextouta),
  [DrawText](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-drawtexta).
- FreeType [interpreter properties](https://freetype.org/freetype2/docs/reference/ft2-properties.html)
  and the pinned SDK implementation/`fonts-contract.md`: consume its v35 grey
  coverage without changing the backend or adding a second rasteriser.

Decisions: use a scalar, integer-only, pitch-aware software engine. Evaluate
ROP3's truth table at index `(P<<2)|(S<<1)|D` on destination-format bits after
source conversion. Ignore the low ROP encoding word. Preserve unused DIB bits
on pixel writes. Snapshot overlapping source storage before a transfer.
Rectangles are half open. Clipping changes destination coverage, never source
sampling phase. COLORONCOLOR chooses source `floor(i*source/destination)` for
each destination coordinate, with signed extents controlling reflection.
AND/OR reduction includes eliminated source samples; HALFTONE uses rational
area coverage. These are explicitly pinned software rules: the period SDK
describes deletion/aggregation but does not specify every fractional tie.
Host reference frames do not establish Windows 98 pixel parity or driver-
specific HALFTONE support. No Wine implementation is used.

The private interface is `sdk/win32/include/w32/gdi32.h`. USER32 binds borrowed
storage through `w32_gdi_surface_dc`, provides a copied visible region and a
damage callback, and retains the backing until DeleteDC. It can replace a
resized backing atomically with `w32_gdi_bind_surface`. Shape publication uses
`w32_gdi_window_shape` for the drawing clip and `GetRegionData` for the shape;
USER32 retains ownership of window regions and
the protocol request. Drawing never publishes a surface or owns a window.

`origin` is the child DC origin in the backing buffer. It is independent of
the mapping state's viewport origin. `LPtoDP`/`DPtoLP` expose DC-relative device
coordinates; painting adds the buffer origin. USER32's visible region and
damage callbacks use buffer coordinates; application clip regions use
DC-relative device coordinates. `w32_gdi_set_screen_origin` supplies the absolute
screen position for `GetDCOrgEx`. All-zero masks at the surface boundary select
the standard BI_RGB masks (555 at 16 bpp, BGR at 24/32 bpp); partially specified
or overlapping masks fail validation. The raw DIB engine requires explicit masks.
The f5-10A `user32/gdi_bridge.c` record/signatures were inspected read-only:
its borrowed XRGB8888 buffer and child-relative origin match this boundary.
The lead still integrates absolute screen-origin updates and window publication.

Font selection creates an HFONT through `w32_gdi_create_font` with a provider
and opaque face. Providers return owned coverage, release it after use, expose
26.6 ascent/descent/line gap/advance, and retain/release the face for object
lifetime. A native adapter consumes the unchanged `ciuki/font.h` API. The PE
personality must install its native-font bridge; an absent provider fails text
operations instead of drawing fabricated glyphs. f5-10C owns names, LOGFONT,
enumeration, metrics exports and resource APIs. USER32 owns DrawText/DrawIcon
exports and calls the GDI helpers. Palette notifications use a registered
callback; USER32 dispatches WM_QUERYNEWPALETTE/WM_PALETTECHANGED.

The optional font `get_object` callback supplies LOGFONT(A/W) metadata to the
common GetObject exports. The native raster adapter does not invent LOGFONT
selection metadata. A provider is copied at font creation; code and face storage
must remain valid until its release callback. The native adapter owns its SDK
face and balances retain/release on DeleteObject or process detach.

Limits: 4095 generational slots including stock slots (4074 dynamic objects),
8192×8192 buffers, coordinates ±32767, 32768 region rectangles, 8192 text input
units and polygon vertices, 1024 saved DC states, 256-pixel cosmetic pen widths.
Allocation failures return failure. Selected/saved objects cannot be deleted;
saved bitmap ownership is tracked per DC, so selecting it again in that DC is
allowed while selection in another DC is refused. DLL detach releases all
dynamic objects and saved-state allocations. The surface byte counter reports
bitmap storage owned/mapped by GDI; USER32 accounts for borrowed window storage.

ExtCreateRegion decodes IEEE binary32 XFORM fields into 16.16 integers, applies
the affine transform to each rectangle and combines the resulting polygons;
nonfinite/out-of-range values fail. Strokes use cosmetic raster masks, Beziers
use 32 fixed subdivisions per cubic, and Arc uses 128 rational circle samples.
These raster rules and the four software stretch modes are deterministic host
implementations, not measured Windows 98 raster equivalence. Non-Latin shaping,
printing and metafile playback remain outside this drawing directive; census
exports for the latter two have explicit W21 refusals.

Host entry point: `nice -n 19 python3 -B -m unittest tests.host.win32.test_gdi32`.
The frame manifest under `tests/host/win32/gdi32/frames/` pins 569 raw frames:
256 ROPs at both 16/32-bit destinations, every source depth (both 555/565),
both DIB orientations, four stretch modes at three ratios, five region modes,
palette realisation/animation and ten text alignment combinations. Independent
Python Boolean, pixel-conversion, area-filter and region-grid oracles check
the bytes before digest comparison. Text uses the actual pinned FreeType
backend and Liberation Sans. Set `CIUKI_GDI_PIN_FRAMES=1` only for an intentional
reviewed raster change; normal tests cannot rewrite pins. The PE builder uses
the kernel's disassembly classifier to refuse any x87/MMX/SSE instructions.

Palette colour qualification: Microsoft [PALETTEINDEX](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-paletteindex)
and [PALETTERGB](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-palettergb)
were inspected before correcting primitive colour conversion. Resolve logical
indices through the selected palette; match palette-relative RGB on palette
devices and use ordinary RGB on true-colour devices. Preserve the realised
physical index on palette-controlled surfaces, including brush ROPs and fully
covered text pixels, so PC_RESERVED/PC_NOCOLLAPSE duplicates remain distinct
under animation. DIB sections with their own colour tables quantise against
those tables instead of replacing their indices with system-palette indices.
