# CiukiOS system icons

The official CiukiOS system icon family is based on **Tango Icon Theme 0.8.90**.
It provides the desktop's shared visual vocabulary for applications, files,
folders, storage and controls. Computer, About and other CiukiOS identity icons
incorporate the owner's approved portrait of **Ciuki**, the dog to whom this
operating system is dedicated.

## Source and license

- Upstream release: [Tango Icon Theme 0.8.90](https://tango.freedesktop.org/releases/).
- Original archive: [tango-icon-theme-0.8.90.tar.gz](https://tango.freedesktop.org/releases/tango-icon-theme-0.8.90.tar.gz).
- Vendored archive: `upstream/tango-icon-theme-0.8.90.tar.gz`.
- Archive SHA-256: `6e98d8032d57d818acc907ec47e6a718851ff251ae7c29aafb868743eb65c88e`.
- Upstream license statement: [COPYING](upstream/COPYING), which releases the
  icons into the **Public Domain**.
- Upstream authors: [AUTHORS](upstream/AUTHORS).

Tango's authors are Ulisse Perusin, Steven Garrity, Lapo Calamandrei, Ryan Collier,
Rodney Dawes, Andreas Nilsson, Tuomas Kuosmanen, Garrett LeSage and Jakub Steiner.
CiukiOS preserves these credits even though the upstream icon release does not
require attribution. Its Public Domain declaration applies to the upstream
icons; this document does not relabel it as a different license such as CC0.

The F2 desktop ships the complete Tango 0.8.90 PNG set as its default and only
icon style (see below). This family does not import extracted Microsoft
Windows or Apple Macintosh icons. It does not imply an endorsement by Tango's
authors.

## Ciuki identity

The portrait in [`assets/brand/ciuki-logo.png`](../brand/ciuki-logo.png) remains
the exact approved original. Its [provenance](../brand/generation.md) and
[identity requirements](../brand/README.md) are maintained separately.
Deterministic size, format and palette conversions may place this portrait
within Computer and About icons; they must preserve its shape, pose and
colours. Do not redraw it, substitute a different dog, or use a rejected logo.

The Ciuki portrait and the CiukiOS composites are separate project assets.
They are **not** covered by Tango's Public Domain declaration merely because
they are displayed alongside or composed with Tango artwork. No new license
for the portrait is granted by this document. The original photograph remains
in the boot splash.

## System icon store (F2 desktop)

`apps/desktop/convert_icons.py` converts every PNG member of the vendored
archive into one store file, `build/apps/desktop/tango.cis`, installed as
`/system/icons/tango.cis` next to `TANGO.TXT` (upstream COPYING), `AUTHORS.TXT`
and `CREDITS.TXT`. The desktop, libdeskui and every libdeskui application read
it at run time; no sprite is compiled into a binary.

- Content: 246 names (214 Tango icons of actions, apps, categories, devices,
  emblems, emotes, mimetypes, places and status, plus the 32 frames
  `process-working-00` to `-31` of `animations/process-working`), each at 16,
  22, 32 and 48 pixels; 645 archive members.
- Conversion: 16, 22 and 32 px are the archive PNGs decoded exactly (standard
  library decoder, cross-checked against Pillow; 16-bit samples keep the high
  byte); the RGB of fully transparent pixels is stored as zero. Tango 0.8.90
  has no 48x48 PNGs: 48 px entries, and any missing size, are the integer area
  filter (overlap- and alpha-weighted average, premultiplied, rounded half up)
  of the nearest PNG size, larger preferred, computed by the reader with the
  same arithmetic as the converter.
- Size: 726,504 bytes (710 KiB) for 984 images, 738 QOI-coded and 246 derived.
  Only the index (24,524 bytes) is read at start; each icon is read and decoded on first
  use and then cached, about 9 KB of memory per 48 px icon.
- Pinned: store SHA-256
  `5892ae32d3f1a121c6fb06dc0d9848e00923fe080919e72ee42fad5612bfceb4`, decoded
  pixels SHA-256 `a8a5101d4c0953ea455dacaa5cb5844925659de7ab8f7ca1744ec4c7120904e2`,
  the archive SHA-256 and every member's SHA-256, in
  `apps/desktop/tango-store.json`. `make desktop` rebuilds the store and refuses
  a different result; `python3 apps/desktop/convert_icons.py --prepare-store`
  rewrites the record after a deliberate conversion change.

File format `CIUKICS1` version 1, little-endian:

| Part | Layout |
| --- | --- |
| Header, 48 bytes | magic `CIUKICS1`; u16 version 1; u16 header size 48; u16 name count; u16 image count; u8 sizes 16, 22, 32, 48; u32 offsets of names, images and strings; u32 strings length; u32 data offset; u32 data length; u32 notice offset (into the strings) |
| Names, 16 bytes each, sorted by `strcmp` | u32 name offset; u32 context offset (strings); u16 image index per size, `0xffff` for none |
| Images, 16 bytes each | u8 size; u8 encoding; u16 source image; u32 data offset; u32 data length; u32 FNV-1a 32 of the decoded R, G, B, A bytes |
| Strings | NUL-terminated ASCII names, contexts and the Tango licence notice |
| Data | encoding 0: a [QOI 1.0](https://qoiformat.org/qoi-specification.pdf) chunk stream without its 14-byte header and end marker, straight alpha, exactly size x size pixels; encoding 1 has no data: the area filter of image `source` |

The file ends at data offset plus data length; the reader rejects any other
length, out-of-range offsets, unsorted or unterminated strings and an image
whose checksum differs (that icon only). QOI is used for its short integer-only
decoder; its specification is by Dominic Szablewski, and the CiukiOS encoder
and decoder are original implementations of it.

`apps/libdeskui/icons.c` is the reader (`du_icon(name, size)`, see
`docs/desktop.md`); `apps/libdeskui/icon_map.c` maps CiukiOS objects (programs,
menu entries, toolbar actions, places, file extensions, status, dialogs,
devices, settings, window buttons) to Tango names. The identity name `ciuki`
is not in the store: it is the pinned portrait conversion
(`/system/assets/ciuki-portrait.xrgb`, `apps/desktop/convert_portrait.py`)
reduced with the same area filter, used for Computer, About and the Ciuki
button.

`apps/desktop/shell_icons.h`, the earlier set of 43 compiled-in sprites, is no
longer included by any production source; it remains verified until the
branches that still reference it are merged.

## Legacy 0.8 runtime assets

`scripts/build_ui_icons.py` records the selected sources and produces the
renderer data in `native/`. Conversion takes place during asset preparation;
the operating system does not decode PNG or SVG at runtime. The normal build
runs `python3 scripts/build_ui_icons.py --check` to reject changed sources or
stale generated output before packaging.

`native/ICONS.DAT` contains a 16-byte header: the eight-byte magic `CIUKICO1`,
then four little-endian 16-bit fields for icon count, width, height and reserved
value. This version contains 18 icons of 32 by 32 pixels, reserved value zero,
followed by 18,432 bytes of palette indices. The resource is stored at
`\SYSTEM\UI\ICONS.DAT` on the boot volume, alongside `CREDITS.TXT`,
`TANGO.TXT` and `AUTHORS.TXT`.

The graphical installer copies the complete source FAT16 volume, including
these resources and credits. The legacy nine-entry demonstration manifest is
not the source of the graphical HDD installer's file selection.

See [CREDITS.TXT](CREDITS.TXT) for the plain ASCII attribution shipped with
CiukiOS.
