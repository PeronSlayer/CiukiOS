# System icon preparation

Tango 0.8.90 remains the default family. The original converter, archive,
`tango-store.json` and pixel oracle remain intact. `system_icons.py` supplies
selected Haiku application subjects absent from Tango to the same CIUKICS1
packer. The native reader and public interfaces are unchanged. It installs
as `/system/icons/tango.cis`, retaining the existing path and lazy loading.

Research and source selection:

- [Tango upstream release](https://tango.freedesktop.org/releases/): the pinned
  0.8.90 COPYING releases its artwork into the Public Domain. Existing Tango
  file, folder, drive, device, settings, tray, notification and action icons
  remain suitable and are retained.
- [Haiku exports](https://github.com/darealshinji/haiku-icons) identify the
  original artwork commit `49f62bc1728ce870478ae4bdb82201f0ee0c204b` and warn
  that some exports may be incorrect. [Their license](https://github.com/darealshinji/haiku-icons/blob/master/LICENSE)
  is MIT, copyright Haiku, Inc. Preserve the exact LICENSE and README beside
  the pinned archive and ship them with the generated artwork. Select no
  Haiku logo, leaf or unrelated mascot. Native 16/32 PNG exports are decoded
  exactly; 22/48 use the existing integer alpha-weighted area filter on the
  64 px export. The eight selected subjects have all three upstream sizes.
- [Oxygen upstream](https://invent.kde.org/frameworks/oxygen-icons): the supplied
  6.0.0 COPYING specifies LGPL-3.0-or-later and explains that SVG, where
  available, is the corresponding artwork source. COPYING.LIB alone is an
  older LGPL-2.1 text and must not be treated as the icon license. No Oxygen
  asset is selected: suitable Tango assets already cover files and peripherals,
  and Haiku covers the selected application gaps. Do not introduce a second
  supplemental desktop family or its large archive without an actual gap.
- [Tabler license](https://github.com/tabler/tabler-icons/blob/main/LICENSE)
  (Paweł Kuna) and [Pixelarticons license](https://github.com/halfmage/pixelarticons/blob/master/LICENSE)
  (Gerrit Halfmann) are MIT. Neither is needed for the existing Tango action
  and status vocabulary. No new SVG rasterizer or runtime dependency is needed.
- [W3C PNG specification](https://www.w3.org/TR/png/): preparation writes
  non-interlaced RGBA8 PNG with CRC-checked chunks. The native OS continues
  to use the existing decoded store, without adding SVG/PNG parsing to the
  icon reader. The original converter's PNG decoder checks the upstream
  samples; independent Pillow comparisons remain in the existing host suite.

The research used the supplied archives for implementation; no artwork was
downloaded. Oxygen's upstream web endpoint returned HTTP 403, so the pinned
local release's COPYING, AUTHORS and README were inspected directly.

`assets/icons/inventory.json` enumerates object mappings, explicit catalogue
and drawing references, all 19 Control Panel applets, and the busy animation.
It records prior stand-ins, intentional generic fallbacks and missing native
source sizes (as distinct from available derived runtime sizes). Unknown
file/program fallbacks remain intentional. Solitaire and FreeCell share
Tango's games category rather than using a face as FreeCell's icon.
External application's own embedded artwork and user-provided shortcut
artwork are outside this system asset catalogue.

`system-store.json` pins the combined store and every source member hash.
The build emits PNG at 16/32/48 for inventory-referenced names, plus
`MANIFEST.JSON` and `INVENTORY.JSON`. Every store name has a manifest row;
unused names from the existing complete Tango API remain available in the
store but do not add PNG files. Only the eight selected Haiku subjects enter
the store. Source archives stay on the host; license notices enter the image.
`build_desktop.py` includes all outputs and source archives in its provenance;
`scripts/build_image.py` stages its declared `ICON_FILES` into `/system/icons`.

Ciuki remains the exact approved portrait. The existing 256 px XRGB conversion
and source PNG hash are checked before PNG export. The standalone portrait
uses libdeskui's area filter; `ciuki-computer` uses the existing shell's
portrait badge arithmetic over Tango's computer. These are deterministic
conversions, not new artwork. The manifest explicitly separates both project
assets from Public Domain/MIT upstream artwork; it grants no portrait license.
Desktop/Files/About identity and the boot splash are preserved.

Prepare deliberate catalogue/source changes with
`nice -n 19 python3 apps/desktop/system_icons.py --prepare`; stage assets alone
with `nice -n 19 python3 apps/desktop/system_icons.py --stage build/host/system-icons`.
Host tests are `tests.host.desktop.test_system_icons` and
`tests.host.desktop.test_icons`, followed by desktop, Files, Control Panel
and image-builder regressions. Visual golden changes require lead review;
do not silently re-pin unrelated render expectations.
