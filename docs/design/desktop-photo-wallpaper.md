# Desktop photo wallpaper design

Status: implemented and runtime-tested for the canonical 800×600 and 1024×768
profiles. The full-build asset generator packages the project photos, and the
desktop loading/rendering path passed the photo gate described below. This
document records the format, shell ABI, memory budget, rendering stage, and
remaining validation scope.

## Compatibility and assets

Keep the current CWC1 catalog format and its 44-byte entry records. Preserve
the selected CWP1 input set, order, file names, import, and refresh behavior:
public builds currently contain three CC0 tile patterns, while the optional
personal-artwork build contains eleven CWP1 entries. Append the three
owner-provided Ciuki photos after whichever CWP1 set was selected, titled
`Ciuk1`, `Ciuk2`, and `Ciuk3`; their source images remain
`misc/ciukios_bg/Ciuk1.png` through `Ciuk3.png`. Thus the public photos occupy
entries 4–6 and the optional eleven-tile profile uses entries 12–14.

Store the 1-based selected entry and position in the two-byte
`\SYSTEM\UI\WALL.CFG`. The first byte identifies `Ciuk1` (4 in public builds,
12 with the eleven personal tiles); the second defaults to Fill (0). The
reader also accepts legacy one-byte selections, using Fill. Missing or invalid
configuration defaults to Ciuk1. Both CWP1 and CWP2 use the prepared wallpaper
renderer described in [wallpaper-cache.md](wallpaper-cache.md).
A CWP2 selection leaves the shell's base fill in place and is painted by the
desktop module. A missing or invalid photo or unavailable XMS falls back to
the existing tile/solid wallpaper without changing the selected byte.

The photos use a distinct, deterministic CWP2 file format. The 16-byte little-
endian header is:

| Offset | Size | Field |
| --- | ---: | --- |
| 0 | 4 | ASCII `CWP2` magic/version |
| 4 | 2 | Width, 1672 |
| 6 | 2 | Height, 941 |
| 8 | 2 | Row stride, 5016 bytes |
| 10 | 1 | Pixel encoding, 1 = row-major RGB888 |
| 11 | 1 | Flags, must be zero |
| 12 | 4 | Payload bytes, 4,720,056 |

The payload is the source PNG decoded to RGB888, row-major, with no resize,
palette reduction, channel reorder, or lossy compression. Thus each complete
file is exactly 4,720,072 bytes. Validate the magic, fixed dimensions,
encoding, flags, stride, payload length, and exact file size before making it
active. Preserve the original PNG assets and record source and generated-file
hashes in the asset manifest.

`scripts/build_wallpapers.py` keeps the selected CWP1 set in its existing
order, appends `Ciuk1.png` through `Ciuk3.png` as CWP2 entries, and emits the
two-byte `WALL.CFG` default by locating the `Ciuk1` catalog title. This keeps
the default correct when the optional personal CWP1 set changes the photo
indices. `scripts/build_full.sh` always includes these three owner-provided
project assets; the CWP1 personal-artwork option remains independent. The
manifest records each source and generated SHA-256, dimensions, payload/file
sizes, and the validated exact RGB sample count.

## Runtime ownership and ABI

Display Properties hosts the catalog and selection UI. Shell service 32 returns
the active catalog entry's metadata to Desktop and Display. The packed output
record is 20 bytes:

| Offset | Size | Field |
| --- | ---: | --- |
| 0 | 2 | `bytes`, caller sets 20; shell returns 20 |
| 2 | 2 | `generation`, incremented when the selected entry changes |
| 4 | 1 | 1-based catalog index; zero means no selection |
| 5 | 1 | `kind`: 0 none, 1 CWP1 tile, 2 CWP2 photo |
| 6 | 13 | NUL-terminated DOS filename, e.g. `WALL12.CWP` |
| 19 | 1 | Position: Fill/Fit/Stretch/Center/Tile (0..4) |

The service writes only within the requesting application's segment after
validating the 20-byte destination. `app.h` exposes a typed wrapper so the
desktop never reads shell globals or parses configuration itself. The
generation lets it notice a selection change without disk reads in paint.

Keep a small `DESKTOP.APP` shim and put photo storage, loading, and rendering
in `WALLP.APP`, loaded once with the existing `webmodule` mechanism. On desktop
`EV_POLL`, the shim queries service 32. For a new wallpaper generation, `WALLP.APP`
validates the file and streams it from DOS into an XMS block using bounded
chunks. It loads only the selected photo. Build a replacement in a temporary
XMS block and swap it in only after the complete file has passed validation;
then free the previous block. A read, allocation, or format failure retains
the previously working photo. Never open files or allocate memory from
`EV_PAINT`; only the prepared cache is read there.

For a 4,720,056-byte payload, XMS rounds the active block to 4,609 KiB. Keeping
the current photo while staging a replacement requires at most 9,218 KiB of
XMS for the original pixels temporarily. At 1280×800×32, replacement can also
retain two 1280×739 native caches, bringing the four-block peak to about
17,007,472 bytes (16.22 MiB). A source scanline is 5,016 bytes; use the existing 6,146-byte
`webstore_pixels` buffer or an equivalent bounded row buffer, plus a bounded
file-transfer buffer. Do not allocate a conventional-memory photo copy. The
helper remains a single CAPP below `webmodule`'s 0xFE00-byte file and 0x1000-
paragraph allocation limits. If XMS is unavailable, use the existing tile or
solid wallpaper.

`EV_PAINT` must not open files, decode images, or allocate, release, or resize
XMS blocks. It may make bounded XMS-to-compositor reads for the already
validated cached photo, limited to rows intersecting the current compositor
band. Loading, validation, and block replacement happen outside painting.

## Compositor placement and rendering

The existing `ui_draw_scene` ordering resolves the former layering question.
For an uncovered desktop band, it paints the base and wallpaper, draws the
top bar, then calls `app_desktop_paint`; the desktop app paints its own icons
before shell windows and the bottom bar are drawn. Covered window bands skip
the desktop background entirely. Therefore invoke the photo helper at the
start of the desktop surface's `EV_PAINT`, before `top_paint()` and icon
drawing, and clip it to `x=0`, `y=29`, `width=screen_width`,
`height=screen_height-61`. The shell then draws windows, taskbar, overlays, and
cursor in their normal order. This preserves the top and bottom 32-pixel UI
bands and prevents the photo from drawing over a covered application window.

During that paint callback, use `app_band_info` (service 27) for the current
compositor band's origin, dimensions, stride, byte count, and RGB channel
layout. Its 19-byte descriptor is valid only for the active band; do not retain
its segment or pointer after the callback. `WALLP.APP` prepares a native cache
in foreground poll batches for the current dimensions, position and RGB masks.
Paint reads only intersecting rows and copies clipped spans into the current
owned band. Position choices are Fill, Fit, Stretch, Center and Tile; Fill is
the default. Preparation supports 15-, 16-, 24- and 32-bit direct color.

When `app_band_info` returns zero (the VGA16 path), use the renderer's fixed
RGB-to-16-color mapping and `ui_rect` spans/pixels, with no palette writes.
This fallback is deterministic and bounded to the clipped desktop area; it
does not reduce the photo to an indexed 256-color asset. Keep the existing
CWP1 file format intact; its indexed samples expand losslessly to the common
RGB preparation path. The older Wallpaper window remains compatible, while
desktop Properties and Control Panel open the unified Display Properties sheet.

## Primary research and decisions

The W3C [PNG Specification, Third Edition](https://www.w3.org/TR/png/)
defines truecolor pixels as RGB sample triplets when alpha is absent. The
owner-provided sources are opaque RGB PNGs at the target dimensions, so the
generator copies those decoded samples directly into the CWP2 payload without
resizing, palette conversion, or channel reordering.

The XMS 3.0 specification defines extended-memory blocks as handles managed
through an XMS driver. Function 0Bh moves data between conventional memory and
an XMS block; it does not provide a direct pointer to the full photo. The
design therefore keeps the photo in one XMS block and uses small conventional
buffers for file and row transfers. See the
[Microsoft/Lotus/Intel/AST XMS 3.0 specification](https://ps-2.kev009.com/basil.holloway/ALL%20PDF/Microsoft_XMS_3%5B1%5D.0_Specification.pdf).

The VESA VBE 3.0 specification defines the active mode pitch and channel
layout and describes the linear-framebuffer base as a physical address that
must be mapped before protected-mode access. The app therefore renders through
the compositor's active band descriptor rather than treating a framebuffer
address as an ordinary C pointer or assuming tightly packed 32-bit pixels.
See the [VBE 3.0 Core Functions specification](https://courses.cs.washington.edu/courses/cse451/21sp/readings/vbe3.pdf)
and [VESA standards catalog](https://vesa.org/vesa-standards/).

Repository evidence: `src/com/shell_gui.inc:1103` paints the wallpaper and
top bar before `app_desktop_paint`; it draws shell windows and the bottom bar
afterwards. `src/com/shell_apps.inc:1245` returns the current compositor band
through service 27. `src/apps/webstore.c` already reads RGB rows from XMS,
clips them to `app_band_info`, converts supported VBE layouts, and has a
palette-preserving VGA16 fallback. These behaviors support using the existing
desktop compositor rather than adding direct physical-framebuffer access.

## Build and validation gates

The wallpaper asset generator writes the three raw CWP2 files and appends
unchanged-size CWC1 records after the selected CWP1 entries. The full image
packages `WALLP.APP` under `\SYSTEM\APPS` and preserves existing tile
import/refresh behavior.

Remaining validation includes every supported VBE pixel layout and VGA16
fallback; repaint clipping across multiple compositor bands; full window and
taskbar/top-bar occlusion checks; and XMS allocation, swap, and release under
the full wallpaper suite. Use a copied image, record asset/module hashes and
XMS results, and rerun existing CWP1 pixel/import tests. Follow the repository's
capped full-build and sequential QEMU rules; do not use the standalone floppy
profile. Refresh and verify the portable ZIP after full-image integration,
keeping local commercial game payloads out of it.

## Photo runtime validation

The sequential QEMU photo gate passed at both 800×600 and 1024×768 using
`build/full/t23-vbe-fix/final-photo-wallpaper7/{800,1024}/result.json`.
Each run used a copied full HDD image and a freshly assembled SHELL.COM/listing;
the assembled shell bytes were compared with the installed shell, and both
hashes are recorded in each report. There were no binary overrides or guest-RAM
writes. The source image hash in both reports is
`3ed12469cd36f9e3909d0e6177b0c88362edc6d1e8c97588c3f2d4e331429c80`.

The three packaged CWP2 payloads match the owner PNGs' exact decoded RGB888
bytes. Catalog indices are 4, 5 and 6, with `Ciuk1` selected by default.
Source PNG SHA-256 values are `ab48a5b348004a2ae315dc722f16c65556164d69a00f7f319ba216cf4733fe90`
(`Ciuk1`), `e0c8a42ff797d909c3105a9762438fc6f695cbcb3b950e33373e182a58fe4794`
(`Ciuk2`) and `92bbeb879d08cfbaba3da485a2a49e09de2ef594178556bcee860c83568d544e`
(`Ciuk3`).

The harness dismisses About and closes the Programs and Wallpaper windows
before sampling unobstructed desktop areas away from icons. At 800×600, all
36 samples for each photo, restart-retained Ciuk3, malformed-entry retention,
and the CWP1 wallpaper comparison had maximum channel delta 0. At 1024×768,
maximum deltas were 5 for Ciuk1, 23 for Ciuk2 and Ciuk3 (including restart and
malformed-entry retention), and 0 for CWP1. The photo comparator permits a
neighboring source pixel for fixed-point scaling and nearest-neighbor tie
behavior; its gate allows maximum delta 48 and median 24. Truncated and
trailing-byte CWP2 entries were rejected without changing the saved selection.
This is emulator validation, not a physical-display qualification.
