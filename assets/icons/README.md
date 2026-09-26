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

Only selected generic Tango artwork is used. This family does not import
extracted Microsoft Windows or Apple Macintosh icons. It does not imply an
endorsement by Tango's authors.

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

## Reproducible runtime assets

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
