# Display appearance and DPI (G23)

This contract covers DS-2, DS-5, DS-6, DS-9, DS-10, FT-4 and FT-5 in
`os-feature-inventory.md`. The approved Ciuki portrait, original splash photo,
English copy, tagline and Tango icon family remain project requirements.

## Windows 98 reference and sources

The local Windows 98 Resource Kit describes Display Properties as the place
for background, Appearance schemes and scalable interface elements. Appearance
customizes the selected element's colours, font and dimensions. Advanced
provides small, large and custom font sizes; the Resource Kit requires a
restart for a font-size change. Desktop Themes groups visual and audio choices.

Primary sources read for this implementation:

- Owner's read-only Resource Kit extraction:
  `/home/peronslayer/Desktop/CiukiOS/build/rskt/wrkc23_2.htm` (Display),
  `wrkapph_3.htm` (schemes, fonts and scalable elements), and `wrkc24_3.htm`
  (Advanced font size and monitor settings), plus `wrkc02_3.htm` (Desktop
  Themes components). No Resource Kit payload is copied.
- [Microsoft SystemParametersInfo](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-systemparametersinfoa): system parameters, Effects, icon and nonclient metrics, setter arguments and notifications.
- [Microsoft NONCLIENTMETRICSA](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-nonclientmetricsa): caption/menu/scrollbar dimensions and the five nonclient fonts. CiukiOS uses the 340-byte Windows 98 record, without Vista's padded border.
- [Microsoft GetSysColor](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getsyscolor): element colours, including active/inactive caption gradient endpoints; COLORREF uses BGR byte order.
- [Microsoft GetSystemMetrics](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getsystemmetrics): dimensions are physical pixels; icon spacing is the cell size.
- [Microsoft GetDeviceCaps](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-getdevicecaps): LOGPIXELSX/Y and VREFRESH. A refresh value of zero represents the hardware default.
- [Microsoft DEVMODEA](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-devmodea), [ChangeDisplaySettingsA](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-changedisplaysettingsa), and [monitor class driver](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/monitor-class-function-driver): driver mode enumeration and monitor-supplied EDID must not be confused with active scanout timing.
- [Upstream EDK2 VBE/DDC declarations](https://android.googlesource.com/device/linaro/bootloader/edk2/+/cb9fcac/DuetPkg/BiosVideoThunkDxe/VesaBiosExtensions.h): INT 10h AX=4F15h, BL=0 for capabilities and BL=1/CX=DX=0/ES:DI for the 128-byte base EDID block.

## Shared appearance value

`apps/desktop/appearance.h` is an internal, allocation-free model shared with
USER32 and GDI. It contains DPI, four Effects bits and 20 element records.
Each element has two background colours, a text colour, logical size, pinned
raster font face, point size and bold/italic flags. Families are the existing
Inter and Inconsolata raster faces; user fonts remain viewable in the Fonts
applet. No new font or identity artwork is generated.

The active value is a validated hexadecimal `appearance=` line in the existing
`desktop.cfg`. Invalid values retain the preceding/default value. Numeric
limits prevent oversized frames and invalid font indexes. Six built-ins are
Windows Standard, High Contrast Black, High Contrast White, Desert, Rainy Day
and Ciuki Blue. Eight user schemes support Save As (replace the same user
name), Delete and selection. Built-ins cannot be overwritten or deleted.
The existing Platinum/Ciuki Blue theme key continues to select the base skin.
Old files retain the original default look until an appearance is edited.

The editor previews an item and edits its colours, size, font, point size and
styles. Desktop title gradients, frame geometry, menus, message boxes, icon
labels/spacing, scrollbar width, selection and tooltips consume that value.
USER32 translates the same fields for GetSysColor, GetSystemMetrics and
SPI_GETNONCLIENTMETRICS/ICONMETRICS. When a process observes a changed appearance, USER queues WM_SYSCOLORCHANGE
and WM_SETTINGCHANGE to its top-level windows and invalidates their clients.
Effects queries reflect the shared value;
shared setters merge only appearance keys into a flushed temporary settings
file and publish through the existing native rename syscall. This avoids
claiming support for KERNEL32 MoveFileEx replacement flags, which are currently
unsupported. Failed writes/flushes/renames retain the previous effective value.
The existing non-appearance SPI compatibility paths remain available.

Every settings change invalidates the full scene and releases cached caption
rasters, even when the scheme name or mutable theme pointer is unchanged.
Desktop asynchronous polling and explicit reload both compare appearance,
icon/pointer choices and desktop colour. Unrelated settings are preserved.
Display Apply/OK/Cancel retain the existing durable preview transaction.

## Effects and DPI

Effects controls select large icons, contents versus a deferred outline while
moving a window, binary versus grey font coverage, and a 120 ms menu reveal.
The outline damages its old/new bounds, is excluded from window copies and is
committed only on release; Escape cancels it. Every menu reveal step damages
the menu. Native bitmap/FreeType text and Win32 GDI use the smoothing choice.
Documents and Recycle Bin pictures can select existing Tango alternatives.
Computer and About continue to use deterministic conversions of the exact
approved Ciuki portrait.

Font DPI accepts 96 (Small), 120 (Large) and custom values from 48 through 240.
Settings > Advanced opens a General property sheet with these choices and
OK, Cancel and Apply; Cancel restores its entry snapshot, while the outer
Display transaction still owns durable preview rollback.
Desktop font advances, glyph painting, frames, bars, task buttons, tray and Start geometry, icon dimensions and icon cells
use a common integer scale. Toolkit windows keep a logical canvas and create a
physical surface at the selected DPI; the final presentation scales the entire
canvas, including controls, icons and text. Pointer/drop coordinates use the
same pixel-centre mapping into logical coordinates. This preserves existing
application layouts and hit targets. Resize rollback remembers the old logical
canvas dimensions, including odd dimensions at 50 percent. New windows use
the selected DPI; existing toolkit windows retain their surface scale until
reopened, consistent with the Windows 98 restart requirement. GDI exposes the
selected logical DPI through GetDeviceCaps. Native icon sizes above the stored
48-pixel slots use deterministic conversions, including the exact approved
portrait, in a bounded one-MiB optional cache.

## Desktop Theme packages

Four named user packages capture appearance, base skin, wallpaper/placement and
colour, sound event files and volume, pointer scheme, screen saver policy,
Tango icon picture choices and large-icon settings. The Themes page applies
built-ins or saved packages and supports Save As/Delete. Settings serialization
uses separate bounded keys for each member rather than a host struct dump.
Sound settings travel through the existing sound provider; Cancel restores the
entry sound snapshot as well as display preferences. Pointers select the
existing raster, a bounded deterministic enlargement, or inverted mask/image
colours. Package selection and its name persist. External Microsoft `.theme`
file import/export is not implemented or claimed by this native package format.

## Monitor and refresh ABI

The existing loader (`src/boot/ciukldr/video.inc`, `.edid`) already performs
VBE/DDC block-zero acquisition, validates its header/checksum and retains the
copied bytes in boot info. G23 reuses that acquisition and revalidates the block
before publishing it. No firmware pointer is exposed or dereferenced by users.

Existing 12-byte mode and 612-byte display-info records remain unchanged.
Additive syscall-74 operations are MONITOR=4 and REFRESH=5, after DPMS=3 on
main. Display-grant devctl requests are `DISPLAY_GET_MONITOR=0x53540005` and
`DISPLAY_GET_REFRESH=0x53540006`, after main's highest allocated request
`CIUKI_SETUP_NET_QUERY=0x53540004`. Devctl validates the display grant and exact
payload length; syscall queries copy only their input headers.

The new fixed-width records are:

- `ciuki_monitor_timing`: 28 bytes, detailed timing dimensions/totals, pixel
  clock, millihertz rate and preferred/interlaced flags.
- `ciuki_monitor_info`: 316 bytes, validity/source flags, manufacturer/product/
  serial/name, manufacture date, vertical limits, four detailed timings and the
  original 128-byte EDID.
- `ciuki_display_refresh`: 92 bytes, requested mode, dimensions, known active
  rate (zero when unknown) and matching monitor timing rates.

The EDID decoder accepts checksum-valid EDID 1.0–1.4 base blocks, bounded ASCII
names, range descriptors and four detailed timings. It calculates refresh from
pixel clock and totals, accounting for interlaced field timing. Duplicate
matching rates are removed. These are monitor capabilities, not a promise that
the current VBE mode uses them. The Settings page labels the monitor and EDID
rate separately from the firmware-default scanout rate. GDI no longer invents
60 Hz. Arbitrary refresh programming needs a qualified VBE3 CRTC/native driver
path; this change does not claim one. Adapter/driver replacement and an
acceleration slider also remain separate gaps in DS-9.

Every new ABI field and constant has a literal layout-oracle row. Public SDK
display headers stay identical, and `gen_api_reference.py` regenerates the API
reference. Host verification and environment limitations are recorded in
`build/codex/g23-report.md`; this work does not provide QEMU or hardware evidence.
