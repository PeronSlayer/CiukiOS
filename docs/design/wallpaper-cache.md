# Wallpaper layout and repaint cache

The October 7 report includes a missing background at the base resolution and
lag/glitches at 1280×800 on QEMU VirtIO. The existing photo painter reads the
original RGB rows and scales/converts every pixel on every compositor pass.
An earlier CD RAM snapshot proves the selected catalog entry was returned, but
its serial log has no wallpaper-module status markers. It does not establish
whether that run failed to load the helper, accept the request, or finish the
asynchronous photo read. These paths are distinct; a successful emulator run
does not qualify the T23.

## Sources and implementation decision

* [Microsoft wallpaper positions](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/ne-shobjidl_core-desktop_wallpaper_position)
  defines Fill (preserve aspect and crop), Fit (preserve aspect with background
  bands), Stretch, Center at native size, and Tile. CiukiOS uses these five
  positions on its single desktop surface below the top bar and above the dock.
* [XMS 3.0, Microsoft/Lotus/Intel/AST](https://www.phatcode.net/res/219/files/xms30.txt)
  defines handle-based extended memory moves, their even length, and conventional
  far addresses. Original RGB pixels and prepared native pixels remain in XMS;
  every conventional scratch move is bounded and aligned. No framebuffer physical
  address is retained across a display change.
* [VESA VBE 2.0](https://www.phatcode.net/res/221/files/vbe20.pdf)
  distinguishes native color masks and scan-line pitch. The compositor's current
  owned band supplies the actual byte layout, segment and clipping boundaries.
  A cache is reused only for matching screen dimensions and native masks.
* RBIL [DOS open](https://fd.lod.bz/rbil/interrup/dos_kernel/213d.html),
  [read](https://fd.lod.bz/rbil/interrup/dos_kernel/213f.html), and
  [allocate](https://fd.lod.bz/rbil/interrup/dos_kernel/2148.html)
  define carry/error returns, read byte counts, and the largest free block on
  allocation failure. The helper loader logs the failing stage and actual DOS
  result. Desktop commits the generation only after a successful module load and
  request dispatch, and retries failures at most once every 91 BIOS ticks.

## Contract and lifetime

The wallpaper cache commit and the top-bar update may happen in the same poll.
The application's poll results are enumerated modes, not bit flags: 1 requests
the owner's whole surface, while 3 requests only an already queued rectangle.
Combining them with bitwise OR turns a required whole-desktop update into a
top-bar-only update. Desktop now gives the whole-surface request precedence.
This follows the update-region contract described by Microsoft's
[InvalidateRect](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-invalidaterect):
changed regions accumulate, and adding the whole client area must retain the
smaller damaged regions. The first 1280 VirtIO CWP1 switch exposed the old
combination: the helper completed its cache, but the screenshot retained Ciuk1.
A separate cold boot and later switch passed, demonstrating the timing-sensitive
character of the failure rather than a deterministic CWP1 decode error.

Service 32 keeps its 20-byte metadata packet; its formerly reserved final byte
is the position: 0 Fill, 1 Fit, 2 Stretch, 3 Center, 4 Tile. Desktop and Display
may query. Only Display may use command 1 to validate/apply/persist a catalog
index and position. Catalog enumeration is ordinary foreground DOS reading in
Display, outside paint. `WALL.CFG` accepts the old one-byte index (Fill) and a
two-byte index/position; other lengths or positions are rejected. The canonical
build continues to supply Ciuk1 as its default index.

WALLP loads exact CWP2 RGB rows or expands validated CWP1 indexed rows to RGB.
Loading and cache preparation happen in bounded EV_POLL batches. Preparation
scales/converts up to eight output rows per event and commits a complete native
cache. Ordinary EV_PAINT uses aligned XMS row reads and bounded far copies into
the current compositor band. It does not allocate, read DOS files, resample or
convert native pixels. Fit/Center expose the theme's desktop color as the bands;
palette changes therefore do not invalidate photo pixels. Resolution, native
format, photo or position changes prepare a replacement. Suspension frees all
handles, and a failed load preserves the previous usable pixels.

Native cached presentation currently covers screens up to 2560 pixels wide;
the existing direct-color mode list is bounded accordingly. The VGA16 fallback
retains the palette-safe renderer. The final HDD gates cover all three photos
at 640×480, 800×600, 1024×768 and 1280×800; the 1280×800 Virtio checks cover
all five styles and persisted choices, and the final mode gate covers rollback,
Keep and reboot at 640×480. These are emulator checks, not physical qualification.

## Validation

The production `wallp.c` cache/paint host fixture passes 229 cases. It checks
XMS read/write alignment and bounds, band clipping and canaries, and sampled
pixels; the host DOS/XMS/compositor services are stubs, so this is not a DOS or
graphics-device runtime test. The actual NASM `ui_text`, `ui_glyph` and
compositor-glyph ranges also pass the Unicorn clipping regression at 15-, 16-,
24- and 32-bit depths, including the retained planar 24-pixel edge guard.

The CD-only run in
[`cd-properties-verified/report.json`](../../build/full/t23-vbe-fix/cd-properties-verified/report.json)
passed on QEMU 11.1.1 with 512 MiB and no hard disk. The test verified the
shipped WALLP CAPP header and entry, default Ciuk1 catalog/config, a fresh
SHELL.COM match, and `[WALLP] ready` after startup About was dismissed. At
1024×768, 36 screenshot samples in the desktop Fill region matched the source
RGB exactly (median and maximum channel delta 0); WALLP reported ready about
20.23 seconds after QEMU start. This is emulator evidence only and does not
identify the cause of the earlier marker-free CD failure or qualify physical
hardware.

The HDD pixel, positioning, mode persistence, Appearance Cancel and drag-damage
results are recorded in
[`2026-10-07-desktop-properties.md`](../validation/2026-10-07-desktop-properties.md).
The cache commits its changed result as a full desktop invalidation even when
the same poll also updates a precise HUD rectangle; the earlier bitwise merge
could suppress that invalidation and leave the previous photo visible.
